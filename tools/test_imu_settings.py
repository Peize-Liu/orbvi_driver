#!/usr/bin/env python3
"""Probe configuration acceptance without altering a running IMU."""
import json
from pathlib import Path
import re
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'config/orbvi.yaml').read_text()
directory=ROOT/'reports/imu-configs';directory.mkdir(parents=True,exist_ok=True)
results=[]
for key,values in [('frequency_hz',[25,50,100,200,500,1000,2000]),
                   ('accel_range_g',[2,4,8,16]),
                   ('gyro_range_dps',[15.625,31.25,62.5,125,250,500,1000,2000])]:
    for value in values:
        path=directory/(f'{key}-{value}.yaml')
        path.write_text(re.sub(rf'(?m)^  {key}: .*$',f'  {key}: {value}',source))
        result=subprocess.run([sys.executable,str(ROOT/'tools/control.py'),'check','--config',str(path)],capture_output=True,text=True,timeout=15)
        results.append(dict(parameter=key,value=value,accepted=result.returncode==0,
                            stdout=result.stdout,stderr=result.stderr,
                            method='BSP capability validation; no register writes'))
(ROOT/'reports/imu-settings.json').write_text(json.dumps(results,indent=2))
print(json.dumps([dict(parameter=r['parameter'],value=r['value'],accepted=r['accepted']) for r in results],indent=2))
