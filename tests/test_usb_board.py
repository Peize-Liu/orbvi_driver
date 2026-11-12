#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct
import unittest
spec=importlib.util.spec_from_file_location('usb_board',Path(__file__).resolve().parents[1]/'tools/usb_board.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Framing(unittest.TestCase):
    def message(self):
        return module.packet(0x10,42,123456,struct.pack('<I',2)+b'\0\0\0\1\x65test')
    def test_fragments_and_coalescing(self):
        message=self.message();parser=module.CameraParser();frames=[]
        for byte in message:frames+=parser.feed(bytes([byte]))
        self.assertEqual(frames,[message])
        self.assertEqual(parser.feed(message*3),[message]*3)
    def test_partial_record_is_not_forwarded(self):
        message=self.message();parser=module.CameraParser()
        self.assertEqual(parser.feed(message[:-1]),[])
        self.assertEqual(parser.feed(message[-1:]),[message])
    def test_invalid_size(self):
        data=bytearray(self.message());struct.pack_into('<I',data,8,0xffffffff)
        with self.assertRaises(ValueError):module.CameraParser().feed(data)
    def test_invalid_camera(self):
        data=bytearray(self.message());struct.pack_into('<I',data,24,4)
        with self.assertRaises(ValueError):module.CameraParser().feed(data)

if __name__=='__main__':unittest.main()
