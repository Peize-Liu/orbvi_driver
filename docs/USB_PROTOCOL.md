# USB Bulk transport

The driver uses the existing BSP `ffs.x5-data` FunctionFS endpoint and
`/run/x5-usb-gadget/bulk.sock`. It does not replace or rebind the gadget.
VID is 0x3652; supported PIDs are 0x1055 and 0x1056. Hosts discover interface
class/subclass/protocol ff/58/35 and its Bulk IN endpoint rather than assuming
an interface or endpoint number. ADB remains available for deployment/control.

This is an ORBVI driver extension over the BSP X5US v1 envelope. It is not the
legacy UVC atlas stream or the legacy IMU payload. Existing UVC and third-party
host software will not automatically understand these extension messages.

Every message has this 24-byte little-endian header:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII X5US |
| 4 | 1 | Version 1 |
| 5 | 1 | Message type |
| 6 | 2 | Reserved, zero |
| 8 | 4 | Payload bytes, bounded to 4 MiB |
| 12 | 4 | Sequence |
| 16 | 8 | Board monotonic timestamp, ns |

Types:

- 0x10: camera access unit. First four payload bytes are the little-endian
  camera index (0..3), followed by Annex B H.264 including the existing capture
  SEI. Header sequence and timestamp must match the decoded SEI. The current
  encoder emits all-intra pictures, exactly one decoded picture per message.
- 0x11: unchanged 72-byte OIMU/1 record, including its CRC, 64-bit sequence,
  separate accelerometer/gyro timestamps and SI measurements. Header sequence
  contains the low 32 bits; header timestamp is the earlier sensor timestamp.
- 0x12: UTF-8 JSON configuration/capability snapshot, sent once per second.
  Contains usb_protocol=1, usb_mux_dropped and usb_mux_invalid. The receiver
  verifies resolution, FPS, IMU frequency/scales and transport mode before
  delivering samples. Configuration mismatches terminate the receiver.

Image and IMU messages originate from the same acquisition used by the ADB
backend. Camera access units go through a local Unix socket; IMU records use a
board-local UDP socket. Only the board-local USB publisher talks to bulk.sock.
No RTSP, UDP or configuration stream crosses an ADB forward in USB mode.
The existing board RTSP listener remains available locally for readiness checks.

Linux C++ applications can claim USB directly (`usb_backend: direct`). The
macOS helper claims the same interface and passes complete X5US messages to
Docker clients over loopback TCP (`usb_backend: bridge`). Port 15700 is the
sample stream; ports 15710–15713 expose the same compressed camera data for
web preview. These host-local ports do not connect to board TCP ports.

Receiver queues are bounded and expose dropped-message counts. The host bridge
allows up to 16 clients and disconnects a client above 8 MiB pending data. The
board publisher has an 8 MiB outgoing bound; incomplete local camera messages
are discarded and counted. USB unplug, invalid framing, corrupt IMU data,
configuration mismatch and decode/SEI mismatch fail explicitly. A receiver must
be restarted after disconnect. For restart, stop receivers, stop board acquisition while the host helper
continues draining USB, then stop the helper. Start the helper before starting
board acquisition again. Only initial USB stream attachment seeks the next
valid header; subsequent framing errors are fatal.

ROS timestamps and camera numbering remain identical to the C++ interface.
The default mode is four 1280×1088 H.264 streams at 60 Hz and nominal 1000 Hz
IMU; the stock UVC 2560×2176@30 descriptor is not changed.
