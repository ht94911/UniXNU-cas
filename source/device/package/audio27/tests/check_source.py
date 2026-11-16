#!/usr/bin/env python3
"""Source parsing and provenance only. Never invokes a compiler or phone tool.

Dependencies: tree-sitter, tree-sitter-c, tree-sitter-cpp. This does not resolve
types, macro expansions, binary ABI, linker imports, or execute the C tests.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
from tree_sitter import Language, Parser
import tree_sitter_c
import tree_sitter_cpp

ROOT = Path(__file__).resolve().parents[1]


def run(upstream, smmu):
    manifest = json.loads((ROOT / 'integration/draft.json').read_text())
    assert manifest['enabled'] is False
    selected = manifest['selected_test_package']
    if selected is None:
        assert manifest['source_status'] in ('partial-source-not-hardware-ready', 'source-reviewed-awaiting-selected-package')
    else:
        bases = {
            'n27-main-persist015': ('154f8a08e6800734408c99f909c28ceeb94caa3c', 'GPU015'),
            'n27-main-persist023': ('673cde869b7378658b470c4e6b2b9a1766b99411', 'GPU023'),
            'baseband044-GPU024': ('120c8ca16177eafbc4d000135165a676ad7f91df', 'GPU024'),
        }
        revision, label = bases[selected['name']]
        assert selected['device_revision'] == revision
        assert manifest['source_status'] == (label+'-baseband045-audio102-source-integrating' if label=='GPU024' else label+'-integrating-native-ABI-and-readonly-resource-census')
        assert manifest['resource_probe']['hardware_writes'] is False
        assert manifest['resource_probe']['grants_resource_ownership'] is False
        assert smmu, 'selected build requires its actual staged ArmSmmu4K component'
        component = json.loads((ROOT / 'integration/shared-component.json').read_text())
        for name, digest in component['files'].items():
            assert hashlib.sha256((smmu / name).read_bytes()).hexdigest() == digest, name
    implemented = {str(p.relative_to(ROOT)) for p in ROOT.rglob('*')
                   if p.suffix in ('.c', '.cpp') and p.relative_to(ROOT).parts[0] not in ('upstream', 'tests')}
    assert set(manifest['implemented_sources']) == implemented, 'stale integration source list'
    provenance = json.loads((ROOT / 'upstream/provenance.json').read_text())
    assert len({row['local'] for row in provenance['files']}) == len(provenance['files'])
    original_count = 0
    for row in provenance['files']:
        path = (ROOT / row['local']).resolve()
        assert path.is_relative_to(ROOT) and path.is_file(), row['local']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == row['local_sha256'], row['local']
        assert row['modified'] == (row['sha256'] != row['local_sha256'])
        if upstream:
            data = (upstream / row['source']).read_bytes()
            assert hashlib.sha256(data).hexdigest() == row['sha256'], row['source']
            original_count += 1
    shared = manifest.get('shared_source_dependencies', {})
    for relative, digest in shared.items():
        target = ROOT.parent / relative
        assert target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == digest, relative
    extra_originals = 0
    wcd = json.loads((ROOT/'upstream/wcd-provenance.json').read_text())
    assert wcd['license'] == 'GPL-2.0-only' and (ROOT/'upstream/LICENSE.GPL-2.0').is_file()
    for row in wcd['local']:
        data = (ROOT/row['path']).read_bytes()
        assert hashlib.sha256(data).hexdigest() == row['sha256'], row['path']
        assert b'SPDX-License-Identifier: GPL-2.0-only' in data
    if upstream:
        for row in wcd['sources']:
            data = (upstream.parent/'downstream'/row['path']).read_bytes()
            assert hashlib.sha256(data).hexdigest() == row['sha256'], row['path']
            extra_originals += 1
    pandora = json.loads((ROOT/'upstream/pandora-provenance.json').read_text())
    assert hashlib.sha256((ROOT/pandora['local']).read_bytes()).hexdigest() == pandora['local_sha256']
    assert (ROOT/'upstream/LICENSE.PandoraAcdb').is_file()
    if upstream:
        assert hashlib.sha256((upstream.parent/'pandora'/pandora['source']).read_bytes()).hexdigest() == pandora['source_sha256']
        extra_originals += 1
    parsers = {'c': Parser(Language(tree_sitter_c.language())),
               'cpp': Parser(Language(tree_sitter_cpp.language()))}
    parsed = []
    declared = set()
    defined = set()
    calls = set()
    for path in sorted(ROOT.rglob('*')):
        if path.suffix == '.py':
            ast.parse(path.read_text(), filename=str(path))
        if path.suffix not in ('.c', '.h', '.cpp', '.inc'):
            continue
        data = path.read_text()
        assert data.endswith('\n'), path
        assert all(line == line.rstrip() for line in data.splitlines()), path
        for include in re.findall(r'^#include "([^"]+)"', data, re.M):
            target = (path.parent / include).resolve()
            assert target.is_file(), (path, include)
            assert target.is_relative_to(ROOT) or (target.is_relative_to(ROOT.parent) and str(target.relative_to(ROOT.parent)) in shared), (path, include)
        variants = [('cpp' if path.suffix == '.cpp' else 'c', data)]
        if path.relative_to(ROOT) == Path('os/NezhaAudioXpuTrace.h'):
            # This IOKit inline helper has references/default arguments and
            # is used by exactly these C++ translation units. Check that
            # boundary and parse its real language; public C headers below
            # still require both syntax views.
            callers = {str(source.relative_to(ROOT)) for source in ROOT.rglob('*')
                       if source.suffix in ('.c', '.cpp', '.h') and
                       '#include "NezhaAudioXpuTrace.h"' in source.read_text()}
            assert callers == {'os/NezhaAudioNative.cpp', 'os/NezhaAudioTasBoard.cpp',
                               'os/NezhaAudioDspProbe.cpp', 'os/NezhaAudioMicBoard.cpp'}, callers
            variants = [('cpp', data)]
        elif path.suffix == '.h':
            # These are the exact public extern-C guards, not general macro
            # evaluation. Tree-sitter cannot join scopes across #ifdef blocks.
            opening = '#ifdef __cplusplus\nextern "C" {\n#endif'
            closing = '#ifdef __cplusplus\n}\n#endif'
            variants = [('c', data.replace(opening, '').replace(closing, '')),
                        ('cpp', data.replace(opening, 'extern "C" {').replace(closing, '}'))]
        for language, view in variants:
            tree = parsers[language].parse(view.encode())
            stack = [tree.root_node]
            while stack:
                node = stack.pop()
                assert node.type != 'ERROR' and not node.is_missing, (str(path), language, node.start_point, node.text[:80])
                stack.extend(node.children)
                if node.type == 'function_declarator':
                    name_node = node.child_by_field_name('declarator')
                    if name_node and name_node.type == 'identifier':
                        name = name_node.text.decode()
                        if name.startswith('na_'):
                            declared.add(name)
                            if node.parent.type == 'function_definition':
                                defined.add(name)
                if node.type == 'call_expression':
                    callee = node.child_by_field_name('function')
                    if callee and callee.type == 'identifier' and callee.text.startswith(b'na_'):
                        calls.add(callee.text.decode())
            parsed.append({'file': str(path.relative_to(ROOT)), 'view': language})
    assert declared == defined, {'declarations_without_definition': sorted(declared-defined)}
    assert calls <= defined, sorted(calls-defined)
    for relative in manifest['host_test_sources']:
        assert (ROOT / relative).is_file(), relative
    tests = json.loads((ROOT/'tests/host-tests.json').read_text())
    assert tests['status'] == ('deferred-not-compiled-or-executed' if selected is None else
        'nine-suites-passed-gcc13-asan-ubsan-GPU015-ArmSmmu4K1811b6')
    assert tests['tests'] == manifest['host_test_sources']
    for relative in tests['common_sources_relative_to_audio27']:
        assert (ROOT/relative).is_file(), relative
    for relative in tests['shared_sources_relative_to_package']:
        if selected and selected['name']=='baseband044-GPU024':
            # The selected kit is already hash-checked above. Historical
            # package paths do not select a second ArmSmmu implementation.
            assert smmu and (smmu/Path(relative).name).is_file(), relative
        else:
            assert relative in shared and (ROOT.parent/relative).is_file(), relative
    return {'kind': 'source-parser-and-provenance-check', 'files': len({x['file'] for x in parsed}),
            'syntax_views': len(parsed), 'public_functions_defined': len(defined),
            'pinned_original_files_checked': original_count,
            'local_provenance_files_checked': len(provenance['files']),
            'additional_original_files_checked': extra_originals,
            'gpl_local_files_checked': len(wcd['local']),
            'implemented_source_entries': len(implemented),
            'c_test_sources': len(manifest['host_test_sources']),
            'checker_compiled_or_executed_c': False, 'checker_compiled_kernel_or_hal': False,
            'checker_accessed_phone': False, 'abi_verified': False,
            'prior_stock_capture_available': bool(manifest.get('local_private_inputs_available')),
            'hardware_ready': False, 'syntax_files': parsed}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, help='Read-only pinned sheng checkout')
    parser.add_argument('--smmu', type=Path, help='Exact GPU015 ArmSmmu4K component from its fixed framework')
    parser.add_argument('--report', type=Path, help='Local evidence path outside source tree')
    args = parser.parse_args()
    result = run(args.upstream, args.smmu)
    if args.report:
        target = args.report.resolve()
        if target.is_relative_to(ROOT):
            parser.error('Keep generated evidence outside the source directory')
        target.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'syntax_files'}, indent=2))
