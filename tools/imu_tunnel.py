#!/usr/bin/env python3
"""Carry unchanged UDP datagrams over an ADB-forwarded, length-framed TCP stream."""
import argparse
import select
import socket
import struct
import time


class Decoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        packets = []
        while len(self.buffer) >= 2:
            size = struct.unpack_from('!H', self.buffer)[0]
            if not 1 <= size <= 65507:
                raise ValueError('invalid datagram length')
            if len(self.buffer) < size + 2:
                break
            packets.append(bytes(self.buffer[2:size + 2]))
            del self.buffer[:size + 2]
        return packets


def server(tcp_port, udp_port):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    udp.bind(('127.0.0.1', udp_port))
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', tcp_port))
    listener.listen(8)
    clients = {}
    received = discarded = disconnected = 0
    last_report = time.monotonic()
    print(f'UDP {udp_port} -> TCP {tcp_port} fanout ready', flush=True)

    def remove(sock):
        nonlocal disconnected
        clients.pop(sock, None)
        sock.close()
        disconnected += 1

    while True:
        readable, writable, _ = select.select(
            [udp, listener, *clients], [s for s, pending in clients.items() if pending], [], 1)
        if listener in readable:
            sock, _ = listener.accept()
            if len(clients) >= 8:
                sock.close()
            else:
                sock.setblocking(False)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                clients[sock] = bytearray()
                print(f'TCP connected; clients={len(clients)}', flush=True)
        for sock in readable:
            if sock not in clients:
                continue
            try:
                if not sock.recv(4096):
                    remove(sock)
            except BlockingIOError:
                pass
            except OSError:
                remove(sock)
        if udp in readable:
            packet = udp.recv(65535)
            received += 1
            if not clients:
                discarded += 1
            frame = struct.pack('!H', len(packet)) + packet
            for sock, pending in list(clients.items()):
                pending.extend(frame)
                if len(pending) > 1024 * 1024:
                    print('Slow client exceeded 1 MiB; disconnected without affecting peers', flush=True)
                    remove(sock)
        for sock in writable:
            if sock not in clients:
                continue
            try:
                sent = sock.send(clients[sock])
                del clients[sock][:sent]
            except BlockingIOError:
                pass
            except OSError:
                remove(sock)
        if time.monotonic() - last_report >= 10:
            print(f'received={received} discarded_without_client={discarded} '
                  f'disconnects={disconnected} clients={len(clients)}', flush=True)
            last_report = time.monotonic()


def client(host, tcp_port, udp_port):
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.connect(('127.0.0.1', udp_port))
    total = 0
    last_report = time.monotonic()
    while True:
        try:
            with socket.create_connection((host, tcp_port), timeout=5) as connection:
                connection.settimeout(10)
                decoder = Decoder()
                print('TCP connected', flush=True)
                while True:
                    data = connection.recv(65536)
                    if not data:
                        raise ConnectionError('peer closed')
                    for packet in decoder.feed(data):
                        try:
                            udp.send(packet)
                            total += 1
                        except ConnectionRefusedError:
                            pass  # ROS receiver is not yet listening; never replay stale data.
                    if time.monotonic() - last_report >= 10:
                        print(f'forwarded={total}', flush=True)
                        last_report = time.monotonic()
        except (OSError, ValueError) as exc:
            print(f'reconnecting: {exc}', flush=True)
            time.sleep(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['server', 'client'])
    parser.add_argument('--host', default='host.docker.internal')
    parser.add_argument('--tcp-port', type=int, required=True)
    parser.add_argument('--udp-port', type=int, required=True)
    args = parser.parse_args()
    if args.mode == 'server':
        server(args.tcp_port, args.udp_port)
    else:
        client(args.host, args.tcp_port, args.udp_port)
