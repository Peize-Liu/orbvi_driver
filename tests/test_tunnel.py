import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import struct
import unittest
from imu_tunnel import Decoder
import socket
import subprocess
import time


class FramingTest(unittest.TestCase):
    def test_multiple_clients_receive_identical_packets(self):
        def free_port(kind):
            with socket.socket(socket.AF_INET, kind) as sock:
                sock.bind(('127.0.0.1', 0))
                return sock.getsockname()[1]
        tcp, udp = free_port(socket.SOCK_STREAM), free_port(socket.SOCK_DGRAM)
        process = subprocess.Popen([sys.executable, str(Path(__file__).resolve().parents[1]/'tools/imu_tunnel.py'),
                                    'server', '--tcp-port', str(tcp), '--udp-port', str(udp)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        clients=[]
        try:
            deadline=time.monotonic()+3
            while len(clients)<2:
                try:
                    clients.append(socket.create_connection(('127.0.0.1',tcp),timeout=.5))
                except OSError:
                    if time.monotonic()>deadline: raise
                    time.sleep(.02)
            time.sleep(.05)
            packets=[b'OIMU'+bytes([i])*68 for i in range(20)]
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sender:
                for packet in packets: sender.sendto(packet,('127.0.0.1',udp))
            for client in clients:
                decoder=Decoder();received=[]
                while len(received)<len(packets):received+=decoder.feed(client.recv(4096))
                self.assertEqual(received,packets)
            clients[0].close()
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sender:
                sender.sendto(b'peer survives',('127.0.0.1',udp))
            decoder=Decoder();received=[]
            while not received:received+=decoder.feed(clients[1].recv(4096))
            self.assertEqual(received,[b'peer survives'])
        finally:
            for client in clients:client.close()
            process.terminate();process.wait(timeout=3);process.stderr.close()

    def test_fragmentation_and_coalescing(self):
        packets = [b'OIMU' + bytes(range(68)), bytes(range(255)), b'x']
        wire = b''.join(struct.pack('!H', len(p)) + p for p in packets)
        for chunk in (1, 2, 3, 71, 72, 73, 1000):
            decoder = Decoder()
            result = []
            for offset in range(0, len(wire), chunk):
                result.extend(decoder.feed(wire[offset:offset + chunk]))
            self.assertEqual(result, packets)
            self.assertEqual(decoder.buffer, b'')

    def test_invalid_length(self):
        for length in (0, 65508, 65535):
            with self.assertRaises(ValueError):
                Decoder().feed(struct.pack('!H', length))

    def test_partial_packet_is_not_emitted(self):
        decoder = Decoder()
        self.assertEqual(decoder.feed(b'\x00\x04abc'), [])
        self.assertEqual(decoder.feed(b'd'), [b'abcd'])


if __name__ == '__main__':
    unittest.main()
