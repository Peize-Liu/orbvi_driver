#!/usr/bin/env python3
"""Publish camera access units, full OIMU records and config over BSP USB Bulk."""
from collections import deque
import http.client
import json
from pathlib import Path
import selectors
import signal
import socket
import struct
import time

ROOT = Path(__file__).resolve().parent
HEADER = struct.Struct('<4sBB2xIIQ')
MAX_PAYLOAD = 4 * 1024 * 1024
MAX_QUEUE = 8 * 1024 * 1024
stop_deadline = None


def stop(signum, frame):
    global stop_deadline
    stop_deadline = time.monotonic()+2


def packet(kind, sequence, stamp, payload):
    return HEADER.pack(b'X5US', 1, kind, len(payload), sequence & 0xffffffff, stamp) + payload


class CameraParser:
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        frames = []
        offset = 0
        while len(self.pending)-offset >= HEADER.size:
            magic, version, kind, length, seq, stamp = HEADER.unpack_from(self.pending, offset)
            if magic != b'X5US' or version != 1 or kind != 0x10 or not 4 < length <= MAX_PAYLOAD:
                raise ValueError('Invalid local camera message')
            if len(self.pending)-offset < HEADER.size+length:
                break
            if struct.unpack_from('<I', self.pending, offset+HEADER.size)[0] >= 4 or not stamp:
                raise ValueError('Invalid camera identity')
            frames.append(bytes(self.pending[offset:offset+HEADER.size+length]))
            offset += HEADER.size+length
        if offset:
            del self.pending[:offset]
        if len(self.pending) > MAX_PAYLOAD+HEADER.size:
            raise ValueError('Camera buffer exceeded')
        return frames


def main():
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    path = ROOT/'camera.sock'
    if path.exists():
        path.unlink()  # Only the owned supervisor starts this process.
    local = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    local.bind(str(path)); local.listen(4); local.setblocking(False)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024)
    udp.bind(('127.0.0.1',5601)); udp.setblocking(False)
    selector = selectors.DefaultSelector()
    selector.register(local, selectors.EVENT_READ, 'listener')
    selector.register(udp, selectors.EVENT_READ, 'imu')
    bulk = None
    outgoing = deque()
    queued = sent = dropped = invalid = 0
    next_connect = next_config = next_report = 0.
    config_sequence = 0

    def enqueue(frame):
        nonlocal queued, dropped
        if bulk is None or queued+len(frame)>MAX_QUEUE:
            dropped += 1
            return
        outgoing.append(memoryview(frame)); queued += len(frame)
        selector.modify(bulk, selectors.EVENT_READ|selectors.EVENT_WRITE, 'bulk')

    while True:
        now = time.monotonic()
        if stop_deadline is not None and (not outgoing or now >= stop_deadline):
            if outgoing:
                print('USB shutdown discarded pending messages: '+str(len(outgoing)),flush=True)
            return
        if bulk is None and now >= next_connect:
            next_connect = now+1
            candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                candidate.connect('/run/x5-usb-gadget/bulk.sock')
                candidate.setblocking(False); bulk=candidate
                selector.register(bulk, selectors.EVENT_READ, 'bulk')
                print('BSP USB Bulk connected', flush=True)
            except OSError:
                candidate.close()
        if now >= next_config:
            next_config=now+1
            connection=http.client.HTTPConnection('127.0.0.1',6562,timeout=.2)
            try:
                connection.request('GET','/config'); response=connection.getresponse()
                if response.status==200:
                    config=json.loads(response.read())
                    config.update(usb_protocol=1,usb_mux_dropped=dropped,usb_mux_invalid=invalid)
                    enqueue(packet(0x12,config_sequence,time.monotonic_ns(),json.dumps(config).encode()))
                    config_sequence+=1
            except (OSError,ValueError,http.client.HTTPException):
                pass
            finally:
                connection.close()
        for key, mask in selector.select(.01):
            sock, kind=key.fileobj,key.data
            if kind=='listener':
                client,_=local.accept(); client.setblocking(False)
                selector.register(client, selectors.EVENT_READ, CameraParser())
            elif kind=='imu':
                for _ in range(128):
                    try:
                        data=udp.recv(2048)
                    except BlockingIOError:
                        break
                    if len(data)!=72 or data[:4]!=b'OIMU':
                        invalid+=1;continue
                    seq,accel,gyro=struct.unpack_from('>QQQ',data,8)
                    enqueue(packet(0x11,seq,min(accel,gyro),data))
            elif kind=='bulk':
                try:
                    if mask & selectors.EVENT_READ:
                        if not sock.recv(4096):
                            raise ConnectionError('BSP bulk disconnected')
                    if mask & selectors.EVENT_WRITE and outgoing:
                        n=sock.send(outgoing[0])
                        if n<=0:raise ConnectionError('BSP bulk send failed')
                        queued-=n;sent+=n
                        outgoing[0]=outgoing[0][n:]
                        if not outgoing[0]:outgoing.popleft()
                        if not outgoing:selector.modify(sock,selectors.EVENT_READ,'bulk')
                except BlockingIOError:
                    pass
                except OSError:
                    selector.unregister(sock);sock.close();bulk=None
                    dropped+=len(outgoing);outgoing.clear();queued=0
            else:
                try:
                    data=sock.recv(256*1024)
                    if not data:raise ConnectionError('camera disconnected')
                    for frame in kind.feed(data):enqueue(frame)
                except BlockingIOError:
                    pass
                except (OSError,ValueError) as exc:
                    if kind.pending:invalid+=1
                    selector.unregister(sock);sock.close()
        if now>=next_report:
            next_report=now+10
            print(json.dumps(dict(sent_bytes=sent,queued_bytes=queued,dropped=dropped,invalid=invalid)),flush=True)


if __name__=='__main__':
    main()
