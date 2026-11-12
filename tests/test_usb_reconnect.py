#!/usr/bin/env python3
"""Bridge disconnect/timeout recovery without hardware; incompatible config stays fatal."""
import json
import math
import selectors
import socket
import struct
import subprocess
import sys
import time
import zlib


def envelope(kind, seq, stamp, payload):
    return struct.pack('<4sBB2xIIQ', b'X5US', 1, kind, len(payload), seq, stamp) + payload


def configuration(width=1280):
    config = dict(schema_version=1, width=width, height=1088, fps=60, bitrate_kbps=4000,
                  imu_frequency_hz=1000, accel_range_g=16, gyro_range_dps=2000,
                  transport_mode='usb', usb_protocol=1, usb_mux_dropped=0, usb_mux_invalid=0,
                  capabilities=dict(imu=[dict(name='icm42688-accel', frequency_hz=1000, scale=16*9.80665/32768),
                                         dict(name='icm42688-gyro', frequency_hz=1000, scale=2000*math.pi/180/32768)]))
    return envelope(0x12, 0, 1, json.dumps(config).encode())


def imu(seq):
    stamp = 1000000000 + seq * 1000000
    payload = struct.pack('>4sBBHQQQQI6f', b'OIMU', 1, 1, 72, seq, stamp, stamp, stamp,
                          0, 0., 0., 9.80665, 0., 0., 0.)
    payload += struct.pack('>I', zlib.crc32(payload))
    return envelope(0x11, seq, stamp, payload)


with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(10)
    process = subprocess.Popen([sys.argv[1], str(listener.getsockname()[1])], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, bufsize=1)
    selector = selectors.DefaultSelector(); selector.register(process.stdout, selectors.EVENT_READ)
    peers = []
    def wait_for(predicate, timeout=10):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            if selector.select(.2):
                line = process.stdout.readline()
                if not line:
                    raise AssertionError(process.stderr.read())
                last = json.loads(line)
                if predicate(last):
                    return last
        raise AssertionError(f'Expected state not reached: {last}')
    try:
        first, _ = listener.accept(); peers.append(first)
        first.sendall(configuration()+imu(1)); wait_for(lambda s: s['count'] == 1)
        first.close()
        wait_for(lambda s: 'reconnecting' in s['error'])
        second, _ = listener.accept(); peers.append(second)
        second.sendall(imu(2))  # Must be ignored before the new handshake.
        time.sleep(.2)
        second.sendall(configuration()+imu(3))
        resumed = wait_for(lambda s: s['count'] == 2 and not s['error'])
        assert resumed['gaps'] == 1, resumed  # History is not reset or concealed.
        wait_for(lambda s: 'timed out' in s['error'], timeout=8)
        third, _ = listener.accept(); peers.append(third)
        third.sendall(configuration()+imu(4))
        wait_for(lambda s: s['count'] == 3 and not s['error'])
        third.sendall(configuration(width=640))
        wait_for(lambda s: 'configuration mismatch' in s['error'])
        listener.settimeout(1)
        try:
            extra, _ = listener.accept(); extra.close()
            raise AssertionError('Invalid configuration must not trigger retries')
        except socket.timeout:
            pass
        print('PASS disconnect + idle timeout recovery, handshake gating, retained gaps, fatal config mismatch')
    finally:
        for peer in peers:
            peer.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(); raise
        selector.close()
