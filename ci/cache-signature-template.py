"""Mac checks for public cache serialization and an ad hoc detached template.

Only a length is supplied by the local preparation. No Apple plist, firmware,
device address or pairing input enters this fixture or the runner.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--bytes', type=int, required=True)
p.add_argument('--tool', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
assert 0 < a.bytes <= 4 * 1024 * 1024
a.out.mkdir(exist_ok=True)
spec = importlib.util.spec_from_file_location('detached_cache', a.tool)
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)
fixture = a.out / 'cache-template.data'
signature = a.out / 'cache-template.sig'
fixture.write_bytes(bytes(a.bytes))
subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--digest-algorithm','sha256',
                '--identifier',tool.IDENTIFIER,'--detached',str(signature),str(fixture)],check=True)
subprocess.run(['codesign','--verify','--strict','--detached',str(signature),str(fixture)],check=True)
tool.verify(signature.read_bytes(), fixture.read_bytes())
data = b'A' * a.bytes
rewritten, state = tool.rewrite(signature.read_bytes(), data)
changed = a.out / 'cache-rehashed.data'; changed.write_bytes(data)
changed_signature = a.out / 'cache-rehashed.sig'; changed_signature.write_bytes(rewritten)
subprocess.run(['codesign','--verify','--strict','--detached',str(changed_signature),str(changed)],check=True)
changed.write_bytes(b'B' + data[1:])
negative = subprocess.run(['codesign','--verify','--strict','--detached',str(changed_signature),str(changed)],capture_output=True)
assert negative.returncode != 0, 'Changed signed data must be refused'
for item in (fixture,changed,changed_signature): item.unlink()

# The local Linux plutil shim is plistlib's binary1 writer. Exercise that
# serializer with the same scalar/collection kinds and a large job dictionary
# through the real Mac private XPC entry used by upstream's runtime check.
jobs = {}
for i in range(729):
    jobs['/System/Library/LaunchDaemons/org.unixnu.fixture'+str(i)+'.plist'] = {
        'Label':'org.unixnu.fixture'+str(i),'UserName':'mobile','EnableTransactions':True,
        'EnablePressuredExit':False,'ThrottleInterval':3,'ProgramArguments':['/public/fixture'],
        'MachServices':{'org.unixnu.fixture'+str(i):True},'EnvironmentVariables':{'PUBLIC':'1'}}
blue = '/System/Library/LaunchDaemons/com.apple.BlueTool.plist'
jobs[blue] = {'Label':'com.apple.BlueTool','UserName':'root','EnableTransactions':True,
             'EnablePressuredExit':False,'ThrottleInterval':3}
value = {'LaunchDaemons':jobs,'AppExtensions':{},'VersionNumber':1,
         'AppRemovalServices':[],'SystemLibraryTreeState':{},'Symlinks':{'public':'fixture'}}
serialized = plistlib.dumps(value,fmt=plistlib.FMT_BINARY,sort_keys=True)
assert plistlib.loads(serialized) == value
xpc = ctypes.CDLL('/usr/lib/system/libxpc.dylib')
xpc.xpc_create_from_plist.argtypes = [ctypes.c_void_p,ctypes.c_size_t]
xpc.xpc_create_from_plist.restype = ctypes.c_void_p
xpc.xpc_dictionary_get_value.argtypes = [ctypes.c_void_p,ctypes.c_char_p]
xpc.xpc_dictionary_get_value.restype = ctypes.c_void_p
xpc.xpc_dictionary_get_string.argtypes = [ctypes.c_void_p,ctypes.c_char_p]
xpc.xpc_dictionary_get_string.restype = ctypes.c_char_p
xpc.xpc_release.argtypes = [ctypes.c_void_p]
buffer = ctypes.create_string_buffer(serialized)
root = xpc.xpc_create_from_plist(buffer,len(serialized)); assert root
dictionary = xpc.xpc_dictionary_get_value(root,b'LaunchDaemons'); assert dictionary
job = xpc.xpc_dictionary_get_value(dictionary,blue.encode()); assert job
assert xpc.xpc_dictionary_get_string(job,b'UserName') == b'root'
xpc.xpc_release(root)
report = dict(passed=True,template_bytes=a.bytes,template_sha256=hashlib.sha256(signature.read_bytes()).hexdigest(),
    real_codesign_verified_template=True,real_codesign_verified_rehashed_file=True,
    changed_data_refused=True,private_XPC_accepted_public_plistlib_fixture=True,
    fixture_jobs=len(jobs),Apple_input_uploaded=False,phone_operations=False,
    tool_sha256=hashlib.sha256(a.tool.read_bytes()).hexdigest(),signature_metadata=state)
(a.out/'cache-template-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
