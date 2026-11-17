#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Supply native model and real rear-camera identity in the retained ADT.

24A446 HWModelDescriptionForCamera reads /product/unique-model. The retained
GPU051 ADT identifies iPhone14,4/D16 at its root but has no product properties.
Use that same target's D16AP model; preserve all existing encoded properties,
children, private inputs, hardware nodes and platform identity.
The native still-camera tag also reads /arm-io/isp/camera-rear. When selected
by this board's actual camera support, declare rear presence without an Apple
ISP compatible, MMIO resource or additional service.
"""
import argparse
from pathlib import Path
import struct
import tomllib


def camera_identity(data, rear_present=False):
    def decode(offset):
        start=offset
        count,children=struct.unpack_from('<II',data,offset)
        offset+=8
        properties={}
        for _ in range(count):
            key=data[offset:offset+32].split(b'\0',1)[0].decode('ascii')
            size=struct.unpack_from('<I',data,offset+32)[0]&0x7fffffff
            offset+=36
            if offset+size>len(data) or key in properties:
                raise ValueError('Invalid retained ADT property')
            properties[key]=data[offset:offset+size]
            offset+=(size+3)&~3
        end_properties=offset
        nodes=[]
        for _ in range(children):
            child,offset=decode(offset)
            nodes.append(child)
        return dict(start=start,count=count,child_count=children,properties=properties,
                    end_properties=end_properties,end=offset,children=nodes),offset

    root,end=decode(0)
    if end!=len(data) or root['properties'].get('model')!=b'iPhone14,4\0' or root['properties'].get('target-type')!=b'D16\0':
        raise ValueError('Camera identity requires the retained iPhone14,4/D16 ADT')
    product=next(child for child in root['children'] if child['properties'].get('name')==b'product\0')
    existing=product['properties'].get('unique-model')
    if existing is not None and existing!=b'D16AP\0':
        raise ValueError('Retained camera model differs from the selected D16 target')
    changes=[]
    if existing is None:
        field=struct.pack('<32sI',b'unique-model',6)+b'D16AP\0'+bytes(2)
        changes += [(product['start'],4,struct.pack('<I',product['count']+1)),
                    (product['end_properties'],0,field)]
    if rear_present:
        bus=next(child for child in root['children'] if child['properties'].get('name')==b'arm-io\0')
        # Original still-camera asks only /arm-io/isp.camera-rear. The name
        # is its native capability path, not an Apple hardware driver match.
        isp=next((child for child in bus['children'] if child['properties'].get('name')==b'isp\0'),None)
        if isp:
            if isp['properties'].get('camera-rear')!=struct.pack('<I',1):
                raise ValueError('Existing camera capability differs from the real rear camera')
        else:
            name=struct.pack('<32sI',b'name',4)+b'isp\0'
            rear=struct.pack('<32sI',b'camera-rear',4)+struct.pack('<I',1)
            logical=struct.pack('<II',2,0)+name+rear
            changes += [(bus['start']+4,4,struct.pack('<I',bus['child_count']+1)),
                        (bus['end'],0,logical)]
    output=data
    for offset,size,replacement in sorted(changes,reverse=True):
        output=output[:offset]+replacement+output[offset+size:]
    return output


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--board',type=Path,default=Path(__file__).resolve().parents[1]/'board.toml')
    args=parser.parse_args()
    board=tomllib.loads(args.board.read_text())
    rear=board.get('camera',{}).get('adt',{}).get('rear_present',False)
    output=camera_identity(args.input.read_bytes(),rear)
    args.output.write_bytes(output)
    print('Retained D16AP camera model supplied; real rear-camera capability:',rear)


if __name__=='__main__':
    main()
