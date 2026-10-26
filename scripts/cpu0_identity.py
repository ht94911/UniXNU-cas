# SPDX-License-Identifier: BSD-2-Clause-Patent
import hashlib,json
def candidate_tag(contract,mode,adt_sha256,source_heads):
    value={'profile':contract['profile'],'mode':mode,'kernel':contract['kernel']['sha256'],'adt':adt_sha256,'sources':source_heads}
    version=contract.get('identity_version',1)
    if version not in (1,2):raise ValueError('unsupported candidate identity version')
    if version==2:
        value.update(identity_version=2,owned={k:contract['owned'][k] for k in ('base','bytes')},
                     command_line=contract['command_lines']['execute'],
                     aux={k:contract.get(k) for k in ('ramdisk','trustcache')})
    return int.from_bytes(hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).digest()[:8],'little')
