# ORBVI Four-Camera + IMU Driver

[English](README.en.md) | [简体中文](README.md)

Native C++, ROS 2 Humble and ROS 1 Noetic interfaces share a single configuration file: [config/orbvi.yaml](config/orbvi.yaml).

The default transport is native USB Bulk for all four cameras and the IMU. ADB is used to deploy, start and stop the board software.

## Installation and startup

Run the following commands from the repository root. On macOS, install Python 3 with PyYAML, ADB, Docker, a C++ compiler, pkg-config and libusb. Connect the board over USB.

```sh
brew install libusb pkg-config
python3 -m pip install PyYAML
adb devices -l
python3 tools/build.py all       # Initial build
python3 tools/control.py deploy  # Initial deployment or board software update
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
```

Board scripts use `/userdata/x5-dev/python/bin/python3` by default. Set `ORBVI_BOARD_PYTHON` if Python is installed elsewhere on the board.

Start the interface you need:

```sh
# C++ receiver example: capture for 60 seconds and print statistics
python3 tools/control.py capture --config config/orbvi.yaml --seconds 60

# ROS 2: publish continuously; Ctrl+C to stop
python3 tools/run_ros.py ros2 --config config/orbvi.yaml

# ROS 1: start roscore automatically; Ctrl+C to stop
python3 tools/run_ros.py ros1 --config config/orbvi.yaml

# ROS 2 with a browser preview, running in the background
python3 tools/run_ros.py ros2 --config config/orbvi.yaml --dashboard
```

Open <http://127.0.0.1:18765/> for the browser preview. ROS nodes run in Docker; the native libusb receiver on the Mac forwards USB data to the container.

## Electron desktop viewer

See [desktop/README.en.md](desktop/README.en.md) for dependency installation, building, packaging and running the viewer. The current packaging target is Apple Silicon Mac.

Start the USB receiver, board acquisition and `--dashboard` service as described above, then launch the viewer with Node.js 22.12 or newer:

```sh
cd desktop
npm ci
npm start
```

The viewer displays four RGB images, capture and display frame rates, three-axis IMU plots, camera timestamp skew and diagnostic counters. Click an image for fullscreen. Use the buttons to pause the display, reconnect or export a JSON status snapshot. Closing the viewer leaves acquisition running.

The default **Low latency · Latest frame** mode decodes incoming images immediately and discards older preview frames when necessary. Use the selector to switch to **Smooth · 300 ms buffer**. “Receive → draw” measures only the viewer's processing time, not end-to-end latency. IMU plots use approximately 50 Hz samples, and the four preview images are displayed independently.

Package and open the Apple Silicon Mac application from `desktop/`:

```sh
npm run package
open "dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app"
```

The default service address is `http://127.0.0.1:18765`. To use another local preview service port:

```sh
ORBVI_DASHBOARD_URL=http://127.0.0.1:18766 npm start
```

## Stopping acquisition

Run these commands from the repository root:

```sh
docker stop orbvi-dashboard      # Only if the dashboard is running
python3 tools/control.py stop
python3 tools/usb_host.py stop
```

## Configuration options

Edit the required fields in `config/orbvi.yaml` and keep the other defaults.

| Field | Available values | Default |
| --- | --- | --- |
| `transport.mode` | `usb`: native Bulk; `adb`: compatibility forwarding | `usb` |
| `transport.usb_backend` | `bridge`: use the local USB service; `direct`: open USB directly on Linux | `bridge` |
| `transport.usb_port` | Local USB service port; smooth preview also uses ports +10 through +13 | 15700 |
| `transport.usb_serial` | Select a board by USB serial number; empty selects the single connected device | Empty |
| `camera.width` / `camera.height` | 1280×1088, 1280×720, 720×480, 640×480, 480×720, 480×640 | 1280×1088 |
| `camera.fps` | 25, 30, 40, 50, 60 | 60 |
| `camera.bitrate_kbps` | 1–100000 kbps per camera | 4000 |
| `imu.frequency_hz` | The current BSP supports only 1000 | 1000 |
| `imu.accel_range_g` | The current BSP supports only 16, meaning ±16 g | 16 |
| `imu.gyro_range_dps` | The current BSP supports only 2000, meaning ±2000 °/s | 2000 |
| `ros.topic_prefix` | ROS topic prefix | `/orbvi/raw` |
| `ros.image_queue` / `ros.imu_queue` | Image queue: 1–64; IMU queue: 1–65536 | 16 / 2048 |
| `runtime.image_queue` / `runtime.imu_queue` | C++ callback queues: images 1–256; IMU 1–65536 | 64 / 2048 |

All four cameras share the same resolution and frame rate. Each listed resolution can be used with any listed frame rate. Unsupported IMU frequencies or ranges cause a startup error.

For example, change the following fields to use 640×480 at 30 Hz:

```yaml
camera:
  width: 640
  height: 480
  fps: 30
  bitrate_kbps: 4000
imu:
  frequency_hz: 1000
  accel_range_g: 16
  gyro_range_dps: 2000
```

After editing the configuration, stop any running receivers, then restart acquisition and the required interface from the repository root:

```sh
python3 tools/control.py stop
python3 tools/usb_host.py stop
python3 tools/control.py check --config config/orbvi.yaml
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
python3 tools/run_ros.py ros2 --config config/orbvi.yaml
```

## ROS topics

ROS 1 and ROS 2 use the same topic names:

| Topic | Content |
| --- | --- |
| `/orbvi/raw/camera_0/image` … `/orbvi/raw/camera_3/image` | `sensor_msgs/Image`, RGB8 |
| `/orbvi/raw/camera_0/metadata` … `/orbvi/raw/camera_3/metadata` | Frame sequence and capture timestamp |
| `/orbvi/raw/imu` | `sensor_msgs/Imu`, acceleration in m/s² and angular velocity in rad/s |
| `/orbvi/raw/imu/metadata` | IMU sequence, accelerometer and gyroscope timestamps |
| `/orbvi/raw/diagnostics` | Acquisition statistics and error counters |

Images are RGB decoded from H.264. Image and IMU timestamps use the board's monotonic clock; align clock domains before fusing data with other devices. IMU messages do not include an orientation estimate.

## C++ integration

Building on Linux requires CMake, C++17, pkg-config, libusb, yaml-cpp and FFmpeg 4.4+ development libraries:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j4
cmake --install build
```

Add the installation directory to your application's `CMAKE_PREFIX_PATH`, then link the driver:

```cmake
find_package(orbvi_driver REQUIRED)
target_link_libraries(your_app PRIVATE orbvi_driver::driver)
```

Example:

```cpp
#include <orbvi_driver/driver.hpp>

auto config = orbvi_driver::load_config("/absolute/path/orbvi.yaml");
orbvi_driver::Driver driver(config);
driver.start({
    [](orbvi_driver::Image image) {
        // image.camera: 0..3; image.rgb: RGB8 pixels.
    },
    [](orbvi_driver::Imu imu) {
        // imu.acceleration: m/s²; imu.angular_velocity: rad/s.
    }
});
// Process data in your application's main loop.
driver.stop();
```

Avoid lengthy work in callbacks and do not call `stop()` from a callback.

For direct USB reception on Linux, set `transport.mode: usb` and `transport.usb_backend: direct`. The application opens USB directly without a host bridge. The user running the application needs access to the USB device. On Ubuntu, install the device rule and reconnect the USB cable:

```sh
sudo install -m 644 config/99-orbvi-usb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

To use ADB mode, stop the receivers, local USB service and board acquisition, then start the board and the required interface with `config/orbvi-adb.yaml`. Do not start `usb_host.py` in ADB mode.
