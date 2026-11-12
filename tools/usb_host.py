#!/usr/bin/env python3
"""Build and manage the native libusb receiver used by Mac Docker clients."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import signal
import subprocess
import time
import yaml

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'build/usb-host'
STATE=WORK/'process.json'
BINARY=WORK/'orbvi_usb_bridge'


def identity(pid):
    result=subprocess.run(['ps','-p',str(pid),'-o','lstart=','-o','command='],capture_output=True,text=True)
    return result.stdout.strip() if result.returncode==0 else None


def running(record):
    return record.get('identity') is not None and identity(record['pid'])==record['identity']


def build():
    WORK.mkdir(parents=True,exist_ok=True)
    environment=dict(os.environ)
    environment['PKG_CONFIG_PATH']='/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig:'+environment.get('PKG_CONFIG_PATH','')
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','libusb-1.0'],text=True,env=environment)
    compiler=os.environ.get('CXX','clang++' if platform.system()=='Darwin' else 'g++')
    subprocess.run([compiler,'-O2','-std=c++17','-pthread','-I'+str(ROOT/'include'),str(ROOT/'src/usb_bridge.cpp'),
                    *shlex.split(flags),'-o',str(BINARY)],check=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command',choices=['build','probe','start','stop','status'])
    parser.add_argument('--config',type=Path,default=ROOT/'config/orbvi.yaml')
    args=parser.parse_args()
    record=json.loads(STATE.read_text()) if STATE.exists() else {}
    alive=bool(record and running(record))
    if args.command=='status':
        print(json.dumps(dict(record,alive=alive)));return
    if args.command=='stop':
        if alive:
            os.kill(record['pid'],signal.SIGTERM)
            end=time.monotonic()+5
            while running(record) and time.monotonic()<end:time.sleep(.1)
            if running(record):raise RuntimeError('USB receiver did not stop; inspect '+str(WORK/'usb.log'))
        STATE.unlink(missing_ok=True);print('USB host receiver stopped');return
    if args.command=='start' and alive:raise RuntimeError('USB host receiver already running')
    build()
    if args.command=='build':print(BINARY);return
    config=yaml.safe_load(args.config.read_text())['transport']
    command=[str(BINARY),'--port',str(config.get('usb_port',15700))]
    if config.get('usb_serial'):command+=['--serial',config['usb_serial']]
    if args.command=='probe':subprocess.run(command+['--probe'],check=True);return
    log=WORK/'usb.log'
    with log.open('w') as output:
        child=subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
    record=dict(pid=child.pid,identity=identity(child.pid),config=str(args.config.resolve()))
    STATE.write_text(json.dumps(record))
    end=time.monotonic()+8
    while time.monotonic()<end:
        if child.poll() is not None:raise RuntimeError(log.read_text())
        if 'USB bridge ready' in log.read_text():
            print('USB host receiver started; '+str(log));return
        time.sleep(.1)
    child.terminate();child.wait(timeout=5)
    raise RuntimeError('USB host receiver startup timed out')


if __name__=='__main__':
    main()
