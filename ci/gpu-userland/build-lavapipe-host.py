"""Build Mesa's existing CPU Vulkan ICD for the source-bound Mac frontend tests."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args(); root = a.unixnu.resolve()
source, out = root/'.cache/mesa-work', root/'build/lavapipe-host'
logs = a.logs.resolve()/'lavapipe-host'; logs.mkdir(parents=True,exist_ok=True)
assert sys.platform == 'darwin' and not out.exists()
assert (source/'src/gallium/frontends/lavapipe/lvp_device.c').is_file()
# The CPU driver also uses cross-tree GLAPI inputs. Use the complete existing
# pinned checkout for this host dependency and preserve every applied GPU patch.
before = subprocess.check_output(['git','-C',str(source),'diff','HEAD','--binary'])
subprocess.run(['git','-C',str(source),'sparse-checkout','disable'], check=True)
assert subprocess.check_output(['git','-C',str(source),'diff','HEAD','--binary']) == before
clock_patch = Path(__file__).with_name('patches')/'lavapipe-host-clock.patch'
subprocess.run(['git','-C',str(source),'apply','--check',str(clock_patch)], check=True)
target = source/'src/gallium/targets/lavapipe/meson.build'
target_before = target.read_bytes()
# Apple MTLCompiler and the host CPU driver must not share LLVM symbols.
# Keep LLVM in the ICD and expose only Mesa's three existing loader entries.
icd_exports = ('vk_icdGetInstanceProcAddr', 'vk_icdGetPhysicalDeviceProcAddr',
               'vk_icdNegotiateLoaderICDInterfaceVersion')
exports = logs/'host-icd.exports'
exports.write_text(''.join('_'+name+'\n' for name in icd_exports))
link_args = 'link_args : [ld_args_bsymbolic, ld_args_gc_sections],'
target_text = target_before.decode()
assert target_text.count(link_args) == 1
target_text = target_text.replace(link_args,
    'link_args : [ld_args_bsymbolic, ld_args_gc_sections, '+
    "'-Wl,-exported_symbols_list,"+str(exports)+"'],", 1)
meson = root/'.cache/mesa-venv/bin/meson'
env = dict(os.environ)
env['PATH'] = str(root/'.cache/mesa-tools/bin')+':'+str(root/'.cache/mesa-venv/bin')+':'+env['PATH']
env['CC'], env['CXX'] = 'xcrun clang', 'xcrun clang++'
zstd_lib = Path(subprocess.check_output(['pkg-config','--variable=libdir','libzstd'], text=True).strip())
assert zstd_lib.is_dir(), 'installed LLVM system dependency libzstd is missing'
options = ['-Dc_args=-DCLOCK_MONOTONIC_RAW=CLOCK_MONOTONIC_RAW',
    '-Dc_link_args=-L'+str(zstd_lib), '-Dcpp_link_args=-L'+str(zstd_lib),
    '-Dgallium-drivers=llvmpipe', '-Dvulkan-drivers=swrast', '-Dplatforms=',
    '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dopengl=false',
    '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=enabled', '-Dshared-llvm=disabled',
    '-Dshared-glapi=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=enabled',
    '-Dtools=', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=']
commands = [([str(meson),'setup',str(out),str(source),'--wrap-mode=nodownload','--buildtype=release',*options], 'configure'),
            (['ninja','-C',str(out),'-j32'], 'build')]
try:
    subprocess.run(['git','-C',str(source),'apply',str(clock_patch)], check=True)
    target.write_text(target_text)
    for command, name in commands:
        with (logs/(name+'.log')).open('w') as stream:
            result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            print((logs/(name+'.log')).read_text(errors='replace')[-20000:])
            raise SystemExit(result.returncode)
finally:
    # Both adaptations belong only to the CPU host ICD.
    target.write_bytes(target_before)
    subprocess.run(['git','-C',str(source),'apply','-R',str(clock_patch)], check=True)
    assert subprocess.check_output(['git','-C',str(source),'diff','HEAD','--binary']) == before
drivers = list((out/'src/gallium/targets/lavapipe').glob('libvulkan_lvp*.dylib'))
assert drivers, 'built CPU ICD missing'
driver = next((x for x in drivers if x.name == 'libvulkan_lvp.dylib'), drivers[0]).resolve()
dependencies = subprocess.check_output(['xcrun','otool','-L',str(driver)], text=True)
symbols = subprocess.check_output(['xcrun','nm','-gU',str(driver)], text=True)
(logs/'linked-libraries.log').write_text(dependencies)
(logs/'exported-symbols.log').write_text(symbols)
assert 'libLLVM' not in dependencies, 'host ICD still loads shared LLVM'
exported = {line.split()[-1] for line in symbols.splitlines() if len(line.split()) >= 3}
assert {'_'+name for name in icd_exports} <= exported, 'required ICD entry missing'
assert not any(name.startswith(('_LLVM','__ZN4llvm','__ZNK4llvm')) for name in exported), 'host LLVM symbol escapes ICD'
manifests = list((out/'src/gallium/targets/lavapipe').glob('lvp_icd*.json'))
assert len(manifests) == 1
icd = json.loads(manifests[0].read_text()); icd['ICD']['library_path'] = str(driver)
manifest = logs/'lvp_icd.json'; manifest.write_text(json.dumps(icd,indent=2)+'\n')
loader = Path(os.environ['NMB_HOST_LOADER']); assert loader.is_file()
with Path(os.environ['GITHUB_ENV']).open('a') as stream:
    stream.write('VK_DRIVER_FILES='+str(manifest)+'\nVK_ICD_FILENAMES='+str(manifest)+'\nLP_NUM_THREADS=1\n')
diff = subprocess.check_output(['git','-C',str(source),'diff','HEAD','--binary'])
report = dict(passed=True, existing_Mesa_CPU_ICD_built=True, driver=str(driver),
    driver_sha256=hashlib.sha256(driver.read_bytes()).hexdigest(), loader=str(loader),
    source_commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),
    source_diff_sha256=hashlib.sha256(diff).hexdigest(), ICD_manifest=icd,
    source_runtime_floor_unchanged=True, host_clock_patch_sha256=hashlib.sha256(clock_patch.read_bytes()).hexdigest(), build_jobs=32, hardware_verified=False,
    LLVM_linkage='static', shared_LLVM_dependency_absent=True, LLVM_symbols_private=True,
    phone_operations=False, scope='Existing lavapipe CPU backend for actual frontend regressions; A840 target uses Turnip')
(logs/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
