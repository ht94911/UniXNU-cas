#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase 2 Mac CI builder for Adreno840Metal (arm64e iOS 27) & NezhaGpuTest.

Builds 100% Vulkan-free, Turnip-free artifacts from pinned public sources:
  1. Pinned Mesa 85fda3b36405 non-Vulkan ir3+fdl archives (host macOS + arm64e-apple-ios27.0)
  2. Public UniXNU/metal2vulkan (87a869f7fc4b) translator staticlib (host macOS + arm64e-apple-ios)
  3. Real Apple Metal 3.0 NezhaGpuTest.metallib (xcrun -sdk iphoneos metal + metallib)
  4. macOS host E2E test: Apple-compiled NezhaGpuTest.metallib -> translate -> ir3 -> A8XX PM4 -> pm4_check
  5. arm64e-apple-ios27.0 Adreno840Metal.bundle + NezhaGpuTest standalone probe (PAC-rewritten & 4K-signed)
  6. AppleClang strict check on Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp

Supports --dry-run on Linux to validate all source lists, flags, and manifest bindings without Xcode.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

MESA_COMMIT = "85fda3b36405bcaea8bffc64aa4f2651dc90770d"
MESA_TREE = "b72ff1eb2138026ed6aba524770899faabf7d7c8"
M2V_PUBLIC_COMMIT = "87a869f7fc4b73265d4812c40d8cccb6a83c7b17"
RUST_TOOLCHAIN = os.environ.get("RUST_TOOLCHAIN", "1.98.0")

MESA_PATCHES = (
    "ci/patches/mesa/ir3-darwin-build-id.patch",
    "ci/patches/mesa/freedreno-tools-nir.patch",
)

MESA_NON_VULKAN_OPTIONS = [
    "--backend=ninja",
    "--wrap-mode=nodownload",
    "-Dplatforms=[]",
    "-Dvulkan-drivers=[]",
    "-Dgallium-drivers=[]",
    "-Dtools=freedreno",
    "-Dopengl=false",
    "-Dglx=disabled",
    "-Degl=disabled",
    "-Dgbm=disabled",
    "-Dgles1=disabled",
    "-Dgles2=disabled",
    "-Dllvm=disabled",
    "-Dshared-glapi=disabled",
    "-Dxmlconfig=disabled",
    "-Dshader-cache=disabled",
    "-Dspirv-tools=disabled",
    "-Dbuild-tests=false",
    "-Dvideo-codecs=",
    "-Dlibunwind=disabled",
    "-Dlmsensors=disabled",
    "-Dvalgrind=disabled",
    "-Dexpat=disabled",
    "-Dzstd=disabled",
    "-Dbuildtype=release",
    "-Db_ndebug=true",
]

MESA_CORE_ARCHIVES = (
    "src/compiler/nir/libnir.a",
    "src/compiler/spirv/libvtn.a",
    "src/freedreno/ir3/libfreedreno_ir3.a",
    "src/freedreno/fdl/libfreedreno_layout.a",
    "src/freedreno/common/libfreedreno_common.a",
    "src/compiler/libcompiler.a",
    "src/freedreno/isa/libir3encode.a",
    "src/freedreno/isa/libir3decode.a",
    "src/compiler/isaspec/libisaspec.a",
    "src/util/libmesa_util.a",
)

MESA_OPTIONAL_SUBLIBS = (
    "src/util/libmesa_util_clflush.a",
    "src/util/libmesa_util_clflushopt.a",
    "src/util/libmesa_util_simd.a",
    "src/util/blake3/libblake3.a",
    "src/c11/impl/libmesa_util_c11.a",
)

PAC_RENAMES = {
    "_pthread_create": "_nmb_pac_pthread_create",
    "__tlv_atexit": "_nmb_pac_tlv_atexit",
    "_atexit": "_nmb_pac_atexit",
    "__dyld_register_func_for_add_image": "_nmb_pac_dyld_register_func_for_add_image",
    "__Unwind_Backtrace": "_nmb_pac_Unwind_Backtrace",
}

