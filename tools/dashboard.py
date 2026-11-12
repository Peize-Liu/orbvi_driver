#!/usr/bin/env python3
"""Local, read-only ROS 2 live dashboard. Preview decimation never changes capture."""
from collections import deque
from functools import partial
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
import threading
import time
import yaml

CONFIG = yaml.safe_load(Path("/capture.yaml").read_text())

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import Imu
from diagnostic_msgs.msg import DiagnosticArray
from orbvi_driver_ros2.msg import FrameMetadata


def stamp_ns(stamp):
    return stamp.sec * 1000000000 + stamp.nanosec


class Stream:
    def __init__(self):
        self.samples = deque(maxlen=2200)
        self.sequence = None
        self.stamp = 0
        self.last = 0
        self.count = self.gaps = self.resets = 0

    def update(self, sequence, stamp):
        now = time.monotonic()
        if self.sequence is not None:
            if sequence > self.sequence + 1:
                self.gaps += sequence - self.sequence - 1
            elif sequence <= self.sequence:
                self.resets += 1
                self.samples.clear()
        self.sequence, self.stamp, self.last = sequence, stamp, now
        self.count += 1
        self.samples.append((now, stamp))
        while len(self.samples) > 2 and self.samples[0][0] < now - 2:
            self.samples.popleft()

    def snapshot(self, now):
        active = bool(self.last and now - self.last < 2)
        span = self.samples[-1][1] - self.samples[0][1] if len(self.samples) > 1 else 0
        rate = (len(self.samples) - 1) * 1e9 / span if span > 0 and active else 0
        return dict(active=active, rate=rate, count=self.count, gaps=self.gaps,
                    resets=self.resets, sequence=self.sequence, stamp_ns=str(self.stamp),
                    age_ms=(now - self.last) * 1000 if self.last else None)


