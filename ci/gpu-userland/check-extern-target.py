"""Verify the external kernel selector in actual arm64e Objective-C metadata."""
from pathlib import Path
import hashlib
import json
import re
import struct

def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value

class Image:
    def __init__(self, data):
        self.data = data
        magic,cpu,subtype,kind,count,size = struct.unpack_from('<6I', data)
        assert (magic,cpu,subtype & 0xffffff) == (0xfeedfacf,0x100000c,2)
        self.commands = {}
        self.segments = []
        offset = 32
        for _ in range(count):
            command,length = struct.unpack_from('<II',data,offset)
            assert length >= 8 and offset + length <= 32 + size
            self.commands[command] = (offset,length)
            if command == 0x19:
                name,va,span,fileoff,filesize = struct.unpack_from('<16s4Q',data,offset+8)
                self.segments.append(dict(name=name.rstrip(b'\0').decode(),va=va,span=span,fileoff=fileoff,filesize=filesize))
            offset += length
        self.base = next(s['va'] for s in self.segments if s['fileoff'] == 0 and s['filesize'])
    def command(self, value):
        return self.commands.get(value)
    def offset(self, va, length=1):
        return next(s['fileoff']+va-s['va'] for s in self.segments if s['va'] <= va and va+length <= s['va']+s['filesize'])
    def read(self, va, length):
        at=self.offset(va,length)
        return self.data[at:at+length]
    def symbols(self):
        command=self.command(2)
        assert command is not None
        offset,count,strings,size=struct.unpack_from('<4I',self.data,command[0]+8)
        result={}
        for index in range(count):
            name,kind,section,_,value=struct.unpack_from('<IBBHQ',self.data,offset+16*index)
            if kind & 0xe0 or kind & 0xe != 0xe or not section:
                continue
            assert name<size
            end=self.data.index(b'\0',strings+name,strings+size)
            result[self.data[strings+name:end].decode()]=value
        return result
    def function_starts(self):
        command=self.command(0x26)
        assert command is not None
        at,size=struct.unpack_from('<II',self.data,command[0]+8)
        end=at+size
        address=next(s['va'] for s in self.segments if s['name']=='__TEXT')
        result=[]
        while at<end:
            value=shift=0
            while True:
                byte=self.data[at];at+=1
                value|=(byte & 127)<<shift;shift+=7
                if not byte & 128:break
            if not value:break
            address+=value;result.append(address)
        return result