NATIVE_C_SOURCES = (
    "kits/Adreno840/chip_facts_a840.c",
    "kits/Adreno840/a840_context_init.c",
    "kits/Adreno840/a840_abi_v3_host.c",
    "kits/Adreno840Emit/a840_cs.c",
    "kits/Adreno840Emit/a840_descriptors.c",
    "kits/Adreno840Emit/a840_emit_blit.c",
    "kits/Adreno840Emit/a840_emit_sync.c",
    "kits/Adreno840Emit/a840_emit_compute.c",
    "kits/Adreno840Emit/a840_emit_3d.c",
    "adreno_compile/layout/a840_binding_plan.c",
    "adreno_compile/cache/a840_record_io.c",
    "drivers/Adreno840Metal/core/a840_dualpath_router.c",
    "drivers/Adreno840Metal/core/a840_runtime_device.c",
    "drivers/Adreno840Metal/core/a840_runtime_cmd.c",
    "drivers/Adreno840Metal/transport/a840_iokit_wire.c",
    "drivers/Adreno840Metal/transport/a840_hwtest_hook.c",
    "drivers/Adreno840Metal/transport/a840_gfx_bind_sds.c",
)

IR3_BACKEND_C_SOURCES = (
    "adreno_compile/ir3/a840_fdl_layout.c",
    "adreno_compile/ir3/a840_spirv_lower.c",
    "adreno_compile/ir3/adreno_compile_ir3.c",
)

NATIVE_OBJCXX_SOURCES = (
    "drivers/Adreno840Metal/transport/a840_iokit_transport.mm",
    "drivers/Adreno840Metal/objc/A840MTLDevice.mm",
    "drivers/Adreno840Metal/objc/A840MTLResource.mm",
    "drivers/Adreno840Metal/objc/A840MTLCommand.mm",
    "drivers/Adreno840Metal/objc/A840MTLEncoders.mm",
    "drivers/Adreno840Metal/objc/A840MTLPipelineProvider.mm",
)

BUNDLE_ONLY_OBJCXX = ("drivers/Adreno840Metal/objc/A840MTLBundleEntry.mm",)

TEST_APP_SOURCES = (
    "apps/NezhaGpuTest/ref/cpu_reference.cpp",
    "apps/NezhaGpuTest/ios/NezhaGpuTestRunner.mm",
    "apps/NezhaGpuTest/ios/main.mm",
)

KMD_CHECK_C_SOURCES = (
    "drivers/Adreno840Kernel/a840_kmd_zero_page.c",
    "drivers/Adreno840Kernel/a840_kmd_wait_any.c",
    "drivers/Adreno840Kernel/a840_kmd_fault_capture.c",
    "drivers/Adreno840Kernel/a840_kmd_userclient_v3.c",
    "drivers/Adreno840Kernel/a840_kmd_ib_diag.c",
)

