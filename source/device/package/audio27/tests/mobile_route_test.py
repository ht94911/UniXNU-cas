"""Exercise the actual bounded HAL route observer with reentrant CoreAudio peers."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'hal/NezhaAudioPlugIn.c').read_text()


def function(name):
    start = source.index('static void ' + name + '(')
    end = source.index('{', start) + 1
    level = 1
    while level:
        level += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "os/NezhaAudioAbi.h"
#include "core/AudioHalDiagnostics.h"
#include "core/AudioProtocol.h"
typedef uint32_t UInt32,AudioObjectID,AudioObjectPropertySelector,io_connect_t;
typedef int OSStatus;
typedef const char *CFStringRef;
typedef struct {UInt32 mSelector,mScope,mElement;} AudioObjectPropertyAddress;
#define kDeviceUID "NezhaAudio27Device"
#define CFSTR(s) ((CFStringRef)s)
enum {kAudioHardwarePropertyTranslateUIDToDevice=101,kAudioHardwarePropertyDefaultOutputDevice=102,
 kAudioHardwarePropertyDefaultSystemOutputDevice=103,kAudioHardwarePropertyDevices=104,
 kAudioObjectSystemObject=1,kAudioObjectUnknown=0,kAudioObjectPropertyScopeGlobal=10,
 kAudioObjectPropertyElementMain=0,kAudioHardwareBadPropertySizeError=-70};
static pthread_mutex_t gLock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gDiscoveryActive;
static UInt32 gEpoch,gConnect,gSystemRouteSamples,gObservationCount;
static NaHalObservation gObservations[NA_HAL_HISTORY_CAPACITY];
static NaHalHistory history;
static int alive,lookup_error,query_error,short_lookup,short_default;
static unsigned calls,callbacks,lose_on_call,reported,rejected,eligibility_calls;
static UInt32 uid,defaults[2],device_list_bytes;
static int available(void) {return alive;}
static void enter_api(void) {
 assert(!pthread_mutex_trylock(&gLock));++callbacks;pthread_mutex_unlock(&gLock);
 if(++calls==lose_on_call)++gConnect;
}
static void report_hal(UInt32 kind,UInt32 object,UInt32 selector,UInt32 scope,UInt32 element,int result,UInt32 value) {
 NaHalObservation e={NA_HAL_OBSERVATION_VERSION,kind,object,selector,scope,element,result,value};
 int r=na_hal_history_add(&history,&e);if(!r)++reported;else {assert(r==NA_BUSY);++rejected;}
}
static OSStatus AudioObjectGetPropertyData(AudioObjectID object,const AudioObjectPropertyAddress*a,
 UInt32 qualifier_bytes,const void*qualifier,UInt32*bytes,void*out) {
 enter_api();assert(object==1 && a->mScope==10 && !a->mElement && *bytes==4 && out);
 if(a->mSelector==101) {
  assert(qualifier_bytes==sizeof(CFStringRef) && qualifier && !strcmp(*(const CFStringRef*)qualifier,kDeviceUID));
  if(lookup_error)return lookup_error;*(UInt32*)out=uid;*bytes=short_lookup?8:4;return 0;
 }
 assert(!qualifier_bytes && !qualifier && (a->mSelector==102 || a->mSelector==103));
 if(query_error)return query_error;*(UInt32*)out=defaults[a->mSelector-102];*bytes=short_default?8:4;return 0;
}
static OSStatus AudioObjectGetPropertyDataSize(AudioObjectID object,const AudioObjectPropertyAddress*a,
 UInt32 qualifier_bytes,const void*qualifier,UInt32*bytes) {
 enter_api();assert(object==1 && a->mSelector==104 && a->mScope==10 && !a->mElement && !qualifier_bytes && !qualifier);
 *bytes=device_list_bytes;return 0;
}
/* The previously verified eligibility reader is called once, after the last
 * coherent route sample. This fixture tests its caller, not its scalar reads. */
static void sample_default_eligibility(UInt32 epoch,io_connect_t connection,AudioObjectID target) {
 if(target && epoch==gEpoch && connection==gConnect && atomic_load(&gDiscoveryActive) && available())++eligibility_calls;
}
/* There is deliberately no AudioObjectSetPropertyData implementation: the
 * route observer must link and run without a global preference writer. */
'''

main = r'''
static void reset(void) {
 atomic_store(&gDiscoveryActive,1);gEpoch=7;gConnect=11;gSystemRouteSamples=gObservationCount=0;
 memset(gObservations,0,sizeof gObservations);memset(&history,0,sizeof history);
 alive=1;lookup_error=query_error=short_lookup=short_default=0;
 calls=callbacks=lose_on_call=reported=rejected=eligibility_calls=0;uid=75;defaults[0]=defaults[1]=36;device_list_bytes=8;
}
static int event(UInt32 selector,int result,UInt32 value) {
 for(UInt32 i=0;i<history.count;++i) {NaHalObservation*e=&history.records[i];
  if(e->kind==NaHalSystemRoute && e->selector==selector && e->result==result && e->value==value)return 1;}
 return 0;
}
int main(void) {
 unsigned cases=0;
 reset();sample_system_route();assert(calls==4 && callbacks==calls && defaults[0]==36 && defaults[1]==36);
 assert(event(101,0,75) && event(102,0,36) && event(103,0,36) && event(104,0,2));++cases;
 reset();for(unsigned i=0;i<16;++i)sample_system_route();assert(calls==32 && callbacks==32 && gSystemRouteSamples==8 && eligibility_calls==1);
 assert(defaults[0]==36 && defaults[1]==36 && history.count==8);++cases;
 reset();uid=0;sample_system_route();uid=75;for(unsigned i=1;i<8;++i)sample_system_route();
 assert(eligibility_calls==1 && defaults[0]==36 && defaults[1]==36 && event(101,0,75));++cases;
 reset();lookup_error=-80;for(unsigned i=0;i<8;++i)sample_system_route();assert(!eligibility_calls && event(101,-80,0) && defaults[0]==36);++cases;
 reset();short_lookup=1;sample_system_route();assert(event(101,-70,0) && defaults[0]==36);++cases;
 reset();query_error=-77;sample_system_route();assert(event(102,-77,0) && event(103,-77,0));query_error=0;
 sample_system_route();assert(event(102,0,36) && defaults[0]==36);++cases;
 reset();short_default=1;sample_system_route();assert(event(102,-70,0) && event(103,-70,0) && defaults[0]==36);++cases;
 reset();device_list_bytes=3;sample_system_route();assert(event(104,-70,0) && defaults[0]==36);++cases;
 reset();alive=0;sample_system_route();assert(!calls);alive=1;atomic_store(&gDiscoveryActive,0);sample_system_route();assert(!calls);++cases;
 reset();lose_on_call=1;sample_system_route();assert(calls==4 && !reported && !eligibility_calls && defaults[0]==36);++cases;
 reset();sample_system_route();defaults[0]=91;defaults[1]=51;for(unsigned i=1;i<8;++i)sample_system_route();
 assert(defaults[0]==91 && defaults[1]==51 && history.records[history.count-3].value==91 && history.records[history.count-2].value==51);++cases;
 reset();gObservationCount=history.count=NA_HAL_HISTORY_CAPACITY;
 for(UInt32 i=0;i<NA_HAL_HISTORY_CAPACITY;++i)history.records[i]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalPropertyRead,2,200+i,10,0,0,i};
 for(unsigned i=0;i<8;++i)sample_system_route();assert(calls==32 && eligibility_calls==1 && rejected==4 && defaults[0]==36 && defaults[1]==36);++cases;
 printf("%u actual route-observer cases passed; reentrant CoreAudio peers modeled, no global preference writer\n",cases);
 return 0;
}
'''

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    fixture = args.out / 'mobile-route-fixture.c'
    bodies = {name: function(name) for name in ('observe_once', 'sample_system_route')}
    fixture.write_text(prefix + '\n'.join(bodies.values()) + main)
    command = ['gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
               '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread',
               '-I',str(root),str(fixture),str(root/'core/AudioHalDiagnostics.c'),'-o',str(args.out/'mobile-route-test')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(args.out/'mobile-route-test')], capture_output=True, text=True)
    (args.out/'mobile-route-result.json').write_text(json.dumps(dict(returncode=result.returncode,
        stdout=result.stdout,stderr=result.stderr),indent=2)+'\n')
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    result.check_returncode()
    proof = dict(passed=True,cases=12,ASan_UBSan=True,CoreAudio_modeled=True,
                 phone_operations=False,global_preference_writer_linked=False,command=command,
                 extracted_functions={name:hashlib.sha256(body.encode()).hexdigest() for name,body in bodies.items()},
                 source_sha256=hashlib.sha256((root/'hal/NezhaAudioPlugIn.c').read_bytes()).hexdigest(),
                 receiver_sha256=hashlib.sha256((root/'core/AudioHalDiagnostics.c').read_bytes()).hexdigest(),
                 ABI_sha256=hashlib.sha256((root/'os/NezhaAudioAbi.h').read_bytes()).hexdigest())
    (args.out/'host-qualified.json').write_text(json.dumps(proof, indent=2)+'\n')
