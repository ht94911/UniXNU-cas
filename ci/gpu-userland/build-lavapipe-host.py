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
# The existing checkout is sparse for Turnip. Expand only the CPU driver's
# LLVM, target and software winsys inputs at the same pinned Mesa commit.
subprocess.run(['git','-C',str(source),'sparse-checkout','add',
                'src/amd/llvm','src/gallium/targets','src/gallium/winsys','src/mapi'], check=True)
meson = root/'.cache/mesa-venv/bin/meson'
env = dict(os.environ)
env['PATH'] = str(root/'.cache/mesa-tools/bin')+':'+str(root/'.cache/mesa-venv/bin')+':'+env['PATH']
env['CC'], env['CXX'] = 'xcrun clang', 'xcrun clang++'
options = ['-Dgallium-drivers=llvmpipe', '-Dvulkan-drivers=swrast', '-Dplatforms=',
    '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dopengl=false',
    '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=enabled', '-Dshared-llvm=enabled',
    '-Dshared-glapi=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=enabled',
    '-Dtools=', '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=']
commands = [([str(meson),'setup',str(out),str(source),'--wrap-mode=nodownload','--buildtype=release',*options], 'configure'),
            (['ninja','-C',str(out),'-j32'], 'build')]
for command, name in commands:
    with (logs/(name+'.log')).open('w') as stream:
        result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print((logs/(name+'.log')).read_text(errors='replace')[-20000:])
        raise SystemExit(result.returncode)
drivers = list((out/'src/gallium/targets/lavapipe').glob('libvulkan_lvp*.dylib'))
assert drivers, 'built CPU ICD missing'
driver = next((x for x in drivers if x.name == 'libvulkan_lvp.dylib'), drivers[0]).resolve()
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
    source_runtime_floor_unchanged=True, build_jobs=32, hardware_verified=False,
    phone_operations=False, scope='Existing lavapipe CPU backend for actual frontend regressions; A840 target uses Turnip')
(logs/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
