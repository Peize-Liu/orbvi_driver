#!/usr/bin/env python3
"""Run one wrapper, or verify its actual RGB/IMU ROS subscriptions."""
import argparse
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('version',choices=['ros1','ros2'])
p.add_argument('--config',type=Path,default=ROOT/'config/orbvi.yaml')
p.add_argument('--check',action='store_true')
p.add_argument('--seconds',type=int,default=10)
p.add_argument('--dashboard',action='store_true')
a=p.parse_args()
if a.dashboard and (a.version!='ros2' or a.check):p.error('dashboard requires ros2 without --check')
if not 1<=a.seconds<=3600:p.error('seconds must be 1..3600')
args=['docker','run','--rm','--init','--shm-size','3g',
      '--label','com.orbvi.unified='+a.version,
      '-v',str(ROOT)+':/sources/orbvi_driver','-v','orbvi-driver-build:/work',
      '-v',str(a.config.resolve())+':/capture.yaml:ro']
if a.dashboard:
    args+=['-d','--name','orbvi-dashboard','-p','127.0.0.1:18765:8765']
args+=['orbvi-driver:humble' if a.version=='ros2' else 'orbvi-driver:noetic',
       'bash','/sources/orbvi_driver/tools/receiver.sh',a.version,
       'check' if a.check else 'dashboard' if a.dashboard else 'run',str(a.seconds)]
raise SystemExit(subprocess.run(args).returncode)