INCLUDE_DIRS = (
    "contracts",
    "kits/Adreno840",
    "kits/Adreno840/gen",
    "kits/Adreno840Emit",
    "translate/include",
    "adreno_compile/ir3",
    "adreno_compile/layout",
    "adreno_compile/cache",
    "drivers/Adreno840Metal/core",
    "drivers/Adreno840Metal/transport",
    "drivers/Adreno840Metal/objc",
    "drivers/Adreno840Kernel",
    "apps/NezhaGpuTest/ref",
)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: list[str | Path], *, cwd: Path | None = None, env: dict[str, str] | None = None,
        log: Path | None = None) -> str:
    argv = [str(c) for c in cmd]
    print(f"$ {shlex.join(argv)}", flush=True)
    res = subprocess.run(argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    if log is not None:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(res.stdout, encoding="utf-8")
    if res.returncode != 0:
        tail = "\n".join(res.stdout.splitlines()[-60:])
        raise RuntimeError(f"exit {res.returncode}: {shlex.join(argv)}\n{tail}")
    return res.stdout


def read_tool(cmd: list[str | Path]) -> str:
    return subprocess.check_output([str(c) for c in cmd], text=True).strip()


def check_repo_inputs(repo: Path) -> dict:
    all_required = (
        list(NATIVE_C_SOURCES)
        + list(IR3_BACKEND_C_SOURCES)
        + list(NATIVE_OBJCXX_SOURCES)
        + list(BUNDLE_ONLY_OBJCXX)
        + list(TEST_APP_SOURCES)
        + list(KMD_CHECK_C_SOURCES)
        + [
            "drivers/Adreno840Metal/transport/A840RustPacBoundary.c",
            "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
            "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal",
            "translate/Cargo.toml",
            "translate/src/lib.rs",
            "tools/mac_ci/rust_pac_rewrite.py",
        ]
    )
    missing = [rel for rel in all_required if not (repo / rel).is_file()]
    if missing:
        raise RuntimeError(f"missing required Phase 2 Mac CI source files: {missing}")
    return {
        "required_source_count": len(all_required),
        "mesa_commit": MESA_COMMIT,
        "mesa_tree": MESA_TREE,
        "m2v_public_commit": M2V_PUBLIC_COMMIT,
        "vulkan_drivers": "disabled (zero src/freedreno/vulkan)",
        "targets": ["macos-host-e2e", "arm64e-apple-ios27.0/Adreno840Metal.bundle",
                    "arm64e-apple-ios27.0/NezhaGpuTest", "appleclang-kmd-v3-check"],
    }


def meson_include_flags(repo: Path, mesa_src: Path, mesa_build: Path) -> list[str]:
    flags = [
        "-I", str(repo / "contracts"),
        "-I", str(repo / "kits/Adreno840"),
        "-I", str(repo / "adreno_compile/ir3"),
    ]
    mesa_dirs = (
        mesa_src / "include",
        mesa_src / "src",
        mesa_src / "src/compiler",
        mesa_src / "src/compiler/isaspec",
        mesa_src / "src/compiler/nir",
        mesa_src / "src/compiler/spirv",
        mesa_src / "src/freedreno",
        mesa_src / "src/freedreno/common",
        mesa_src / "src/freedreno/ir3",
        mesa_src / "src/freedreno/fdl",
        mesa_src / "src/freedreno/isa",
        mesa_src / "src/freedreno/registers",
        mesa_src / "src/util",
        mesa_src / "src/util/format",
        mesa_build / "src",
        mesa_build / "src/compiler",
        mesa_build / "src/compiler/nir",
        mesa_build / "src/compiler/spirv",
        mesa_build / "src/freedreno/common",
        mesa_build / "src/freedreno/registers",
        mesa_build / "src/freedreno/registers/adreno",
        mesa_build / "src/freedreno/isa",
        mesa_build / "src/util",
        mesa_build / "src/util/format",
    )
    for d in mesa_dirs:
        flags.extend(["-isystem", str(d)])
    return flags


def extract_mesa_defines(mesa_build: Path) -> list[str]:
    compdb = json.loads((mesa_build / "compile_commands.json").read_text(encoding="utf-8"))
    for entry in compdb:
        if "freedreno/ir3/" in entry.get("file", "").replace("\\", "/"):
            tokens = shlex.split(entry["command"])
            defs = ["-DA840_BUILD_MESA_IR3_BACKEND=1"]
            for tok in tokens:
                if tok.startswith("-D") and tok not in defs:
                    defs.append(tok)
            return defs
    raise RuntimeError("could not find freedreno/ir3 entry in Mesa compile_commands.json")


def build_mesa_ir3(repo: Path, mesa_src: Path, work: Path, meson_bin: Path, target: str,
                   sdk: str | None, jobs: int) -> tuple[Path, list[Path], Path]:
    build_dir = work / f"mesa-ir3-{target}"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    setup_cmd: list[str | Path] = [meson_bin, "setup", build_dir, mesa_src, *MESA_NON_VULKAN_OPTIONS]
    env = os.environ.copy()
    macos_sdk = read_tool(["xcrun", "--sdk", "macosx", "--show-sdk-path"])
    if target == "ios27":
        assert sdk is not None
        cc = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "clang"])
        cxx = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "clang++"])
        ar = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "ar"])
        strip = read_tool(["xcrun", "--sdk", "iphoneos", "--find", "strip"])
        flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-fstack-protector-strong"]
        cross = work / "mesa-ir3-ios27.cross"
        cross.write_text(
            f"[binaries]\nc = {cc!r}\ncpp = {cxx!r}\nobjc = {cc!r}\nar = {ar!r}\nstrip = {strip!r}\n"
            f"pkg-config = '/usr/bin/false'\n"
            f"[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n"
            f"[properties]\nneeds_exe_wrapper = true\n[built-in options]\n"
            + "".join(f"{k} = {flags!r}\n" for k in ("c_args", "cpp_args", "objc_args",
                                                     "c_link_args", "cpp_link_args", "objc_link_args")),
            encoding="utf-8",
        )
        setup_cmd += ["--cross-file", cross]
        env.update(CC="/usr/bin/clang", CXX="/usr/bin/clang++", SDKROOT=macos_sdk,
                   CFLAGS="", CXXFLAGS="", CPPFLAGS="", LDFLAGS="")
    else:
        cc = "/usr/bin/clang"
        cxx = "/usr/bin/clang++"
        ar = "/usr/bin/ar"
        env.update(CC=cc, CXX=cxx, SDKROOT=macos_sdk)

    run(setup_cmd, env=env, log=work / f"logs/meson-setup-{target}.log")

    # Select only existing non-Vulkan targets from build.ninja.
    ninja_text = (build_dir / "build.ninja").read_text(encoding="utf-8", errors="replace")
    if "src/freedreno/vulkan/" in ninja_text:
        raise RuntimeError("FORBIDDEN src/freedreno/vulkan detected in Meson build.ninja")
    targets = list(MESA_CORE_ARCHIVES) + [s for s in MESA_OPTIONAL_SUBLIBS if s in ninja_text]
    run(["ninja", "-C", build_dir, f"-j{jobs}", *targets], env=env,
        log=work / f"logs/ninja-mesa-{target}.log")

    libs = [build_dir / t for t in targets]
    defs = extract_mesa_defines(build_dir)
    inc = meson_include_flags(repo, mesa_src, build_dir)
    obj_dir = work / f"ir3-obj-{target}"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs = []
    compile_env = env.copy()
    if target == "ios27":
        compile_env.pop("SDKROOT", None)
    target_flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-fstack-protector-strong"] if target == "ios27" else []
    for rel in IR3_BACKEND_C_SOURCES:
        src = repo / rel
        obj = obj_dir / (src.stem + ".o")
        run([cc, *target_flags, "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", *defs, *inc, "-c", src, "-o", obj], env=compile_env)
        objs.append(obj)
    archive = work / f"libadreno_compile_ir3_{target}.a"
    archive.unlink(missing_ok=True)
    run([ar, "rcs", archive, *objs], env=compile_env)
    return archive, libs, build_dir


