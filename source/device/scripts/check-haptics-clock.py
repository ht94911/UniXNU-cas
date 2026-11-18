#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise production HAL clock admission and the reused exact converter words."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import json


def function(source, name):
    start = source.index('static int ' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    device = Path(__file__).resolve().parents[1]
    source = device / 'package/haptics27'
    kernel = (source / 'NezhaHapticsClock.cpp').read_text()
    hal = (source / 'hal/NezhaHapticsPlugIn.c').read_text()
    begin = kernel.index('constexpr uint32_t convertersOriginal')
    end = kernel.index('static void synchronize_code')
    helpers = kernel[begin:end]
    words = [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', helpers.split('};', 1)[0])]
    assert len(words) == 23
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <atomic>
#include <initializer_list>
#include "NezhaHapticsProtocol.h"
typedef double Float64;typedef uint64_t UInt64;
static double hz;static uint32_t numer,denom;static int refused;static unsigned calls,exports;
static std::atomic<unsigned> clockReady,clockHz,clockNanos,clockTicks;
#define atomic_store(x,y) (x)->store(y)
static double freq(void){return hz;}
static uint64_t ns(uint64_t t){return (uint64_t)((__uint128_t)t*numer/denom);}
static uint64_t ticks(uint64_t n){return (uint64_t)((__uint128_t)n*denom/numer);}
static void *lookup(int h,const char*n){(void)h;if(!strcmp(n,"AudioGetHostClockFrequency"))return exports&1?(void*)freq:NULL;if(!strcmp(n,"AudioConvertHostTimeToNanos"))return exports&2?(void*)ns:NULL;return exports&4?(void*)ticks:NULL;}
#define dlsym lookup
#define RTLD_DEFAULT 0
static int fix(int fd,unsigned long c){assert(fd==77&&c==NZ_HAPTICS_FIX_HOST_CLOCK);++calls;if(refused){errno=EPERM;return -1;}hz=19200000;numer=625;denom=12;return 0;}
#define ioctl fix
static void reset(){hz=24000000;numer=125;denom=3;refused=0;calls=0;exports=7;clockReady=clockHz=clockNanos=clockTicks=0;}
'''
    cases = r'''
int main(){
reset();assert(!prepareClock(77,625,12)&&calls==1&&clockReady==1&&clockHz==19200000&&clockNanos==1000000000&&clockTicks==19200000);
assert(!prepareClock(77,625,12)&&calls==1);
reset();hz=19200000;assert(!prepareClock(77,625,12)&&calls==1);
reset();refused=1;assert(prepareClock(77,625,12)==EPERM&&!clockReady);
reset();hz=NAN;assert(prepareClock(77,625,12)==EINVAL&&!calls);
reset();exports=3;assert(prepareClock(77,625,12)==ENOTSUP&&!calls);
reset();assert(prepareClock(77,0,12)==EINVAL&&!calls);
reset();assert(!prepareClock(77,125,3)&&!calls&&clockHz==24000000);
uint32_t corrected[23],bad[23];unsigned changed=0;
for(unsigned i=0;i<23;++i){corrected[i]=converter_word(i,true);changed+=corrected[i]!=convertersOriginal[i];}
assert(changed==5&&converters_match(convertersOriginal,false)&&converters_match(corrected,true));
memcpy(bad,corrected,sizeof bad);bad[0]^=1;assert(!converters_match(bad,true));
memcpy(bad,corrected,sizeof bad);bad[20]^=1;assert(!converters_match(bad,true));
assert(corrected[17]==0x9b087c00);
for(uint64_t t:{0ULL,1ULL,19200000ULL,12200000000ULL,1ULL<<40}){
uint64_t n=(uint64_t)((__uint128_t)t*625/12),b=(uint64_t)((__uint128_t)n*12/625);assert(b<=t&&t-b<=1);}
// A UIKit absolute event at284s must address the same clock as the LRA HAL.
assert((284000000000ULL*3/125)/19200000ULL==355);
assert((284000000000ULL*12/625)/19200000ULL==284);
puts("PASS production haptic clock: three APIs, original/corrected/literal-only/denied/missing/24MHz, five exact edits and absolute event alignment");
}
'''
    program = args.out / 'clock.cpp'
    program.write_text(prefix + helpers + function(hal, 'prepareClock') + cases)
    command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(source), str(program),
               '-o', str(args.out / 'clock')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(args.out / 'clock')], capture_output=True, text=True, check=True)
    (args.out / 'execution.log').write_text(result.stdout + result.stderr)
    report = dict(passed=True, production_HAL_and_converter_helpers=True, exact_converter_words=23,
                  changed_converter_words=5, audio072_reuse='7c065c861cf1e565095ae50697743ff9edc14a93',
                  kernel_clock_sha256=hashlib.sha256(kernel.encode()).hexdigest(),
                  HAL_sha256=hashlib.sha256(hal.encode()).hexdigest(),
                  VM_and_cache_synchronization_not_executed=True, phone_operations=False)
    (args.out / 'qualified.json').write_text(json.dumps(report, indent=2) + '\n')
    print(result.stdout, end='')


if __name__ == '__main__':
    main()
