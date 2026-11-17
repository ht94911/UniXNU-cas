"""Run production public parameter parsing before the long runtime build."""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--unixnu',required=True,type=Path);p.add_argument('--logs',required=True,type=Path);a=p.parse_args()
root=a.unixnu.resolve();kit=root/'drivers/AdrenoMetal';out=a.logs/'camera-public';out.mkdir(parents=True,exist_ok=False)
# Read the production Foundation parser and its type objects directly. No
# Vulkan functions are part of this early parser/type check.
frontend=(kit/'NativeBridgeMetalFrontend.mm').read_text()
objects=frontend[frontend.index('@class NBStructType,NBArrayType,NBPointerType,NBTextureReferenceType;'):frontend.index('@interface NBRenderPipelineReflection :')]
objects=objects.replace('@implementation NBStructMember','static MTLType *publicMetadataType(NSDictionary *);\n@implementation NBStructMember')
fixture=(kit/'bridge/camera_function_spi_host.mm').read_text()
fixture=fixture[fixture.index('static NSData *publicArgumentsMetallib('):fixture.index('static unsigned publicArgumentCases()')]
fixture=fixture[:fixture.index('static NBFunction *publicFunction(')]
test=(kit/'bridge/camera_function_spi_host.mm').read_text()
test=test[test.index('static unsigned publicArgumentCases()'):test.index('\nint main(')]
test=test.replace('NBFunction *function=publicFunction(payload);NSArray *arguments=function.arguments;', 'NSData *bytes=publicArgumentsMetallib(payload);bool found=false;NSDictionary *parsed=nbPublicFunctionArguments((const uint8_t *)bytes.bytes,bytes.length,@"public_values",&found);NSArray *arguments=reflectionArguments(parsed[@"bindings"]);')
test=test.replace('assert(function && !function.handle && arguments.count==8);','assert(found && arguments.count==8);')
test=test.replace('assert(function.arguments==arguments && scalar.dataTypeDescription==scalar.dataTypeDescription);','assert(scalar.dataTypeDescription==scalar.dataTypeDescription);')
test=test.replace('assert(function.returnType.dataType==MTLDataTypeFloat4 && function.returnType==function.returnType);','assert(publicMetadataType(parsed[@"returnType"]).dataType==MTLDataTypeFloat4);')
test=test.replace('publicFunction([NSData dataWithBytes:&empty length:1]).arguments.count', 'parseArguments([NSData dataWithBytes:&empty length:1]).count')
test=test.replace('publicFunction(shortened).arguments', 'parseArguments(shortened)')
test=test.replace('publicFunction([NSData dataWithBytes:missingNul length:sizeof missingNul]).arguments', 'parseArguments([NSData dataWithBytes:missingNul length:sizeof missingNul])')
arguments=frontend[frontend.index('static NSArray *reflectionArguments(NSArray *bindings) {'):frontend.index('// Metal answers a reflection where')]
helper="\nstatic NSArray *parseArguments(NSData *payload) {NSData *bytes=publicArgumentsMetallib(payload);bool found=false;NSDictionary *parsed=nbPublicFunctionArguments((const uint8_t *)bytes.bytes,bytes.length,@\"public_values\",&found);return parsed[@\"bindings\"]?reflectionArguments(parsed[@\"bindings\"]):nil;}\n"
text='#import <Foundation/Foundation.h>\n#import <Metal/Metal.h>\n#include <cassert>\n#include <vector>\n#include "'+str(kit/'NativeMetalFunctionMetadata.h')+'"\n'+objects+'\n'+arguments+'\n'+fixture+helper+test+"\nint main() {@autoreleasepool {setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);unsigned checks=publicArgumentCases();printf(\"PASS Camera public metadata: %u cases\\n\",checks);return 0;}}\n"
harness=out/'public.mm';harness.write_text(text)
flags=['-std=c++17','-fobjc-arc','-O1','-g0','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-deprecated-declarations','-I',str(kit),'-I',str(kit/'bridge'),'-I',str(root/'drivers/Adreno740'),'-I',str(root/'.cache/mesa-work/include')]
compiled=subprocess.run(['xcrun','clang++',*flags,str(harness),'-Wl,-dead_strip','-framework','Foundation','-framework','Metal','-framework','IOSurface','-framework','CoreFoundation','-o',str(out/'consumer')],capture_output=True,text=True)
(out/'build.log').write_text(compiled.stdout+compiled.stderr);assert compiled.returncode==0,'Public consumer compile failed'
run=subprocess.run([str(out/'consumer')],capture_output=True,text=True,timeout=30,env=dict(os.environ,SHENG_DIAG='0',NMB_LOG_STDERR='1'))
(out/'run.log').write_text(run.stdout+run.stderr);(out/'exit.json').write_text(json.dumps(dict(returncode=run.returncode,stdout_bytes=len(run.stdout),stderr_bytes=len(run.stderr)))+'\n')
if run.returncode:
 debug=subprocess.run(['xcrun','lldb','--batch','-o','run','-o','bt','--',str(out/'consumer')],capture_output=True,text=True,timeout=45,env=dict(os.environ,SHENG_DIAG='0',NMB_LOG_STDERR='1'))
 (out/'failure-backtrace.log').write_text(debug.stdout+debug.stderr)
match=re.search(r'PASS Camera public metadata: (\d+) cases',run.stdout)
assert run.returncode==0 and match and int(match[1])==14,str(run.returncode)+' '+run.stdout+run.stderr
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report=dict(passed=True,checks=14,production_public_parameters_executed=True,production_return_type_executed=True,source_sha256=sha(kit/'NativeBridgeMetalFrontend.mm'),metadata_source_sha256=sha(kit/'NativeMetalFunctionMetadata.h'),test_sha256=sha(kit/'bridge/camera_function_spi_host.mm'),GPU_submissions=False,phone_operations=False)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
