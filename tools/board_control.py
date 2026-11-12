#!/usr/bin/env python3
"""Manage only the isolated development acquisition and IMU tunnel processes."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
STATE = ROOT / 'processes.json'


def identity(pid):
    try:
        text = Path(f'/proc/{pid}/stat').read_text()
        fields = text[text.rfind(')') + 2:].split()
        return None if fields[0] == 'Z' else fields[19]
    except FileNotFoundError:
        return None


def alive(record):
    return identity(record['pid']) == record['start_ticks']


def stop():
    records = json.loads(STATE.read_text()) if STATE.exists() else []
    for record in reversed(records):
        if not alive(record):
            continue
        os.kill(record['pid'], signal.SIGTERM)
        deadline = time.monotonic() + 12
        while alive(record) and time.monotonic() < deadline:
            time.sleep(0.1)
        if alive(record):
            os.killpg(record['pid'], signal.SIGKILL)
            raise RuntimeError(f'{record["name"]} needed forced shutdown; inspect GPIO/IIO before restart')
    STATE.unlink(missing_ok=True)
    print('Stopped owned acquisition processes')


def start():
    if STATE.exists() and any(alive(r) for r in json.loads(STATE.read_text())):
        raise RuntimeError('Acquisition already running; use status or stop')
    if Path('/sys/class/gpio/gpio417').exists():
        raise RuntimeError('GPIO417 is busy; refusing to take over another capture')
    for device in Path('/sys/bus/iio/devices').glob('iio:device*'):
        if 'icm42688' in (device / 'name').read_text():
            if (device / 'buffer/enable').read_text().strip() != '0':
                raise RuntimeError(f'{device} is busy')
    records = []
    jobs = [
        ('tunnel', [sys.executable, '-u', str(ROOT / 'imu_tunnel.py'), 'server',
                    '--tcp-port', '6561', '--udp-port', '5601']),
        ('sensor', [str(ROOT / 'orbvi_board'), 'run', str(ROOT / 'config.yaml'),
                    str(ROOT / 'backend/sensor_demo')]),
    ]
    try:
        for name, command in jobs:
            with (ROOT / (name + '.log')).open('w') as log:
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True,
                                           cwd=ROOT, env=dict(os.environ, LD_LIBRARY_PATH='/usr/hobot/lib:/usr/hobot/lib/sensor:' + os.environ.get('LD_LIBRARY_PATH', '')))
            record = {'name': name, 'pid': process.pid, 'start_ticks': identity(process.pid)}
            records.append(record)
            STATE.write_text(json.dumps(records))
            time.sleep(0.3)
            if process.poll() is not None:
                raise RuntimeError(f'{name} failed: {(ROOT / (name + ".log")).read_text()[-4000:]}')
        deadline = time.monotonic() + 30
        ready = set()
        while len(ready) < 4 and time.monotonic() < deadline:
            if not all(alive(r) for r in records):
                raise RuntimeError('Capture exited during startup; inspect sensor.log')
            for index in range(4):
                if index in ready:
                    continue
                try:
                    with socket.create_connection(('127.0.0.1', 554 + index), timeout=.5) as sock:
                        request = (f'DESCRIBE rtsp://127.0.0.1:{554+index}/live/PRR RTSP/1.0\r\n'
                                   'CSeq: 1\r\nAccept: application/sdp\r\n\r\n')
                        sock.sendall(request.encode())
                        response = bytearray()
                        while b'\r\n\r\n' not in response or b'sprop-parameter-sets=' not in response:
                            chunk = sock.recv(4096)
                            if not chunk or len(response) > 32768:
                                break
                            response.extend(chunk)
                        if b'200 OK' in response and b'sprop-parameter-sets=' in response:
                            ready.add(index)
                except OSError:
                    pass
            time.sleep(.2)
        if len(ready) != 4:
            raise RuntimeError('Four RTSP streams did not become ready within 30 seconds')
        print(json.dumps(records))
    except BaseException:
        stop()
        raise


if __name__ == '__main__':
    if sys.argv[1] == 'start':
        start()
    elif sys.argv[1] == 'stop':
        stop()
    elif sys.argv[1] == 'status':
        records = json.loads(STATE.read_text()) if STATE.exists() else []
        print(json.dumps([dict(r, alive=alive(r)) for r in records]))
    else:
        raise SystemExit('usage: board_control.py start|stop|status')
