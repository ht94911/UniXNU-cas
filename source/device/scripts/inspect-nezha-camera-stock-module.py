#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inspect local nezha Parameter Parser 6.0.7 sensor modules."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


MAGIC = b"QTI Chromatix Header"
POWER_NAMES = ("MCLK", "VANA", "VDIG", "VIO", "VAF", "VAF_PWDM", "CUSTOM_REG1", "CUSTOM_REG2",
               "RESET", "STANDBY", "CUSTOM_GPIO1", "CUSTOM_GPIO2", "VANA1", "CUSTOM_REG3")
OPERATIONS = ("WRITE", "WRITE_BURST", "WRITE_SEQUENTIAL", "READ", "POLL")


def require(value, description):
    if not value:
        raise ValueError(description)


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


class StockModule:
    def __init__(self, path):
        binary = path.read_bytes()
        self.sha256 = hashlib.sha256(binary).hexdigest()
        require(len(binary) >= 0xd8 and binary.startswith(MAGIC) and u32(binary, 0x98) == 5,
                "Unexpected module header")
        require(binary[0x28:0x40].startswith(b"Parameter Parser V6.0.7") and
                u32(binary, 0x1c) == len(binary) - 8, "Unexpected parser version or module length")
        self.name = binary[0x58:0x98].split(b"\0", 1)[0].decode("ascii")
        require(self.name == path.stem and self.name.startswith("com.qti.sensormodule.nezha_"),
                "Module name does not match stock nezha file")
        sections = list(struct.iter_unpack("<III", binary[0x9c:0xd8]))
        require(sections[0] == (0, 0xa8, 4) and [x[0] for x in sections[1:]] == [0, 3, 2, 1],
                "Unexpected section types")
        end = 0xd8
        for _, offset, length in sections[1:]:
            require(offset == end and length > 0, "Section offsets overlap or leave a gap")
            end += length
        require(end == len(binary) - 8, "Section data extends beyond the declared module")
        _, start, size = sections[2]
        self.symbols = list(struct.iter_unpack("<III", binary[start:start + size]))
        _, start, size = sections[4]
        self.data = memoryview(binary)[start:start + size]
        for offset, length, alias in self.symbols:
            require(offset <= size and length <= size - offset and alias < len(self.symbols),
                    "Symbol points outside its module")

    def symbol(self, index):
        require(0 <= index < len(self.symbols), "Symbol index is outside its module")
        offset, size, _ = self.symbols[index]
        return self.data[offset:offset + size]

    def board(self):
        # This generated version stores SlaveInformation at this position
        # in all six stock camera module variants. The full word and mask
        # are validated before an identity is emitted.
        slave, addr_size, data_size, reg, expected, mask, speed = struct.unpack_from("<7I", self.data, 0x6c)
        require(slave % 2 == 0 and 0x10 <= slave <= 0xee and addr_size == 2 and data_size == 2 and
                reg <= 0xffff and expected <= 0xffff and mask in (0xffff, 0xffffffff) and speed < 4,
                "Unexpected slave descriptor")

        powers = []
        assignments = []
        for index, (_, length, alias) in enumerate(self.symbols[:80]):
            if index != alias:
                continue
            data = self.symbol(index)
            if length == 2 and index + 1 < len(self.symbols) and bytes(self.symbol(index + 1)) == b"\0":
                lane_assign = struct.unpack_from("<H", data)[0]
                if lane_assign in (0x210, 0x3210):
                    assignments.append(lane_assign)
            if not (128 <= length <= 224 and length % 16 == 0):
                continue
            records = list(struct.iter_unpack("<IIII", data))
            if all(size == 12 and kind < len(POWER_NAMES) and delay <= 100 for size, kind, _, delay in records):
                powers.append([dict(type=POWER_NAMES[kind], value=value, delay_ms=delay)
                               for _, kind, value, delay in records])
        require(len(assignments) == 1 and len(powers) == 2, "Could not identify CSI assignment and power sequences")
        require(Counter(x["type"] for x in powers[0]) == Counter(x["type"] for x in powers[1]) and
                all(x["value"] == 0 for x in powers[1]), "Power-down sequence does not cover power-up")
        return dict(slave_7bit=slave >> 1, address_bytes=addr_size, data_bytes=data_size, id_register=reg,
                    expected_id=expected, mask=mask, i2c_speed_index=speed, csi_lane_assign=assignments[0],
                    power_up=powers[0], power_down=powers[1])

    def streams(self, index, count):
        rows = self.symbol(index)
        require(1 <= count <= 4 and len(rows) == count * 89, "Unexpected CSI stream table size")
        for offset in range(0, len(rows), 89):
            size, vc_shape, vc_id = struct.unpack_from("<III", rows, offset)
            require(size == 85 and vc_shape in (1, 2) and u32(rows, offset + 13) == 16,
                    "Unexpected CSI stream record")
            vc_data = self.symbol(vc_id)
            require(len(vc_data) == vc_shape * 4, "Unexpected virtual channel record")
            vc = u32(vc_data, 0)
            data_type = rows[offset + 12]
            x, y, width, height, bits, kind = struct.unpack_from("<6I", rows, offset + 17)
            require(vc <= 31 and data_type <= 0x3f and 0 < width <= 20000 and 0 < height <= 16000 and
                    1 <= bits <= 32 and kind <= 5, "CSI stream fields are out of range")
            yield dict(vc=vc, data_type=data_type, x=x, y=y, width=width, height=height, bits=bits, type=kind)

    def modes(self):
        tables = []
        for index, (_, length, alias) in enumerate(self.symbols[:160]):
            if index != alias or length < 484:
                continue
            table = self.symbol(index)
            offset = 0
            modes = []
            try:
                while offset < length:
                    size = u32(table, offset) + 4
                    require(400 <= size <= 640 and offset + size <= length, "Unexpected mode record length")
                    record = table[offset:offset + size]
                    shape, stream_count, stream_id = struct.unpack_from("<III", record, 32)
                    require(shape == 8, "Missing CSI stream array")
                    streams = list(self.streams(stream_id, stream_count))
                    lanes = u32(record, 104)
                    settle, three_phase = struct.unpack_from("<II", record, 132)
                    fps = struct.unpack_from("<d", record, 80)[0]
                    output_pixel_clock = u32(record, 68)
                    require(1 <= lanes <= 4 and three_phase in (0, 1) and settle <= 10000 and 1 <= fps <= 1000,
                            "Unexpected mode timing or PHY fields")
                    shape, settings_count, settings_id = struct.unpack_from("<III", record, 148)
                    settings = self.symbol(settings_id)
                    require(shape == 8 and len(settings) == settings_count * 60 and settings_count > 0 and
                            u32(settings, 0) == 56, "Unexpected mode settings reference")
                    modes.append(dict(index=len(modes), fps=fps, lanes=lanes, c_phy=bool(three_phase),
                                      settle_ns=settle, output_pixel_clock_hz=output_pixel_clock,
                                      data_rate_bps=output_pixel_clock * streams[0]["bits"] // lanes,
                                      streams=streams, settings_symbol=settings_id,
                                      settings_count=settings_count))
                    offset += size
            except (ValueError, struct.error):
                continue
            if modes:
                tables.append(modes)
        require(len(tables) == 1, "Could not identify a unique mode array")
        return tables[0]

    def settings(self, index):
        for record in struct.iter_unpack("<15I", self.symbol(index)):
            size, register, count, data_id, address_bytes, data_bytes, op, delay_id = (
                record[0], record[7], record[8], record[9], record[10], record[11], record[12], record[14])
            require(size == 56 and 1 <= address_bytes <= 4 and 1 <= data_bytes <= 4 and op < len(OPERATIONS),
                    "Unexpected sensor register operation")
            data = self.symbol(data_id)
            delay = self.symbol(delay_id)
            require(len(data) == count * 4 and len(delay) == 4, "Invalid sensor register data or delay")
            values = [value for (value,) in struct.iter_unpack("<I", data)]
            require(register < 1 << (address_bytes * 8) and all(v < 1 << (data_bytes * 8) for v in values),
                    "Sensor register values do not fit the declared wire widths")
            yield dict(register=register, operation=OPERATIONS[op], address_bytes=address_bytes,
                       data_bytes=data_bytes, delay_us=u32(delay, 0), values=values)

    def initialization(self):
        root = self.data[0x20:0x24 + u32(self.data, 0x20)]
        result = []
        for offset in range(4, len(root) - 8, 4):
            present, index = struct.unpack_from('<II', root, offset)
            if present != 1 or index >= len(self.symbols) or len(self.symbol(index)) != 24:
                continue
            record = struct.unpack_from('<6I', self.symbol(index))
            if record[0] != 20 or record[5] >= len(self.symbols):
                continue
            if record[3] == 8:
                shape, count, target = record[3:]
            elif record[4] == 1 and len(self.symbol(record[5])) == 12:
                shape, count, target = struct.unpack_from('<III', self.symbol(record[5]))
            else:
                continue
            if shape == 8 and count > 0 and target < len(self.symbols) and len(self.symbol(target)) == count * 60:
                # The stock 6.0.7 descriptor extends the public XSD. Keep
                # this scalar uninterpreted until its version/delay role is
                # established in the matching HAL.
                result.append(dict(header_value=record[1], settings_symbol=target,
                                   settings=list(self.settings(target))))
        require(result, 'No initialization records found in the selected sensor driver')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path, nargs="+")
    parser.add_argument("--mode", type=int, help="also extract the selected mode's register groups")
    parser.add_argument("--initialization", action="store_true", help="also extract initialization and adjacent read groups")
    parser.add_argument("--output", required=True, type=Path, help="local private output JSON")
    args = parser.parse_args()
    result = []
    for path in args.module:
        module = StockModule(path)
        modes = module.modes()
        item = dict(module=module.name, sha256=module.sha256, board=module.board(), modes=modes)
        if args.mode is not None:
            require(0 <= args.mode < len(modes), "Requested mode is outside its module")
            mode = modes[args.mode]
            item["selected_settings"] = list(module.settings(mode["settings_symbol"]))
        if args.initialization:
            item['initialization'] = module.initialization()
        result.append(item)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
