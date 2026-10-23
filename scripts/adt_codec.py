"""Bounded little-endian Apple Device Tree codec (not Android FDT).

Layout reference: XNU pexpert/device_tree.h and UniXNU-umi umi_adt.py.
Copyright (c) 2026 UniXNU contributors.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
from dataclasses import dataclass, field
import re
import struct

MAX_BLOB = 1 << 20
MAX_DEPTH = 32
MAX_NODES = 1024
MAX_PROPERTIES = 128
MAX_CHILDREN = 256
_NAME = re.compile(r"[-A-Za-z0-9_,#@.+:]+")


def _integer(value, bits):
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f"integer does not fit u{bits}")
    return value


def u32(value):
    return struct.pack("<I", _integer(value, 32))


def u64(value):
    return struct.pack("<Q", _integer(value, 64))


def string(value):
    if not isinstance(value, str) or not value or any(ord(c) < 32 or ord(c) > 126 for c in value):
        raise ValueError("expected nonempty printable ASCII string")
    return value.encode("ascii") + b"\0"


def strings(values):
    if not isinstance(values, list) or not values:
        raise ValueError("expected nonempty string list")
    return b"".join(string(value) for value in values)


def _name(value, limit):
    if not isinstance(value, str) or len(value) > limit or not _NAME.fullmatch(value):
        raise ValueError("invalid or overlong ADT name")
    return string(value)


@dataclass
class Node:
    name: str
    properties: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    def at(self, path):
        if not isinstance(path, str) or not path.startswith("/") or "//" in path:
            raise ValueError("expected absolute node path")
        current = self
        for part in path.strip("/").split("/") if path.strip("/") else []:
            matches = [child for child in current.children if child.name == part]
            if len(matches) != 1:
                raise ValueError(f"missing or ambiguous node {path}")
            current = matches[0]
        return current

    def walk(self, path="/"):
        yield path, self
        for child in self.children:
            yield from child.walk(path.rstrip("/") + "/" + child.name)

    def encode(self):
        count = 0
        ancestors = set()

        def visit(node, depth):
            nonlocal count
            count += 1
            if depth > MAX_DEPTH or count > MAX_NODES or id(node) in ancestors:
                raise ValueError("ADT node/depth limit or cycle")
            ancestors.add(id(node))
            name = _name(node.name, 63)
            if not isinstance(node.properties, dict) or "name" in node.properties:
                raise ValueError("explicit name property is forbidden")
            if not isinstance(node.children, list) or len(node.children) > MAX_CHILDREN:
                raise ValueError("invalid child list")
            if any(not isinstance(child, Node) for child in node.children):
                raise ValueError("invalid child node")
            if len({child.name for child in node.children}) != len(node.children):
                raise ValueError("duplicate child node")
            props = {"name": name, **node.properties}
            if len(props) > MAX_PROPERTIES:
                raise ValueError("property count limit")
            data = bytearray(struct.pack("<II", len(props), len(node.children)))
            for key, value in props.items():
                key_raw = _name(key, 31)
                if type(value) is not bytes or len(value) > MAX_BLOB:
                    raise ValueError("invalid property value")
                data += struct.pack("<32sI", key_raw, len(value))
                data += value
                data += bytes(-len(value) % 4)
                if len(data) > MAX_BLOB:
                    raise ValueError("ADT byte limit")
            for child in node.children:
                data += visit(child, depth + 1)
                if len(data) > MAX_BLOB:
                    raise ValueError("ADT byte limit")
            ancestors.remove(id(node))
            return data

        if self.name != "device-tree":
            raise ValueError("root must be device-tree")
        return bytes(visit(self, 0))


def decode(blob):
    """Reject duplicate names, nonzero padding, excessive counts and truncation."""
    if type(blob) is not bytes or not 8 <= len(blob) <= MAX_BLOB:
        raise ValueError("invalid ADT buffer")
    node_count = 0

    def read(offset, depth):
        nonlocal node_count
        node_count += 1
        if depth > MAX_DEPTH or node_count > MAX_NODES or len(blob) - offset < 8:
            raise ValueError("truncated/deep ADT or node count limit")
        properties, children = struct.unpack_from("<II", blob, offset)
        offset += 8
        if not 1 <= properties <= MAX_PROPERTIES or children > MAX_CHILDREN:
            raise ValueError("property/child count limit")
        if properties * 36 + children * 8 > len(blob) - offset:
            raise ValueError("counts exceed remaining buffer")
        props = {}
        for index in range(properties):
            if len(blob) - offset < 36:
                raise ValueError("truncated property header")
            key_raw, size = struct.unpack_from("<32sI", blob, offset)
            offset += 36
            if b"\0" not in key_raw:
                raise ValueError("unterminated property key")
            key_bytes, padding = key_raw.split(b"\0", 1)
            try:
                key = key_bytes.decode("ascii")
            except UnicodeDecodeError as error:
                raise ValueError("non-ASCII property key") from error
            _name(key, 31)
            end = offset + size
            padded = (end + 3) & ~3
            if any(padding) or key in props or padded > len(blob):
                raise ValueError("duplicate property, key padding or truncated value")
            if index == 0 and key != "name":
                raise ValueError("name must be the first property")
            if any(blob[end:padded]):
                raise ValueError("nonzero property alignment padding")
            props[key] = blob[offset:end]
            offset = padded
        name_bytes = props.pop("name")
        if not name_bytes.endswith(b"\0") or b"\0" in name_bytes[:-1]:
            raise ValueError("invalid node name string")
        try:
            name = name_bytes[:-1].decode("ascii")
        except UnicodeDecodeError as error:
            raise ValueError("non-ASCII node name") from error
        _name(name, 63)
        nodes, names = [], set()
        for _ in range(children):
            child, offset = read(offset, depth + 1)
            if child.name in names:
                raise ValueError("duplicate child node")
            names.add(child.name)
            nodes.append(child)
        return Node(name, props, nodes), offset

    tree, end = read(0, 0)
    if end != len(blob) or tree.name != "device-tree":
        raise ValueError("wrong root or trailing bytes")
    return tree
