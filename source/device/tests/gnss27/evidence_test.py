# SPDX-License-Identifier: BSD-2-Clause
"""Synthetic evidence comparison tests. Run with the selected package checks."""
import copy
import importlib.util
from pathlib import Path
import struct

path = Path(__file__).resolve().parents[2] / "tools/gnss27/corelocation_evidence.py"
spec = importlib.util.spec_from_file_location("gnss_evidence", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
receiver = dict(boot_session_uuid="synthetic-boot", candidate_sha256="1" * 64,
                fix=dict(status=0,running=1,published=1,fix_deliveries=1,valid=0x832,cursor=1,
                         lat_bits=struct.pack(">d",0).hex(),lon_bits=struct.pack(">d",0).hex(),
                         accuracy_bits=struct.pack(">f",5).hex(),utc_ms=1700000000000,
                         used_present=1,used_count=1,bds_used=1,signals_present=1,bds_seen=1),
                used=[dict(cursor=1,index=0,system=6,svid=201,signals=0x600,used_in_fix=True)])
app = dict(boot_session_uuid="synthetic-boot",candidate_sha256="1" * 64,provider="CoreLocation",
           authorization="authorizedWhenInUse",accuracy_authorization="fullAccuracy",received_unix_ms=1700000000100,
           location=dict(simulated_by_software=False,latitude=0,longitude=0,horizontal_accuracy=5,timestamp_unix_ms=1700000000000))
result = module.correlate(receiver,app)
assert result["application_aligned"] and result["bds_used_confirmed"]==1
assert result["bds_signals"]==dict(B1I=True,B1C=True,B2a=False)
visible=copy.deepcopy(receiver);visible["used"]=[];visible["fix"]["used_present"]=0
assert module.correlate(visible,app)["status"]=="已观测到北斗，参与解算待证"
stale=copy.deepcopy(app);stale["received_unix_ms"]+=20000
assert not module.correlate(receiver,stale)["application_aligned"]
approximate=copy.deepcopy(app);approximate["accuracy_authorization"]="reducedAccuracy"
assert not module.correlate(receiver,approximate)["application_aligned"]
for key,value in (("boot_session_uuid","different"),("candidate_sha256","2"*64)):
    mismatch=copy.deepcopy(app);mismatch[key]=value
    try:module.correlate(receiver,mismatch)
    except ValueError:pass
    else:raise AssertionError("mismatched source identity accepted")
unknown=copy.deepcopy(receiver);unknown["fix"]["signals_present"]=0
assert module.correlate(unknown,app)["bds_signals"]==dict(B1I=None,B1C=None,B2a=None)
multiband=copy.deepcopy(receiver);multiband["fix"]["used_count"]=2
multiband["used"].append(dict(cursor=1,index=1,system=6,svid=201,signals=0x1000,used_in_fix=True))
assert module.correlate(multiband,app)["bds_used_confirmed"]==1
assert module.correlate(multiband,app)["bds_signals"]["B2a"] is True
print("GNSS evidence synthetic comparison passed; no hardware claim")
