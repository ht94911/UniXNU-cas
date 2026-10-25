# SPDX-License-Identifier: BSD-2-Clause-Patent
import hashlib,json
def candidate_tag(contract,mode,adt_sha256,source_heads):
    value={'profile':contract['profile'],'mode':mode,'kernel':contract['kernel']['sha256'],'adt':adt_sha256,'sources':source_heads}
    return int.from_bytes(hashlib.sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).digest()[:8],'little')
