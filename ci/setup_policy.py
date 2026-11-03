"""Reuse the pinned upstream preference plan and merge function on a fresh RAM Data volume."""
from pathlib import Path
import ast,hashlib,importlib.util,json,os,plistlib,stat
HELPER_SHA256="0e43d2058ce83fff522441f9250151ba8c28badc7aaeaa0c0fddc5c8905f9bfe"
def preference_plan(helper,build):
    assert len(build)<32 and build.isalnum()
    raw=Path(helper).read_bytes();assert hashlib.sha256(raw).hexdigest()==HELPER_SHA256
    tree=ast.parse(raw);main=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=="main")
    calls=[n for n in ast.walk(main) if isinstance(n,ast.Expr) and isinstance(n.value,ast.Call) and isinstance(n.value.func,ast.Name) and n.value.func.id=="merge_pref"]
    assert len(calls)==5
    plan={}
    def collect(var,name,values):
        assert var is None and name not in plan and isinstance(values,dict);plan[name]=values
    scope=dict(merge_pref=collect,mount=None,BUILD=build,no_auto_dim=True)
    exec(compile(ast.Module(body=calls,type_ignores=[]),str(helper),"exec"),scope)
    assert set(plan)=={"com.apple.migration","com.apple.springboard","com.apple.purplebuddy","com.apple.purplebuddy.notbackedup","com.apple.managedconfiguration.notbackedup"}
    # Keep application watchdog policy at the existing value:only setup,
    # exact build migration state and documented idle behavior are changed.
    for key in ["BKNoWatchdogs","FBLaunchWatchdogExceptions"]:plan["com.apple.springboard"].pop(key)
    assert plan["com.apple.migration"]["LastSystemVersion"]==build
    return plan

def apply_to_mount(helper,var,build,owner=None):
    var=Path(var);assert (var/"mobile").is_dir();plan=preference_plan(helper,build)
    spec=importlib.util.spec_from_file_location("upstream_seed_preferences",helper);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    proof=[]
    for name,values in plan.items():
        p=var/"mobile/Library/Preferences"/(name+".plist");before=plistlib.loads(p.read_bytes()) if p.exists() else {}
        m.merge_pref(var,name,values)
        if owner is not None:os.chown(p,*owner)
        actual=plistlib.loads(p.read_bytes());assert actual=={**before,**values};st=p.stat();assert stat.S_IMODE(st.st_mode)==0o600
        if owner is not None:assert (st.st_uid,st.st_gid)==owner
        proof.append(dict(domain=name,keys=sorted(values),mode=stat.S_IMODE(st.st_mode),uid=st.st_uid,gid=st.st_gid,merged_and_read_back=True))
    for sub in ["Bundle","Data/System","Shared/SystemGroup","Temp"]:
        p=var/"containers"/sub;p.mkdir(parents=True,exist_ok=True);os.chmod(p,0o755)
    return dict(upstream_helper_SHA256=HELPER_SHA256,build=build,skip_setup=True,no_auto_dim=True,application_watchdog_policy_changed=False,owner=owner,preferences=proof,original_OS_unchanged=True,phone_operations=False)