class Dashboard(Node):
    def __init__(self):
        super().__init__('orbvi_dashboard')
        self.lock = threading.Lock()
        self.cameras = [Stream() for _ in range(4)]
        self.imu = Stream()
        self.imu_rate = 0.
        self.jpegs = [None] * 4
        self.image_times = [0.] * 4
        self.image_counts = [0] * 4
        self.image_stamps = [0] * 4
        self.sizes = [None] * 4
        self.history = deque(maxlen=600)
        self.last_plot = 0
        self.latest_imu = None
        self.sync_pending = {}
        self.sync_skews = deque(maxlen=600)
        self.sync_groups = 0
        self.started = time.monotonic()
        metadata_qos = QoSProfile(depth=2048, reliability=ReliabilityPolicy.RELIABLE)
        self.subscriptions_owned = []
        for index in range(4):
            base = CONFIG['ros']['topic_prefix'] + f'/camera_{index}'
            self.subscriptions_owned += [
                self.create_subscription(FrameMetadata, base + '/metadata',
                                         partial(self.camera_meta, index), metadata_qos),
                # Full-rate video uses the same board encoder over RTSP, avoiding image copies in Python.
                ]
        self.subscriptions_owned += [
            self.create_subscription(DiagnosticArray, CONFIG['ros']['topic_prefix'] + '/diagnostics', self.diagnostics, 10),
            self.create_subscription(Imu, CONFIG['ros']['topic_prefix'] + '/imu', self.imu_sample,
                                     QoSProfile(depth=1, reliability=ReliabilityPolicy.BEST_EFFORT))]

    def camera_meta(self, index, message):
        with self.lock:
            self.cameras[index].update(message.sequence, message.sensor_timestamp_ns)
            stamp = message.sensor_timestamp_ns
            matches = [k for k, v in self.sync_pending.items() if abs(k-stamp) < 1000000 and index not in v]
            key = min(matches, key=lambda k: abs(k-stamp)) if matches else stamp
            group = self.sync_pending.setdefault(key, {})
            group[index] = message.sensor_timestamp_ns
            if len(group) == 4:
                self.sync_skews.append(max(group.values()) - min(group.values()))
                self.sync_groups += 1
                del self.sync_pending[key]
            while len(self.sync_pending) > 256:
                del self.sync_pending[next(iter(self.sync_pending))]

    def diagnostics(self, message):
        for status in message.status:
            if status.name != 'orbvi_driver':
                continue
            values = {v.key: v.value for v in status.values}
            if 'imu_last_timestamp_ns' not in values:
                continue
            count, stamp = int(values['imu_count']), int(values['imu_last_timestamp_ns'])
            with self.lock:
                advanced = stamp > 0 and stamp != self.imu.stamp
                if self.imu.stamp and stamp > self.imu.stamp and count >= self.imu.count:
                    self.imu_rate = (count-self.imu.count)*1e9/(stamp-self.imu.stamp)
                else:
                    self.imu_rate = 0.
                self.imu.count, self.imu.stamp = count, stamp
                self.imu.sequence = int(values['imu_last_sequence'])
                self.imu.gaps = int(values['imu_gaps'])
                self.imu.resets = int(values['imu_discontinuities'])
                if advanced:
                    self.imu.last = time.monotonic()

    def imu_sample(self, message):
        now = time.monotonic()
        accel, gyro = message.linear_acceleration, message.angular_velocity
        sample = dict(t=now - self.started, stamp_ns=str(stamp_ns(message.header.stamp)),
                      accel=[accel.x, accel.y, accel.z], gyro=[gyro.x, gyro.y, gyro.z])
        with self.lock:
            self.latest_imu = sample
            if now - self.last_plot >= .02:
                self.history.append(sample)
                self.last_plot = now

    def snapshot(self):
        now = time.monotonic()
        with self.lock:
            cameras = []
            for i, stream in enumerate(self.cameras):
                item = stream.snapshot(now)
                item.update(size=self.sizes[i], image_stamp_ns=str(self.image_stamps[i]),
                            image_age_ms=(now-self.image_times[i])*1000 if self.image_times[i] else None,
                            preview_received=self.image_counts[i])
                cameras.append(item)
            imu = self.imu.snapshot(now)
            imu.update(rate=self.imu_rate if imu['active'] else 0,
                       statistics_source='C++ driver diagnostics; preview receives latest samples only')
            return dict(configuration=CONFIG, cameras=cameras, imu=imu, latest_imu=self.latest_imu,
                        history=list(self.history), uptime=now-self.started,
                        sync=dict(groups=self.sync_groups, max_skew_us=max(self.sync_skews, default=0)/1000))


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?')[0]
        if path in [f'/video/{i}.mp4' for i in range(4)]:
            self.video(int(path[7]))
            return
        if path == '/api/status':
            data, mime = json.dumps(self.server.node.snapshot()).encode(), 'application/json'
        elif path == '/':
            data, mime = Path(__file__).with_name('dashboard.html').read_bytes(), 'text/html; charset=utf-8'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def video(self, index):
        if CONFIG['transport'].get('mode','adb') == 'usb':
            endpoint = f"tcp://{CONFIG['transport']['host']}:{CONFIG['transport'].get('usb_port',15700)+10+index}"
            source = ['-r',str(CONFIG['camera']['fps']),'-f','h264','-i',endpoint]
        else:
            source = ['-rtsp_transport','tcp','-stimeout','10000000','-i',
                      f"rtsp://{CONFIG['transport']['host']}:{CONFIG['transport']['rtsp_base_port']+index}{CONFIG['transport']['rtsp_path']}"]
        command = ['ffmpeg', '-hide_banner', '-loglevel', 'error', *source,
                   '-map', '0:v:0', '-an', '-c:v', 'copy', '-movflags',
                   'empty_moov+default_base_moof+frag_keyframe', '-frag_duration', '33333',
                   '-flush_packets', '1', '-f', 'mp4', 'pipe:1']
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'video/mp4')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Connection', 'close')
            self.end_headers()
            while True:
                data = process.stdout.read1(65536)
                if not data:
                    break
                self.wfile.write(data)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.close_connection = True
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            process.stdout.close()

    def log_message(self, *args):
        pass


if __name__ == '__main__':
    rclpy.init()
    node = Dashboard()
    server = ThreadingHTTPServer(('0.0.0.0', 8765), Handler)
    server.node = node
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print('Dashboard ready on :8765', flush=True)
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, rclpy.executors.ExternalShutdownException):
        pass
    finally:
        server.shutdown()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()