def build_rust_translate(repo: Path, m2v_dir: Path, work: Path, target: str) -> Path:
    build_src = work / f"translate-src-{target}"
    if build_src.exists():
        shutil.rmtree(build_src)
    shutil.copytree(repo / "translate", build_src)
    cargo_toml = build_src / "Cargo.toml"
    text = cargo_toml.read_text(encoding="utf-8")
    crate_path = (m2v_dir / "crates/metal2vulkan").resolve()
    text = re.sub(
        r'metal2vulkan\s*=\s*\{[^}]*\}',
        f'metal2vulkan = {{ path = {json.dumps(str(crate_path))}, default-features = false }}',
        text,
    )
    if "[workspace]" not in text:
        text = text.rstrip() + "\n\n[workspace]\n"
    cargo_toml.write_text(text, encoding="utf-8")

    host_arch = platform.machine()
    host_triple = "x86_64-apple-darwin" if host_arch == "x86_64" else "aarch64-apple-darwin"
    triple = "arm64e-apple-ios" if target == "ios27" else host_triple
    env = os.environ.copy()
    env.update(
        CARGO_TARGET_DIR=str(work / "cargo-target"),
        MACOSX_DEPLOYMENT_TARGET="14.0",
        IPHONEOS_DEPLOYMENT_TARGET="27.0",
    )
    cargo_cmd: list[str | Path] = ["cargo", f"+{RUST_TOOLCHAIN}"]
    if target == "ios27":
        env["RUSTC_BOOTSTRAP"] = "1"
        cargo_cmd += ["-Z", "build-std=std,panic_unwind"]
    cargo_cmd += ["build", "--release", "--manifest-path", cargo_toml, "--target", triple]
    run(cargo_cmd, cwd=build_src, env=env, log=work / f"logs/cargo-{target}.log")
    lib = work / "cargo-target" / triple / "release/libnative_metal_translate.a"
    if not lib.is_file():
        raise RuntimeError(f"missing Rust staticlib: {lib}")
    return lib


def sign_4k(target: Path, identifier: str, entitlements: Path | None = None) -> str:
    cmd: list[str | Path] = ["codesign", "--force", "--sign", "-", "--pagesize", "4096",
                             "--identifier", identifier]
    if entitlements is not None:
        cmd += ["--entitlements", entitlements]
    cmd.append(target)
    run(cmd)
    run(["codesign", "--verify", "--strict", target])
    detail = subprocess.run(["codesign", "--display", "--verbose=4", str(target)],
                            capture_output=True, text=True, check=True).stderr
    if not re.search(r"^Page size=4096$", detail, re.M):
        raise RuntimeError(f"4 KiB page signature missing on {target}:\n{detail}")
    m = re.search(r"^CDHash=([a-f0-9]{40})$", detail, re.M)
    if not m:
        raise RuntimeError(f"CDHash missing on {target}:\n{detail}")
    return m.group(1)


