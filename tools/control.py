#!/usr/bin/env python3
"""Deploy and control the single X5 acquisition shared by C++, ROS 1 and ROS 2."""
import argparse
import json
import os
from pathlib import Path
from adb_transport import ADB, device, remote as adb_remote
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
BOARD = '/userdata/x5-dev/unified-driver'
PORTS = [(15554+i, 554+i) for i in range(4)] + [(15601,6561),(15602,6562)]
BOARD_PYTHON = os.environ.get('ORBVI_BOARD_PYTHON', '/userdata/x5-dev/python/bin/python3')

def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)

def connection():
    return [ADB, '-s', device(os.environ.get('ANDROID_SERIAL'))]

def remote(adb, command, timeout=60):
    return adb_remote(adb, command, timeout=timeout)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['deploy','start','stop','status','probe','check','capture'])
    parser.add_argument('--config', type=Path, default=ROOT/'config/orbvi.yaml')
    parser.add_argument('--seconds', type=int, default=10)
    args = parser.parse_args()
    if args.command == 'capture':
        if not 1 <= args.seconds <= 3600:
            parser.error('seconds must be 1..3600')
        config = args.config.resolve()
        run(['docker','run','--rm','--init','--label','com.orbvi.unified=capture',
             '-v', str(ROOT)+':/sources/orbvi_driver','-v','orbvi-driver-build:/work',
             '-v',str(config)+':/capture.yaml:ro', 'orbvi-driver:humble','bash','-c',
             'python3 -u /sources/orbvi_driver/tools/imu_tunnel.py client --tcp-port 15601 --udp-port 5600 >&2 & '
             'relay=$!; trap "kill $relay 2>/dev/null || true" EXIT; '
             f'/work/driver-install/bin/orbvi_capture /capture.yaml {args.seconds} 3'])
        return
    adb=connection()
    ctl=shlex.quote(BOARD_PYTHON)+' '+BOARD+'/board_control.py '
    if args.command == 'deploy':
        if not (ROOT/'tools/orbvi_board-aarch64').exists() or not (ROOT/'backend/sensor_demo').exists():
            raise RuntimeError('Build board runner and backend first; see README')
        print(remote(adb,'mkdir -p '+BOARD))
        # The existing control script verifies PID identity before stopping.
        if remote(adb,'test -f '+BOARD+'/board_control.py && echo present || true').strip() == 'present':
            was_running=any(r.get('alive') for r in json.loads(remote(adb,ctl+'status')))
            print(remote(adb,ctl+'stop'))
            if was_running:
                for local,_ in PORTS:
                    subprocess.run([str(x) for x in adb]+['forward','--remove',f'tcp:{local}'],capture_output=True)
        for path in [ROOT/'backend',ROOT/'tools/board_control.py',ROOT/'tools/imu_tunnel.py']:
            run(adb+['push',path,BOARD+'/'])
        run(adb+['push',ROOT/'tools/orbvi_board-aarch64',BOARD+'/orbvi_board'])
        print(remote(adb,'chmod +x '+BOARD+'/orbvi_board'))
    elif args.command == 'probe':
        print(remote(adb,BOARD+'/orbvi_board probe'))
    elif args.command == 'check':
        run(adb+['push',args.config,BOARD+'/check.yaml'])
        print(remote(adb,BOARD+'/orbvi_board check '+BOARD+'/check.yaml'))
    elif args.command == 'start':
        print(remote(adb,'test ! -d /sys/class/gpio/gpio417'))
        run(adb+['push',args.config,BOARD+'/config.yaml'])
        print(remote(adb,BOARD+'/orbvi_board check '+BOARD+'/config.yaml'))
        added=[]
        try:
            for local, target in PORTS:
                run(adb+['forward','--no-rebind',f'tcp:{local}',f'tcp:{target}']);added.append(local)
            print(remote(adb,ctl+'start'))
        except BaseException:
            for local in added:
                subprocess.run([str(x) for x in adb]+['forward','--remove',f'tcp:{local}'],capture_output=True)
            raise
    elif args.command == 'stop':
        print(remote(adb,ctl+'stop'))
        for local,_ in PORTS:
            subprocess.run([str(x) for x in adb]+['forward','--remove',f'tcp:{local}'],capture_output=True)
    else:
        print(remote(adb,ctl+'status'))

if __name__ == '__main__':
    try:
        main()
    except (RuntimeError,OSError,subprocess.SubprocessError) as exc:
        print('orbvi-control: '+str(exc),file=sys.stderr)
        sys.exit(1)