def verify(binary, map_path):
    image=Image(Path(binary).read_bytes())
    link_map=Path(map_path).read_text(errors='replace')
    symbols=image.symbols()
    def link_address(name):
        rows = re.findall(r"^(0x[0-9A-Fa-f]+)\s+0x[0-9A-Fa-f]+\s+\[\s*\d+\]\s+" +
                          re.escape(name) + r"$", link_map, re.M)
        assert len(rows) == 1, (name, rows)
        return int(rows[0], 16)


    # Walk the file's own ARM64E_USERLAND24 fixup chains. The Objective-C class
    # pointers and selector refs are recovered from actual rebase entries.
    fixup_command = image.command(0x80000034)
    assert fixup_command is not None
    dataoff, fixup_size = struct.unpack_from("<II", image.data, fixup_command[0] + 8)
    version, starts, imports, strings, import_count, import_format, symbol_format = struct.unpack_from(
        "<7I", image.data, dataoff)
    assert version == 0 and import_format == 2 and symbol_format == 0
    table = dataoff + starts
    count, = struct.unpack_from("<I", image.data, table)
    assert count == len(image.segments)
    fixups = {}
    for index in range(count):
        relative, = struct.unpack_from("<I", image.data, table + 4 + index * 4)
        if not relative:
            continue
        at = table + relative
        size, page_bytes, pointer_format, segment_offset, _, pages = struct.unpack_from("<IHHQIH", image.data, at)
        assert pointer_format == 12 and page_bytes == 16384 and 22 + pages * 2 <= size
        assert image.base + segment_offset == image.segments[index]["va"]
        for page, start in enumerate(struct.unpack_from(f"<{pages}H", image.data, at + 22)):
            if start == 0xffff:
                continue
            assert not start & 0x8000 and start < page_bytes
            page_at = image.base + segment_offset + page * page_bytes
            site = page_at + start
            while True:
                assert site not in fixups and site + 8 <= page_at + page_bytes
                value, = struct.unpack("<Q", image.read(site, 8))
                auth, bind, step = value >> 63, (value >> 62) & 1, (value >> 51) & 0x7ff
                if bind:
                    ordinal = value & 0xffffff
                    assert ordinal < import_count
                    flags, addend = struct.unpack_from("<Ii", image.data, dataoff + imports + ordinal * 8)
                    name_at = dataoff + strings + (flags >> 9)
                    name_end = image.data.index(b"\0", name_at, dataoff + fixup_size)
                    fixups[site] = dict(symbol=image.data[name_at:name_end].decode(), auth=bool(auth),
                                        key=(value >> 49) & 3 if auth else None, addend=addend)
                else:
                    offset = value & 0xffffffff if auth else value & ((1 << 43) - 1)
                    high = 0 if auth else ((value >> 43) & 0xff) << 56
                    target = (image.base + offset) | high
                    fixups[site] = dict(target=target, auth=bool(auth))
                if not step:
                    break
                site += step * 8


    def pointer(site):
        assert site in fixups and "target" in fixups[site], hex(site)
        target = fixups[site]["target"]
        image.offset(target)
        return target


    def cstring(address):
        start = image.offset(address)
        end = image.data.index(b"\0", start, start + 512)
        return image.data[start:end].decode()


    class_address = symbols["_OBJC_CLASS_$_NBLibrary"]
    assert class_address == link_address("_OBJC_CLASS_$_NBLibrary")
    class_ro = pointer(class_address + 32)
    assert class_ro == link_address("__OBJC_CLASS_RO_$_NBLibrary")
    assert cstring(pointer(class_ro + 24)) == "NBLibrary"
    method_list = pointer(class_ro + 32)
    assert method_list == link_address("__OBJC_$_INSTANCE_METHODS_NBLibrary")
    flags, method_count = struct.unpack("<II", image.read(method_list, 8))
    assert flags == 0x8000000c and method_count == 23
    methods = {}
    for i in range(method_count):
        record = method_list + 8 + 12 * i
        name_delta, type_delta, imp_delta = struct.unpack("<iii", image.read(record, 12))
        selector_ref = record + name_delta
        selector = cstring(pointer(selector_ref))
        assert selector not in methods
        methods[selector] = dict(record=record, selector_ref=selector_ref,
                                types=cstring(record + 4 + type_delta), imp=record + 8 + imp_delta)
    entry = methods["newExternFunctionWithName:"]
    ordinary = methods["newFunctionWithName:"]
    assert entry["types"] == ordinary["types"]
    assert entry["imp"] == link_address("-[NBLibrary newExternFunctionWithName:]")
    assert ordinary["imp"] == link_address("-[NBLibrary newFunctionWithName:]")
    assert entry["imp"] in image.function_starts()
    instruction, = struct.unpack("<I", image.read(entry["imp"], 4))
    assert instruction & 0xfc000000 == 0x14000000
    stub = entry["imp"] + signed(instruction & 0x3ffffff, 26) * 4
    assert stub == link_address("_objc_msgSend$newFunctionWithName:")
    stub_words = struct.unpack("<8I", image.read(stub, 32))
    assert stub_words[0] & 0x9f00001f == 0x90000001
    assert stub_words[1] & 0xffc003ff == 0xf9400021
    selector_page = (stub & ~4095) + (signed(
        ((stub_words[0] >> 5) & 0x7ffff) << 2 | ((stub_words[0] >> 29) & 3), 21) << 12)
    selector_ref = selector_page + ((stub_words[1] >> 10) & 0xfff) * 8
    assert selector_ref == ordinary["selector_ref"]
    assert cstring(pointer(selector_ref)) == "newFunctionWithName:"
    assert stub_words[2] & 0x9f00001f == 0x90000011
    assert stub_words[3] & 0xffc003ff == 0x91000231
    message_page = ((stub + 8) & ~4095) + (signed(
        ((stub_words[2] >> 5) & 0x7ffff) << 2 | ((stub_words[2] >> 29) & 3), 21) << 12)
    message_slot = message_page + (((stub_words[3] >> 10) & 0xfff) << (12 if stub_words[3] & (1 << 22) else 0))
    got = [message_slot] if fixups.get(message_slot, {}).get("symbol") == "_objc_msgSend" else []
    assert len(got) == 1 and fixups[got[0]]["auth"] and fixups[got[0]]["key"] == 0
    assert stub_words[4] == 0xf9400230 and stub_words[5] == 0xd71f0a11

    camera_methods = {}
    for selector in ('newFunctionWithName:constantValues:pipelineLibrary:error:',
                     'newFunctionWithName:constantValues:functionCache:error:'):
        method = methods[selector]
        assert method['types'] == '@48@0:8@16@24@32^@40', (selector, method)
        assert method['imp'] == link_address('-[NBLibrary ' + selector + ']')
        assert method['imp'] in image.function_starts()
        camera_methods[selector] = dict(types=method['types'], IMP=hex(method['imp']))
    for class_name, selector in (('NBFunction', 'arguments'), ('NBFunction', 'returnType'), ('NBArgument', 'dataTypeDescription')):
        class_ro = pointer(symbols['_OBJC_CLASS_$_' + class_name] + 32)
        methods_at = pointer(class_ro + 32)
        method_flags, count = struct.unpack('<II', image.read(methods_at, 8))
        assert method_flags == 0x8000000c
        entries = {}
        for i in range(count):
            record = methods_at + 8 + 12*i
            name_delta, type_delta, imp_delta = struct.unpack('<iii', image.read(record, 12))
            name = cstring(pointer(record + name_delta))
            entries[name] = dict(types=cstring(record+4+type_delta), imp=record+8+imp_delta)
        camera_entry = entries[selector]
        assert camera_entry['types'] == '@16@0:8' and camera_entry['imp'] == link_address('-['+class_name+' '+selector+']')
        assert camera_entry['imp'] in image.function_starts()
        camera_methods[class_name+'.'+selector] = dict(types=camera_entry['types'], IMP=hex(camera_entry['imp']))

    # Camera085 builds the correction into the production class. Walk its
    # own metadata and authenticated isRequired message, not a test alias.
    constant_class = symbols['_OBJC_CLASS_$_NBFunctionConstant']
    constant_ro = pointer(constant_class + 32)
    assert cstring(pointer(constant_ro + 24)) == 'NBFunctionConstant'
    constant_list = pointer(constant_ro + 32)
    constant_flags, constant_count = struct.unpack('<II', image.read(constant_list, 8))
    assert constant_flags == 0x8000000c and constant_count == 10
    constants = {}
    for i in range(constant_count):
        record = constant_list + 8 + 12*i
        name_delta, type_delta, imp_delta = struct.unpack('<iii', image.read(record, 12))
        selector_ref = record + name_delta
        selector = cstring(pointer(selector_ref))
        assert selector not in constants
        constants[selector] = dict(selector_ref=selector_ref,
            types=cstring(record+4+type_delta), imp=record+8+imp_delta)
    required, original = constants['required'], constants['isRequired']
    assert required['types'] == original['types'] == 'B16@0:8'
    assert required['imp'] == link_address('-[NBFunctionConstant required]')
    assert original['imp'] == link_address('-[NBFunctionConstant isRequired]')
    assert required['imp'] in image.function_starts()
    branch, = struct.unpack('<I', image.read(required['imp'], 4))
    assert branch & 0xfc000000 == 0x14000000
    required_stub = required['imp'] + signed(branch & 0x3ffffff, 26)*4
    assert required_stub == link_address('_objc_msgSend$isRequired')
    words = struct.unpack('<8I', image.read(required_stub, 32))
    assert words[0] & 0x9f00001f == 0x90000001
    assert words[1] & 0xffc003ff == 0xf9400021
    selector_page = (required_stub & ~4095) + (signed(
        ((words[0] >> 5) & 0x7ffff) << 2 | ((words[0] >> 29) & 3), 21) << 12)
    selector_ref = selector_page + ((words[1] >> 10) & 0xfff)*8
    assert selector_ref == original['selector_ref'] and cstring(pointer(selector_ref)) == 'isRequired'
    assert words[2] & 0x9f00001f == 0x90000011 and words[3] & 0xffc003ff == 0x91000231
    message_page = ((required_stub+8) & ~4095) + (signed(
        ((words[2] >> 5) & 0x7ffff) << 2 | ((words[2] >> 29) & 3), 21) << 12)
    message_slot = message_page + (((words[3] >> 10) & 0xfff) << (12 if words[3] & (1 << 22) else 0))
    assert fixups[message_slot]['symbol'] == '_objc_msgSend'
    assert fixups[message_slot]['auth'] and fixups[message_slot]['key'] == 0
    assert words[4] == 0xf9400230 and words[5] == 0xd71f0a11

    return dict(passed=True,binary_sha256=hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
        actual_method_list_verified=True,actual_message_forwarding_verified=True,
        class_address=hex(class_address),method_list=hex(method_list),method_count=method_count,
        extern_IMP=hex(entry["imp"]),message_stub=hex(stub),authenticated_message_branch=True,
        discarded_local_symbol_required=False,phone_operations=False,
        required_function_constant_method_list_verified=True,
        required_function_constant_encoding='B16@0:8',
        required_function_constant_IMP=hex(required['imp']),
        isRequired_function_constant_IMP=hex(original['imp']),
        required_forwards_to_original_getter=True,
        camera_function_SPI_method_metadata_verified=True,
        camera_function_methods=camera_methods)

if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('binary',type=Path);p.add_argument('link_map',type=Path);p.add_argument('--out',type=Path,required=True)
    a=p.parse_args();result=verify(a.binary,a.link_map)
    a.out.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
