#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Nezha17 Synaptics producer,27 pinned public HID API and real iPhone PAC ABI.
Binding algorithm reused from sheng27 recipe69c3;no old unsigned-vptr patch.
"""
from pathlib import Path
import importlib.util,json,subprocess,hashlib,struct,shutil,tomllib,shlex,os
from kernel_target import ARCH,TRIPLE,check_object
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/nezha27-input';OUT.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=300,**kw)
def require(ok,why):
 if not ok:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def load(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
require(ARCH=='arm64e','27 HID requires selected arm64e target')
helper=load('source_driver_compile',ROOT/'scripts/build-sheng-source-drivers.py');storage_helper=load('public_storage_headers',ROOT/'scripts/prepare-storage-headers.py')
# OS profiles are resolved by UniXNU into refer; these are the selected27 source commits.
hid=ROOT/'refer/IOHIDFamily';storage=ROOT/'refer/IOStorageFamily'
hid_pin='e31e90a24c27ad6cbe981650d12a1a4124065b03';storage_pin='7edb88fbae296fb7c8ce2f64e115e116e566d51c'
if not hid.exists():
 hid=OUT/'source/IOHIDFamily';hid.parent.mkdir(parents=True,exist_ok=True);require(not hid.exists(),'partial HID checkout requires review');run(['git','clone','--depth','1','--branch','IOHIDFamily-2238.120.5','https://github.com/apple-oss-distributions/IOHIDFamily.git',hid])
storage_helper.pinned_source(hid,hid_pin);headers=storage_helper.prepare(storage,storage_pin,OUT/'headers');include=OUT/'headers/include'
for source_dir,relative in [(hid/'IOHIDFamily','IOKit/hid'),(hid/'IOHIDSystem/IOKit/hidsystem','IOKit/hidsystem')]:
 dest=include/relative;dest.mkdir(parents=True,exist_ok=True)
 for p in sorted(source_dir.glob('*.h')):shutil.copyfile(p,dest/p.name)
sdk=Path(headers['sdk']);includes=['-I',str(include),'-idirafter',str(sdk/'System/Library/Frameworks/Kernel.framework/Versions/A/Headers')]
# Controller protocol is supplied by the resolved,independent SoC source.
physical_device=Path(os.environ['UNIXNU_DEVICE']).resolve()
local_sources=tomllib.loads((physical_device/'local.toml').read_text())['source']
soc_source=next(item for item in local_sources if item['name']=='sm8850' and item['kind']=='soc')
soc_header=(physical_device/soc_source['local']).resolve()/'include/Sm8850DsiFifo.h'
require(sha(soc_header)=='5f9a2ce924585f327c088bcdaf03e6f30fe4b1573b65b3df10eb02b71bc3442f','qualified SM8850 FIFO protocol identity')
shutil.copyfile(soc_header,include/'Sm8850DsiFifo.h')
base=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837';sources=ROOT/'kits/nezha/nezha27-input';records={False:json.loads((base/'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(',')),True:json.loads((base/'bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))};details={}
for name in ['NativeHidInput.cpp','HidInputProbe.cpp','NezhaInput.c','ResourceProbe.c','BacklightProbe.cpp']:
 source=ROOT/'kits/nezha/backlight'/name if name=='BacklightProbe.cpp' else sources/name;obj=OUT/(source.stem+'.o');rc=records[source.suffix=='.c'];command=helper.compile_command(rc,source,obj,includes,['-I',str(ROOT/'.cache/xnu-work/osfmk'),'-I',str(sources),'-I',str(ROOT/'kits/nezha/backlight'),'-Wno-unknown-warning-option','-Wno-unused-variable'])
 (OUT/(source.stem+'-command.json')).write_text(json.dumps(command,indent=2));r=subprocess.run(command,cwd=rc['directory'],capture_output=True,text=True);(OUT/(source.stem+'.log')).write_text(r.stdout+r.stderr);require(not r.returncode,r.stdout+r.stderr);check_object(obj.read_bytes(),name);details[name]=dict(source_sha256=sha(source),object_sha256=sha(obj))
 deps={}
 for n in shlex.split(obj.with_suffix('.d').read_text().replace(chr(92)+chr(10),'').split(':',1)[1]):
  p=Path(n);p=p if p.is_absolute() else Path(rc['directory'])/p;deps[str(p.resolve())]=sha(p)
 (OUT/(source.stem+'-headers.json')).write_text(json.dumps(deps,indent=2))

def discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 with the fixed key, modulo 0xffff, plus one."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask  # noqa: E731
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = len(data) << 56 & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1

def macho(data):
    """Load commands, segments and sections of an arm64e relocatable object."""
    magic, cpu, _, _, count = struct.unpack_from('<5I', data)
    require(magic == 0xfeedfacf and cpu == 0x100000c, 'arm64 Mach-O object')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', data, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', data, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', data, at + 32)
                sections.append({'name': data[at:at + 16].rstrip(b'\0').decode(), 'va': va, 'size': length,
                                 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    return {'commands': commands, 'segments': segments}

def object_symbols(data):
    commands = macho(data)['commands']
    st = next(p for c, p, _ in commands if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', data, st + 8)
    found = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', data, sy + i * 16)
        found.append((data[strings + ix:data.index(b'\0', strings + ix)].decode(), va, kind))
    return found

def object_vtable(data, found, name, count):
    """[(symbol, signed word)] of the first count entries of vtable name (authenticated pointer relocations)."""
    va = next(v for n, v, _ in found if n == name)
    entries = {}
    for segment in macho(data)['segments']:
        for sec in segment['sections']:
            if sec['va'] <= va < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', data, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', data, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        entries[sec['va'] + offset] = (found[bits & 0xffffff][0],
                                                       struct.unpack_from('<Q', data, sec['fileoff'] + offset)[0])
    return [entries.get(va + 16 + i * 8) for i in range(count)]

abi=json.loads(Path(__file__).with_name('hid-device-iphone-24A446.json').read_text());require(abi['kernelcache_sha256']=='d99b7719ad4b367db264a7682806aecaf6e0621c6761d07325a58337319d4238','exact iPhone fixture');slots=[(int(t,16),int(d,16)) for t,d in abi['slots']];require(len(slots)==207 and int(abi['instance_size'],16)==208,'original HID layout')
raw=(OUT/'NativeHidInput.o').read_bytes();found=object_symbols(raw);bindings={n:int(v,16) for n,v in abi['bindings'].items()};by_diversity={}
for i,(_,d) in enumerate(slots):by_diversity.setdefault(d,[]).append(i)
vtables=[n for n,_,k in found if n.startswith('__ZTV') and k!=1 and '9MetaClassE' not in n];slot_of={};require(vtables,'owned HID subclass vtable')
for vt in vtables:
 entries=object_vtable(raw,found,vt,207);require(all(entries),'all207 compiled authenticated slots')
 for i,(name,word) in enumerate(entries):
  require(word>>63 and word>>48&1 and word>>49&3==0 and (word>>32)&65535==slots[i][1],f'{vt} slot{i} diversity differs from actual original')
  if '11IOHIDDevice' in name:require(slot_of.setdefault(name,i)==i,'one base import slot')
for name in sorted({n for n,_,k in found if k==1 and '11IOHIDDevice' in n}-set(bindings)):
 if name in slot_of:i=slot_of[name]
 else:
  matches=by_diversity.get(discriminator(name[1:]),[]);require(len(matches)==1,'ambiguous original HID import '+name);i=matches[0]
 require(i!=180,'no abstract base slot binding');bindings[name]=slots[i][0]
text='.text\n.p2align 2\n';addresses='';DATA={'__ZN11IOHIDDevice10gMetaClassE','__ZTV11IOHIDDevice'}
for name,va in sorted(bindings.items()):
 if name in DATA:addresses+=f'.globl {name}\n.set {name}, {va:#x}\n'
 else:addresses+=f'.globl {name}.original\n.set {name}.original, {va:#x}\n';text+=f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
for stem,data in [('hid-bindings',text),('hid-addresses',addresses)]:
 p=OUT/(stem+'.S');p.write_text(data);run(['xcrun','clang','-target',TRIPLE,'-isysroot',sdk,'-c',p,'-o',OUT/(stem+'.o')])
# The new backlight class is explicitly unregistered until its postIOBSD
# opt-in probe. All169 owned entries must match the real original class PAC.
backlight=json.loads(Path(__file__).with_name('backlight-iphone-24A446.json').read_text())
require(backlight['kernelcache_sha256']==abi['kernelcache_sha256'] and backlight['virtual_entries']==169,'original27 backlight fixture')
raw_bl=(OUT/'BacklightProbe.o').read_bytes();bl_found=object_symbols(raw_bl)
bl_vtables=[n for n,_,k in bl_found if n.startswith('__ZTV') and k!=1 and '9MetaClassE' not in n]
require(len(bl_vtables)==1,'one owned backlight subclass vtable')
bl_entries=object_vtable(raw_bl,bl_found,bl_vtables[0],169);require(all(bl_entries),'all169 backlight entries')
bl_slots={}
for i,(name,word) in enumerate(bl_entries):
 require(word>>63 and word>>48&1 and word>>49&3==0 and (word>>32)&65535==int(backlight['slots'][i][1],16),f'backlight slot{i} PAC differs from original')
 if '17AppleARMBacklight' in name:bl_slots[name]=i
bl_bindings={n:int(v,16) for n,v in backlight['bindings'].items()}
for name in sorted({n for n,_,k in bl_found if k==1 and '17AppleARMBacklight' in n}-set(bl_bindings)):
 require(name in bl_slots,'unqualified backlight base import '+name)
 bl_bindings[name]=int(backlight['slots'][bl_slots[name]][0],16)
bl_text='.text\n.p2align 2\n';bl_addresses=''
for name,va in sorted(bl_bindings.items()):
 if name in {'__ZN17AppleARMBacklight10gMetaClassE','__ZTV17AppleARMBacklight'}:bl_addresses+=f'.globl {name}\n.set {name}, {va:#x}\n'
 else:
  bl_addresses+=f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
  bl_text+=f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
for stem,text_bl in [('backlight-bindings',bl_text),('backlight-addresses',bl_addresses)]:
 p=OUT/(stem+'.S');p.write_text(text_bl);run(['xcrun','clang','-target',TRIPLE,'-isysroot',sdk,'-c',p,'-o',OUT/(stem+'.o')])
(OUT/'backlight-bindings.json').write_text(json.dumps(dict(kernelcache_sha256=backlight['kernelcache_sha256'],bindings={n:hex(v) for n,v in bl_bindings.items()},compiled_original_PAC_slots=169,owned_class=bl_vtables[0],publication=False,hardware_access=False),indent=2))

llvm=Path(run(['brew','--prefix','llvm'],text=True).strip())/'bin';late=OUT/'NativeHidInput.late.o';run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',OUT/'NativeHidInput.o',late]);bl_late=OUT/'BacklightProbe.late.o';run([llvm/'llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_BacklightProbe.cpp',OUT/'BacklightProbe.o',bl_late]);archive=OUT/'libNezha27Input.a';archive.unlink(missing_ok=True);objects=[late,OUT/'HidInputProbe.o',OUT/'NezhaInput.o',OUT/'ResourceProbe.o',OUT/'hid-bindings.o',OUT/'hid-addresses.o',bl_late,OUT/'backlight-bindings.o',OUT/'backlight-addresses.o'];run(['xcrun','ar','rcs',archive,*objects]);report=dict(source_recipe='sheng27 build-gop-legacy-display-27.py69c3 HID source API binding algorithm',source_17='nezha widgets003 f5b0fdd Synaptics/ordered queues only',original_bindings='fixed-collection-addresses',kernelcache_sha256=abi['kernelcache_sha256'],bindings={n:hex(v) for n,v in bindings.items()},public_HID_commit=hid_pin,compiled_slots=207,backlight_compiled_PAC_slots=169,backlight_qualified_source_only=True,owned_vtables=vtables,objects=details,archive_sha256=sha(archive),late_no_mod_init=True,hardware_qualified=False);(OUT/'build-report.json').write_text(json.dumps(report,indent=2));print('compiled27 native touch/keys,207 actual HID PAC slots verified;hardware pending')