def compile_macos_host_e2e_objs(repo: Path, work: Path) -> list[Path]:
    harness = work / "host_e2e_driver.mm"
    harness.write_text(
        r'''/* SPDX-License-Identifier: MIT */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "A840MTLPipelineProvider.h"
#include "a840_backend_transport.h"
#include "a840_hwtest_hook.h"
#include <cstdio>
#include <cstring>

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc < 3) { std::fprintf(stderr, "usage: host_e2e <metallib> <pm4_out>\n"); return 2; }
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = true;
        A840MockTransport *mock = nullptr;
        if (a840_mock_transport_create(&cfg, &mock) != A840_TRANSPORT_OK || !mock) return 10;
        NSError *err = nil;
        id<MTLDevice> dev = a840_metal_create_mock_device(a840_mock_transport_interface(mock), &err);
        if (!dev) { std::fprintf(stderr, "mock device failed: %s\n", err.description.UTF8String); return 11; }
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&err];
        if (!lib) { std::fprintf(stderr, "metallib load failed: %s\n", err.description.UTF8String); return 12; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        if (!q) return 13;

        /* 1. Compute add_vec end-to-end (real Apple metallib -> translate -> ir3 -> A8XX PM4) */
        id<MTLFunction> fnAdd = [lib newFunctionWithName:@"add_vec"];
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:fnAdd error:&err];
        if (!cps) { std::fprintf(stderr, "add_vec pipeline failed\n"); return 14; }
        id<MTLBuffer> bufA = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufB = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufOut = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        uint32_t count = 64u; float mult = 2.5f;
        id<MTLCommandBuffer> cb1 = [q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb1 computeCommandEncoder];
        [ce setComputePipelineState:cps];
        [ce setBuffer:bufA offset:0 atIndex:0];
        [ce setBuffer:bufB offset:0 atIndex:1];
        [ce setBuffer:bufOut offset:0 atIndex:2];
        [ce setBytes:&count length:sizeof(count) atIndex:3];
        [ce setBytes:&mult length:sizeof(mult) atIndex:4];
        [ce dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [ce endEncoding];
        [cb1 commit];
        [cb1 waitUntilCompleted];
        const char *r1 = a840_hwtest_path_for_command_buffer((__bridge void *)cb1);
        if (!r1 || std::strcmp(r1, "raw") != 0) { std::fprintf(stderr, "compute route=%s\n", r1 ? r1 : "null"); return 15; }

        /* 2. Render test_vertex + triangle_fragment end-to-end */
        MTLRenderPipelineDescriptor *rpd = [MTLRenderPipelineDescriptor new];
        rpd.vertexFunction = [lib newFunctionWithName:@"test_vertex"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"triangle_fragment"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!rps) { std::fprintf(stderr, "render pipeline failed\n"); return 16; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64 height:64 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLBuffer> vbuf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        float extent[2] = {64.0f, 64.0f};
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.colorAttachments[0].texture = rt;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb2 = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb2 renderCommandEncoderWithDescriptor:pass];
        [re setRenderPipelineState:rps];
        [re setVertexBuffer:vbuf offset:0 atIndex:0];
        [re setVertexBytes:extent length:sizeof(extent) atIndex:1];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
        [cb2 commit];
        [cb2 waitUntilCompleted];
        const char *r2 = a840_hwtest_path_for_command_buffer((__bridge void *)cb2);
        if (!r2 || std::strcmp(r2, "raw") != 0) { std::fprintf(stderr, "render route=%s\n", r2 ? r2 : "null"); return 17; }

        /* 3. Compile all remaining NezhaGpuTest.metal shaders to verify 100% Apple->ir3 coverage */
        for (NSString *k in @[@"nonuniform_grid", @"clear_bgra8", @"scale_texture", @"fence_step", @"fence_verify"]) {
            id<MTLFunction> fn = [lib newFunctionWithName:k];
            if (!fn || ![dev newComputePipelineStateWithFunction:fn error:&err]) {
                std::fprintf(stderr, "kernel %s failed\n", k.UTF8String);
                return 18;
            }
        }
        rpd.fragmentFunction = [lib newFunctionWithName:@"textured_fragment"];
        if (![dev newRenderPipelineStateWithDescriptor:rpd error:&err]) {
            std::fprintf(stderr, "textured_fragment pipeline failed\n");
            return 19;
        }

        const size_t dw = a840_mock_transport_total_captured_dwords(mock);
        if (dw == 0u || a840_mock_transport_dump_pm4(mock, argv[2]) != A840_TRANSPORT_OK) return 20;
        std::printf("PASS: macOS host E2E compiled all 9 Apple Metal shaders + emitted %zu A8XX PM4 dwords (all path=raw)\n", dw);
        dev = nil;
        a840_mock_transport_destroy(mock);
        return 0;
    }
}
''',
        encoding="utf-8",
    )
    inc = [f"-I{repo / d}" for d in INCLUDE_DIRS]
    obj_dir = work / "host-e2e-obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs: list[Path] = []
    for rel in list(NATIVE_C_SOURCES) + ["drivers/Adreno840Metal/core/a840_mock_transport.c"]:
        src = repo / rel
        obj = obj_dir / (src.stem + ".o")
        run(["xcrun", "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror", *inc, "-c", src, "-o", obj])
        objs.append(obj)
    for src in [repo / r for r in NATIVE_OBJCXX_SOURCES] + [harness]:
        obj = obj_dir / (src.stem + "_mm.o")
        run(["xcrun", "clang++", "-std=c++17", "-fobjc-arc", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
             "-Wno-deprecated-declarations", *inc, "-x", "objective-c++", "-c", src, "-o", obj])
        objs.append(obj)
    return objs


