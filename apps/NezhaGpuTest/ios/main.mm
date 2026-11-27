/* SPDX-License-Identifier: MIT */
/* Apple SDK compilation and arm64e signing are pending Wave 4 / Mac CI. */
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstring>

extern "C" int NezhaGpuTestRun(const char *suite, const char *case_id,
                              const char *json_out, const char *metallib);

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        const char *suite = "phase2", *case_id = nullptr, *output = nullptr, *metallib = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--help") == 0) {
                std::puts("usage: NezhaGpuTest --suite phase2 [--case <id>] [--json-out <path>] [--metallib <path>]");
                return 0;
            }
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); return 2; }
            if (std::strcmp(argv[i], "--suite") == 0) suite = argv[++i];
            else if (std::strcmp(argv[i], "--case") == 0) case_id = argv[++i];
            else if (std::strcmp(argv[i], "--json-out") == 0) output = argv[++i];
            else if (std::strcmp(argv[i], "--metallib") == 0) metallib = argv[++i];
            else { std::fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
        }
        return NezhaGpuTestRun(suite, case_id, output, metallib);
    }
}
