#!/usr/bin/env python3
"""Open the real host Vulkan driver before compiling the frontend instrument.

Run006's crash report names MoltenVK1.4.2 argument-size discovery and the
paravirtual Metal driver's unimplemented argument encoder. The documented
MoltenVK option disables that host-only path; no phone capability is changed.
This is a device-open prerequisite, not a shader or pixel test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
out = a.logs / 'host-driver'
out.mkdir(parents=True, exist_ok=False)
loader = Path(os.environ['NMB_HOST_LOADER'])
assert loader.is_file()
assert os.environ.get('MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS') == '0'
assert 'NMB_DEVICE_BASE_NSOBJECT' not in os.environ
source = out / 'driver.cpp'
source.write_text(r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <vector>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!get) return 4;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return 5;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "NativeMetal host driver prerequisite";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = create(&ci, nullptr, &instance);
    std::printf("vkCreateInstance=%d\n", result);
    if (result != VK_SUCCESS) return 6;
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
    auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
    auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(get(instance, "vkGetPhysicalDeviceFeatures"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
    if (!enumerate || !properties || !features || !destroy) return 7;
    uint32_t count = 0;
    result = enumerate(instance, &count, nullptr);
    if (result != VK_SUCCESS || !count || count > 16) { destroy(instance, nullptr); return 8; }
    std::vector<VkPhysicalDevice> devices(count);
    result = enumerate(instance, &count, devices.data());
    if (result != VK_SUCCESS) { destroy(instance, nullptr); return 9; }
    std::printf("physical_devices=%u\n", count);
    for (auto device : devices) {
        VkPhysicalDeviceProperties props{};
        VkPhysicalDeviceFeatures feat{};
        properties(device, &props);
        features(device, &feat);
        std::printf("device=%s api=%u.%u.%u shaderInt16=%u multiViewport=%u maxStorageBuffers=%u\n",
            props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion), feat.shaderInt16, feat.multiViewport,
            props.limits.maxPerStageDescriptorStorageBuffers);
    }
    destroy(instance, nullptr);
    dlclose(lib);
    std::puts("PASS: actual host Vulkan instance and physical devices");
    return 0;
}
''')
headers = Path(subprocess.check_output(['brew', '--prefix', 'vulkan-headers'], text=True).strip()) / 'include'
binary = out / 'driver'
command = ['xcrun', 'clang++', '-std=c++17', '-O1', '-I', str(headers), str(source), '-o', str(binary)]
compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
(out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
if compiled.returncode:
    raise SystemExit('host driver prerequisite did not compile')
try:
    result = subprocess.run([str(binary), str(loader)], capture_output=True, text=True, timeout=90)
    code, log = result.returncode, result.stdout + result.stderr
except subprocess.TimeoutExpired:
    code, log = 'timeout', 'host Vulkan device open exceeded 90 seconds\n'
(out / 'open.log').write_text(log)
passed = code == 0 and 'PASS: actual host Vulkan instance' in log
report = {'passed': passed, 'exit_code': code, 'loader': str(loader.resolve()),
          'loader_sha256': hashlib.sha256(loader.read_bytes()).hexdigest(),
          'MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS': '0',
          'compiler_command': command, 'pixel_test_passed': False,
          'phone_operations': False, 'target_plugin_modified': False}
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(log, end='')
print(json.dumps(report, indent=2))
if not passed:
    raise SystemExit('real host Vulkan prerequisite failed; keep its diagnostics')