def run_macos_host_e2e(repo: Path, mesa_src: Path, work: Path, out: Path,
                       objs: list[Path], ir3_lib: Path, mesa_libs: list[Path],
                       translate_lib: Path, metallib: Path) -> dict:
    exe = work / "host_e2e_driver"
    run(["xcrun", "clang++", "-O2", "-g", *objs, ir3_lib, *mesa_libs, translate_lib,
         "-lobjc", "-lz", "-framework", "Foundation", "-framework", "Metal", "-framework", "IOSurface",
         "-o", exe])
    pm4_out = out / "host_e2e_apple_metallib.pm4"
    pm4_out.parent.mkdir(parents=True, exist_ok=True)
    stdout = run([exe, metallib, pm4_out])
    pm4_report = run(["python3", "-B", repo / "tools/verify/pm4_check.py",
                      "--mesa", mesa_src, "--chip", "A8XX", "--format", "binary", pm4_out])
    return {"stdout": stdout.strip(), "pm4_check": pm4_report.strip(),
            "pm4_bytes": pm4_out.stat().st_size, "pm4_sha256": sha256_file(pm4_out)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--mesa", type=Path)
    parser.add_argument("--m2v", type=Path)
    parser.add_argument("--meson", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    plan = check_repo_inputs(repo)
    if args.dry_run:
        print(json.dumps({"status": "DRY_RUN_PASS", **plan}, indent=2))
        return 0

    if platform.system() != "Darwin":
        raise SystemExit("Full build requires macOS with Xcode; use --dry-run on Linux")
    assert args.mesa and args.m2v and args.meson
    work = args.work.resolve()
    out = args.out.resolve()
    (work / "logs").mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)

    # Verify exact pinned upstream commits.
    if read_tool(["git", "-C", args.mesa, "rev-parse", "HEAD"]) != MESA_COMMIT:
        raise RuntimeError("Mesa commit mismatch")
    if read_tool(["git", "-C", args.m2v, "rev-parse", "HEAD"]) != M2V_PUBLIC_COMMIT:
        raise RuntimeError("metal2vulkan public commit mismatch")

    sdk = read_tool(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"])

    # 1. Compile real Apple Metal 3.0 shader library (NezhaGpuTest.metallib).
    air = work / "NezhaGpuTest.air"
    metallib = out / "NezhaGpuTest.metallib"
    run(["xcrun", "--sdk", "iphoneos", "metal", "-std=metal3.0", "-O2",
         "-c", repo / "apps/NezhaGpuTest/shaders/NezhaGpuTest.metal", "-o", air])
    run(["xcrun", "--sdk", "iphoneos", "metallib", air, "-o", metallib])

    # 2. Fail-fast compile of ALL our macOS host, KMD v3, and arm64e iOS 27 C/C++/ObjC++ units
    #    before running the longer Mesa ir3 and Rust translate builds.
    inc = [f"-I{repo / d}" for d in INCLUDE_DIRS]
    host_e2e_objs = compile_macos_host_e2e_objs(repo, work)

    kmd_dir = work / "kmd-check"
    kmd_dir.mkdir(parents=True, exist_ok=True)
    for rel in KMD_CHECK_C_SOURCES:
        run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             *inc, "-c", repo / rel, "-o", kmd_dir / (Path(rel).stem + ".o")])
    run(["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
         *inc, "-c", repo / "drivers/Adreno840Kernel/xnu/NativeAdrenoV3Glue.cpp",
         "-o", kmd_dir / "NativeAdrenoV3Glue.o"])

    ios_flags = ["-target", "arm64e-apple-ios27.0", "-isysroot", sdk, "-O2", "-g",
                 "-Wall", "-Wextra", "-Werror", "-fstack-protector-strong"]
    ios_obj = work / "ios27-obj"
    ios_obj.mkdir(parents=True, exist_ok=True)
    c_objs: list[Path] = []
    for rel in list(NATIVE_C_SOURCES) + ["drivers/Adreno840Metal/transport/A840RustPacBoundary.c"]:
        src_path = repo / rel
        obj = ios_obj / (src_path.stem + ".o")
        run(["xcrun", "clang", *ios_flags, "-std=c11", *inc, "-c", src_path, "-o", obj])
        c_objs.append(obj)
    mm_objs: list[Path] = []
    for rel in NATIVE_OBJCXX_SOURCES:
        src_path = repo / rel
        obj = ios_obj / (src_path.stem + "_mm.o")
        run(["xcrun", "clang++", *ios_flags, "-std=c++17", "-fobjc-arc", "-Wno-deprecated-declarations",
             *inc, "-x", "objective-c++", "-c", src_path, "-o", obj])
        mm_objs.append(obj)
    bundle_entry_obj = ios_obj / "A840MTLBundleEntry_mm.o"
    run(["xcrun", "clang++", *ios_flags, "-std=c++17", "-fobjc-arc", "-Wno-deprecated-declarations",
         *inc, "-x", "objective-c++", "-c", repo / BUNDLE_ONLY_OBJCXX[0], "-o", bundle_entry_obj])
    app_objs: list[Path] = []
    for rel in TEST_APP_SOURCES:
        src_path = repo / rel
        obj = ios_obj / (src_path.stem + "_app.o")
        if src_path.suffix == ".mm":
            run(["xcrun", "clang++", *ios_flags, "-std=c++17", "-fobjc-arc", "-Wno-deprecated-declarations",
                 *inc, "-x", "objective-c++", "-c", src_path, "-o", obj])
        else:
            run(["xcrun", "clang++", *ios_flags, "-std=c++17", *inc, "-c", src_path, "-o", obj])
        app_objs.append(obj)

    # 3. Build macOS host ir3 + translate and run Apple-metallib host E2E + pm4_check.
    host_ir3, host_mesa_libs, _ = build_mesa_ir3(repo, args.mesa, work, args.meson, "host", None, args.jobs)
    host_translate = build_rust_translate(repo, args.m2v, work, "host")
    host_e2e = run_macos_host_e2e(repo, args.mesa, work, out, host_e2e_objs,
                                  host_ir3, host_mesa_libs, host_translate, metallib)

    # 4. Cross-compile arm64e-apple-ios27.0 Mesa non-Vulkan ir3+fdl & Rust translate + PAC boundary.
    ios_ir3, ios_mesa_libs, _ = build_mesa_ir3(repo, args.mesa, work, args.meson, "ios27", sdk, args.jobs)
    ios_translate = build_rust_translate(repo, args.m2v, work, "ios27")

    objcopy = next((p for p in ["/opt/homebrew/opt/llvm/bin/llvm-objcopy",
                                "/usr/local/opt/llvm/bin/llvm-objcopy",
                                shutil.which("llvm-objcopy") or ""] if p and Path(p).is_file()), None)
    if not objcopy:
        raise RuntimeError("llvm-objcopy required for arm64e Rust PAC symbol redefinition")
    pac_translate = work / "libnative_metal_translate_pac.a"
    run([objcopy, *[f"--redefine-sym={a}={b}" for a, b in PAC_RENAMES.items()], ios_translate, pac_translate])

    llvm_nm = next((p for p in ["/opt/homebrew/opt/llvm/bin/llvm-nm",
                                "/usr/local/opt/llvm/bin/llvm-nm",
                                shutil.which("llvm-nm") or ""] if p and Path(p).is_file()), None)
    nofb_env = os.environ.copy()
    if llvm_nm:
        nm_shim = work / "llvm-nm-bin"
        nm_shim.mkdir(parents=True, exist_ok=True)
        nm_link = nm_shim / "nm"
        nm_link.unlink(missing_ok=True)
        nm_link.symlink_to(llvm_nm)
        nofb_env["PATH"] = f"{nm_shim}:{nofb_env.get('PATH', '')}"

    # 5. Link, PAC-rewrite, strip, and 4K-sign Adreno840Metal.bundle (arm64e).
    bundle = out / "Adreno840Metal.bundle"
    if bundle.exists():
        shutil.rmtree(bundle)
    bundle.mkdir(parents=True)
    bundle_exe = bundle / "Adreno840Metal"
    bundle_map = work / "Adreno840Metal.map"
    run(["xcrun", "clang++", *ios_flags, "-bundle", *c_objs, *mm_objs, bundle_entry_obj,
         ios_ir3, *ios_mesa_libs, pac_translate, f"-Wl,-map,{bundle_map}", "-Wl,-dead_strip",
         "-lobjc", "-lz", "-framework", "Foundation", "-framework", "Metal", "-framework", "IOSurface",
         "-o", bundle_exe])
    run(["python3", "-B", repo / "tools/mac_ci/rust_pac_rewrite.py", bundle_exe, bundle_map, pac_translate.name])
    run(["xcrun", "strip", "-x", bundle_exe])
    bundle_dsym = bundle / "Adreno840Metal.dSYM"
    if bundle_dsym.exists():
        shutil.rmtree(work / "Adreno840Metal.dSYM", ignore_errors=True)
        shutil.move(bundle_dsym, work / "Adreno840Metal.dSYM")
    run(["python3", "-B", repo / "tools/verify/no_fallback_test.py", "--profile", "final", "--artifact", bundle_exe],
        env=nofb_env)
    (bundle / "Info.plist").write_bytes(plistlib.dumps({
        "CFBundleIdentifier": "org.nezha.Adreno840Metal",
        "CFBundleExecutable": "Adreno840Metal",
        "CFBundlePackageType": "BNDL",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "0.2.0",
        "NSPrincipalClass": "Adreno840MetalPrincipal",
        "MinimumOSVersion": "27.0",
    }, fmt=plistlib.FMT_XML))
    bundle_cdhash = sign_4k(bundle, "org.nezha.Adreno840Metal")

    # 6. Link, PAC-rewrite, strip, and 4K-sign standalone NezhaGpuTest probe (arm64e).
    app_exe = out / "NezhaGpuTest"
    app_map = work / "NezhaGpuTest.map"
    run(["xcrun", "clang++", *ios_flags, *c_objs, *mm_objs, *app_objs,
         ios_ir3, *ios_mesa_libs, pac_translate, f"-Wl,-map,{app_map}", "-Wl,-dead_strip",
         "-lobjc", "-lz", "-framework", "Foundation", "-framework", "Metal", "-framework", "IOSurface",
         "-o", app_exe])
    run(["python3", "-B", repo / "tools/mac_ci/rust_pac_rewrite.py", app_exe, app_map, pac_translate.name])
    run(["xcrun", "strip", "-x", app_exe])
    app_dsym = out / "NezhaGpuTest.dSYM"
    if app_dsym.exists():
        shutil.rmtree(work / "NezhaGpuTest.dSYM", ignore_errors=True)
        shutil.move(app_dsym, work / "NezhaGpuTest.dSYM")
    run(["python3", "-B", repo / "tools/verify/no_fallback_test.py", "--profile", "final", "--artifact", app_exe],
        env=nofb_env)
    entitlements = work / "NezhaGpuTest.entitlements"
    entitlements.write_bytes(plistlib.dumps({
        "platform-application": True,
        "com.apple.private.security.no-sandbox": True,
        "com.apple.private.security.no-container": True,
        "com.apple.security.iokit-user-client-class": ["AGXDeviceUserClient"],
    }, fmt=plistlib.FMT_XML))
    app_cdhash = sign_4k(app_exe, "org.nezha.NezhaGpuTest", entitlements=entitlements)

    summary = {
        "status": "PASS",
        "hardware_verified": False,
        "target": "arm64e-apple-ios27.0",
        "mesa_commit": MESA_COMMIT,
        "m2v_public_commit": M2V_PUBLIC_COMMIT,
        "host_e2e": host_e2e,
        "kmd_v3_appleclang": "PASS",
        "outputs": {
            "Adreno840Metal.bundle": {"cdhash": bundle_cdhash, "page_size": 4096,
                                      "exe_bytes": bundle_exe.stat().st_size,
                                      "exe_sha256": sha256_file(bundle_exe)},
            "NezhaGpuTest": {"cdhash": app_cdhash, "page_size": 4096,
                             "bytes": app_exe.stat().st_size,
                             "sha256": sha256_file(app_exe)},
            "NezhaGpuTest.metallib": {"bytes": metallib.stat().st_size,
                                      "sha256": sha256_file(metallib)},
        },
    }
    (out / "phase2_mac_build_summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
