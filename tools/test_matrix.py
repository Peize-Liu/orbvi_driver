#!/usr/bin/env python3
"""Sequential real-board sweep. Saves every outcome and restores default capture."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from control import connection, remote

ROOT=Path(__file__).resolve().parents[1]
CONTROL=ROOT/'tools/control.py'
SIZES=[(1280,1088),(1280,720),(720,480),(640,480),(1088,1280),(720,1280),(480,720),(480,640)]
RATES=[25,30,40,50,60]

def command(args, timeout=90):
    return subprocess.run([sys.executable,str(CONTROL),*map(str,args)],capture_output=True,text=True,timeout=timeout)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds',type=int,default=5)
    parser.add_argument('--resume',action='store_true')
    parser.add_argument('--only',help='e.g. 1280x1088@60')
    args=parser.parse_args()
    if not 1<=args.seconds<=3600:parser.error('invalid seconds')
    output=ROOT/'reports/camera-matrix.json'
    configurations=ROOT/'reports/configs';configurations.mkdir(exist_ok=True)
    text=(ROOT/'config/orbvi.yaml').read_text()
    results=json.loads(output.read_text()) if args.resume and output.exists() else []
    done={r['mode'] for r in results}
    try:
        for width,height in SIZES:
            for fps in RATES:
                mode=f'{width}x{height}@{fps}'
                if args.only and mode!=args.only:continue
                if mode in done:continue
                result=dict(mode=mode,width=width,height=height,fps=fps,seconds=args.seconds,
                            tested_at=datetime.datetime.now().astimezone().isoformat(),pass_test=False)
                config=text
                for key,value in [('width',width),('height',height),('fps',fps)]:
                    config=re.sub(rf'(?m)^  {key}: .*$',f'  {key}: {value}',config)
                path=configurations/(mode.replace('@','-')+'.yaml');path.write_text(config)
                try:
                    stopped=command(['stop'])
                    if stopped.returncode:raise RuntimeError(stopped.stdout+stopped.stderr)
                    started=command(['start','--config',path])
                    if started.returncode:raise RuntimeError(started.stdout+started.stderr)
                    captured=command(['capture','--config',path,'--seconds',args.seconds],args.seconds+60)
                    (configurations/(mode+'.stderr')).write_text(captured.stderr)
                    result['receiver']=json.loads(captured.stdout)
                    result['pass_test']=captured.returncode==0 and result['receiver']['pass']
                    # Read source counters before stopping; failures remain visible.
                    source=remote(connection(), 'tail -20 /userdata/x5-dev/unified-driver/sensor.log', timeout=15)
                    (configurations/(mode+'.board.log')).write_text(source)
                    result['source_tail_file']=str((configurations/(mode+'.board.log')).relative_to(ROOT))
                except Exception as exc:
                    result['error']=str(exc)[-5000:]
                results.append(result)
                tmp=output.with_suffix('.tmp');tmp.write_text(json.dumps(results,indent=2));tmp.replace(output)
                print(f'{len(results):02d} {mode}: '+('PASS' if result['pass_test'] else 'FAIL'),flush=True)
    finally:
        stopped=command(['stop'])
        if stopped.returncode:raise RuntimeError('could not stop test capture: '+stopped.stderr+stopped.stdout)
        restored=command(['start'])
        if restored.returncode:raise RuntimeError('could not restore default capture: '+restored.stderr+restored.stdout)
    print(f'Saved {output}',flush=True)

if __name__=='__main__':main()
