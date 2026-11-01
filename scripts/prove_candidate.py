"""Replay the nine cases' 45 final/boundary checks against fresh NoPost captures."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse, importlib.util, json, hashlib, subprocess, datetime, tempfile, os, shutil

ROOT=Path(__file__).resolve().parents[1]
E=ROOT/'evidence/current'
load=lambda p:json.loads(p.read_text(encoding='utf-8-sig'))
save=lambda p,v:p.write_text(json.dumps(v,ensure_ascii=False,indent=2),encoding='utf-8')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--oss-root',required=True,type=Path)
    ap.add_argument('--jobs',type=int,default=2)
    ap.add_argument('--case',default='all')
    ap.add_argument('--publish-reference',action='store_true')
    args=ap.parse_args()
    assert 1<=args.jobs<=4
    old=load(ROOT/'PROOF_TEMPLATE_INDEX.json')
    cap=load(E/'CAPTURE_FULL.json')
    assert cap['complete']
    tasks=[]
    for rec in old['jobs']:
        if rec['kind'] not in {'final_pair','rtl_to_boundary','boundary_to_final'}:continue
        text=(ROOT/'proof_templates'/rec['key']).with_suffix('.eqy').read_text(encoding='utf-8')
        ports=load(E/cap['cases'][rec['case']]['folder']/'initialized.json')['modules'][cap['cases'][rec['case']]['top']]['ports']
        files={}
        for line in text.splitlines():
            if line.startswith(('read_rtlil ','read_json ','read_verilog ')):
                p=ROOT/line.split('"')[1]
                assert p.is_file(),p
                files[p.relative_to(ROOT).as_posix()]=sha(p)
        tasks.append({'key':rec['key'],'case':rec['case'],'kind':rec['kind'],'expected':'PASS','config':text,'ports':ports,'input_sha256':files})
    assert len(tasks)==45
    if args.case!='all':
        tasks=[t for t in tasks if t['case']==args.case]
        assert len(tasks)==5,'Unknown case'
    dest=ROOT/'runs'/('eqy_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    dest.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('audited_runner',ROOT/'scripts/EqyRunner.py')
    runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
    oss=args.oss_root.resolve()
    raw=subprocess.check_output(['/bin/bash','-c','source "$1/environment"; env -0','oss',str(oss)])
    env=dict(item.decode().split('=',1) for item in raw.split(b'\0') if item)
    result={'candidate':'V53H-NoPost','expected_jobs':len(tasks),'complete':False,'jobs':[]}
    save(dest/'PLAN.json',{'tasks':[{k:v for k,v in t.items() if k not in ['config','ports']} for t in tasks]})
    # Keep user-selected extraction paths out of EQY's generated shell commands.
    with tempfile.TemporaryDirectory(prefix='pmux_nopost_',dir='/tmp') as temporary:
        execution=Path(temporary)
        for name in ['evidence','inputs']:os.symlink(ROOT/name,execution/name,target_is_directory=True)
        os.symlink(oss,execution/'tools',target_is_directory=True)
        runner.ROOT=execution;runner.PACKAGE_ROOT=execution
        try:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                fs={pool.submit(runner.one,t,execution/'tools',env):t for t in tasks}
                for f in as_completed(fs):
                    r=f.result();r['expected']='PASS';result['jobs'].append(r)
                    save(dest/'RESULTS.json',result)
                    print(r['key'],r['status'],'complete',len(result['jobs']),'/',len(tasks),flush=True)
        finally:
            if (execution/'proofs_zero_init').exists():shutil.copytree(execution/'proofs_zero_init',dest/'proofs_zero_init')
    result.update(complete=True,positive_total=len(tasks),positive_passed=sum(r['passed'] for r in result['jobs']),all_checks_pass=all(r['passed'] for r in result['jobs']))
    save(dest/'RESULTS.json',result)
    if not result['all_checks_pass']:raise SystemExit('Incomplete or failed formal checks')
    if args.publish_reference:
        assert len(tasks)==45 and args.case=='all'
        save(E/'PROOFS_FULL_LF.json',result)
        save(E/'PROOF_PLAN.json',{'folder':dest.relative_to(ROOT).as_posix(),'tasks':[{k:v for k,v in t.items() if k not in ['config','ports']} for t in tasks]})

if __name__=='__main__':main()
