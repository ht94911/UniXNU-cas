"""Regenerate and verify the combined public runtime tables on their required Mac."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--unixnu', type=Path, required=True)
p.add_argument('--logs', type=Path, required=True)
a = p.parse_args()
source = a.unixnu.resolve()/'.cache/gpu-reference/metal2vulkan'
out = a.logs.resolve()/'runtime-metadata'
out.mkdir(parents=True, exist_ok=True)
sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
production = {str(f.relative_to(source)): sha(f)
              for crate in ['metal2vulkan', 'metal2vulkan-air', 'metal2vulkan-runtime']
              for f in (source/'crates'/crate/'src').rglob('*.rs')}
env = dict(os.environ, RUSTC_BOOTSTRAP='1', CARGO_BUILD_JOBS='32', RUSTFLAGS='-Dwarnings')
steps = [
    ('signatures', ['python3', 'scripts/api-signatures/signatures.py', '--check']),
    ('refusals', ['python3', 'scripts/refusal-surface/refusal_surface.py', '--check']),
    ('golden-bless', ['cargo', '+1.98.0', 'test', '-p', 'metal2vulkan', '--all-features',
                      'bless_golden_hashes', '--', '--ignored', '--nocapture']),
    ('golden-check', ['cargo', '+1.98.0', 'test', '-p', 'metal2vulkan', '--all-features',
                      'golden_hashes_match_the_table', '--', '--nocapture']),
]
results = []
for name, command in steps:
    with (out/(name+'.log')).open('w') as log:
        r = subprocess.run(command, cwd=source, env=env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=7200)
    results.append(dict(check=name, returncode=r.returncode))
    if r.returncode:
        print((out/(name+'.log')).read_text(errors='replace')[-16000:])
        (out/'report.json').write_text(json.dumps(dict(passed=False, checks=results,
            phone_operations=False), indent=2)+'\n')
        raise SystemExit(r.returncode)
for name, digest in production.items():
    assert sha(source/name) == digest, 'metadata generation changed production source: '+name
tables = {}
for name in ['crates/metal2vulkan/tests/api_signatures.tsv',
             'crates/metal2vulkan/tests/golden_hashes.tsv', 'docs/REFUSALS.md']:
    target = out/Path(name).name
    shutil.copyfile(source/name, target)
    tables[name] = dict(file=target.name, sha256=sha(target))
(out/'report.json').write_text(json.dumps(dict(passed=True, checks=results, tables=tables,
    every_required_assembler_available=True, production_sources_unchanged=production,
    metadata_only=True, hardware_verified=False, phone_operations=False), indent=2)+'\n')
print('PASS: preserved public APIs/refusals and complete public golden table; production Rust unchanged')
