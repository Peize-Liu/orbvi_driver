# ORBVI 四目相机 + IMU 驱动

提供原生 C++、ROS 2 Humble、ROS 1 Noetic 三种接口，统一使用 [config/orbvi.yaml](config/orbvi.yaml)。

默认通过原生 USB Bulk 传输四路图像和 IMU。ADB 仅用于部署、启动和停止板端程序。

## 安装与启动

以下命令在本包目录执行。Mac 需安装 Python 3（含 PyYAML）、ADB、Docker、C++ 编译器、pkg-config 和 libusb，开发板通过 USB 连接。

```sh
brew install libusb pkg-config
python3 -m pip install PyYAML
adb devices -l
python3 tools/build.py all       # 首次构建
python3 tools/control.py deploy  # 首次部署，或更新板端程序
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
```

板端脚本默认使用 `/userdata/x5-dev/python/bin/python3`；其他安装位置可通过环境变量 `ORBVI_BOARD_PYTHON` 指定。

选择需要的接口启动：

```sh
# C++ 接收示例：采集 60 秒并输出统计
python3 tools/control.py capture --config config/orbvi.yaml --seconds 60

# ROS 2：持续发布，Ctrl+C 停止
python3 tools/run_ros.py ros2 --config config/orbvi.yaml

# ROS 1：自动启动 roscore，Ctrl+C 停止
python3 tools/run_ros.py ros1 --config config/orbvi.yaml

# ROS 2 + 网页预览，后台运行
python3 tools/run_ros.py ros2 --config config/orbvi.yaml --dashboard
```

网页地址：<http://127.0.0.1:18765/>。ROS 节点运行在 Docker 容器内；Mac 的本机 libusb 接收器将 USB 数据交给容器。

停止网页和板端采集：

```sh
docker stop orbvi-dashboard      # 仅在网页运行时执行
python3 tools/control.py stop
python3 tools/usb_host.py stop
```

## 可选配置

编辑 `config/orbvi.yaml` 中的对应字段，其余字段保留默认值。

| 字段 | 可选值 | 默认值 |
| --- | --- | --- |
| `transport.mode` | `usb` 原生 Bulk；`adb` 兼容转发 | `usb` |
| `transport.usb_backend` | `bridge` 接收本机 USB 服务；`direct` 在 Linux 直接打开 USB | `bridge` |
| `transport.usb_port` | 本机 USB 服务端口，网页另用此端口 +10…+13 | 15700 |
| `transport.usb_serial` | 多板连接时指定 USB 序列号；空值自动选择唯一设备 | 空 |
| `camera.width` / `camera.height` | 1280×1088、1280×720、720×480、640×480、480×720、480×640 | 1280×1088 |
| `camera.fps` | 25、30、40、50、60 | 60 |
| `camera.bitrate_kbps` | 1–100000，单位 kbps，每路编码码率 | 4000 |
| `imu.frequency_hz` | 当前 BSP 仅支持 1000 | 1000 |
| `imu.accel_range_g` | 当前 BSP 仅支持 16，表示 ±16 g | 16 |
| `imu.gyro_range_dps` | 当前 BSP 仅支持 2000，表示 ±2000 °/s | 2000 |
| `ros.topic_prefix` | ROS Topic 前缀 | `/orbvi/raw` |
| `ros.image_queue` / `ros.imu_queue` | 图像队列 1–64，IMU 队列 1–65536 | 16 / 2048 |
| `runtime.image_queue` / `runtime.imu_queue` | C++ 回调队列：图像 1–256，IMU 1–65536 | 64 / 2048 |

四路相机共用分辨率和帧率，上述尺寸可搭配任一列出的帧率。IMU 不支持的频率或量程会在启动时报错。

例如，将相机改为 640×480、30 Hz，修改以下部分：

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

配置修改后，先停止接收器，再重启板端采集和所需接口：

```sh
python3 tools/control.py stop
python3 tools/usb_host.py stop
python3 tools/control.py check --config config/orbvi.yaml
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
python3 tools/run_ros.py ros2 --config config/orbvi.yaml
```

## ROS Topics

ROS 1 和 ROS 2 使用相同的 Topic 名称：

| Topic | 内容 |
| --- | --- |
| `/orbvi/raw/camera_0/image` … `/orbvi/raw/camera_3/image` | `sensor_msgs/Image`，RGB8 |
| `/orbvi/raw/camera_0/metadata` … `/orbvi/raw/camera_3/metadata` | 帧号、采集时间戳 |
| `/orbvi/raw/imu` | `sensor_msgs/Imu`，加速度 m/s²、角速度 rad/s |
| `/orbvi/raw/imu/metadata` | IMU 序号、加速度和陀螺仪时间戳 |
| `/orbvi/raw/diagnostics` | 采集统计与异常计数 |

图像是 H.264 解码后的 RGB。图像和 IMU 时间戳使用板端单调时钟；与其他设备融合前需统一时钟域。IMU 消息不包含姿态估计。

## C++ 接入

Linux 编译需要 CMake、C++17、pkg-config、libusb、yaml-cpp 和 FFmpeg 4.4+ 开发库：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j4
cmake --install build
```

将安装目录加入应用的 `CMAKE_PREFIX_PATH`，在 CMake 中链接：

```cmake
find_package(orbvi_driver REQUIRED)
target_link_libraries(your_app PRIVATE orbvi_driver::driver)
```

使用示例：

```cpp
#include <orbvi_driver/driver.hpp>

auto config = orbvi_driver::load_config("/absolute/path/orbvi.yaml");
orbvi_driver::Driver driver(config);
driver.start({
    [](orbvi_driver::Image image) {
        // image.camera：0..3；image.rgb：RGB8 像素。
    },
    [](orbvi_driver::Imu imu) {
        // imu.acceleration：m/s²；imu.angular_velocity：rad/s。
    }
});
// 在应用主循环中处理数据。
driver.stop();
```

回调中避免耗时操作，不要在回调内调用 `stop()`。

原生 Linux 接收时设置 `transport.mode: usb` 和 `transport.usb_backend: direct`，应用直接打开 USB，无需本机中继。运行用户需有 USB 设备访问权限。Ubuntu 可安装设备规则，然后重新插拔 USB：

```sh
sudo install -m 644 config/99-orbvi-usb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

如需使用原来的 ADB 模式，停止接收器、USB 本机服务和板端采集后，使用 `config/orbvi-adb.yaml` 启动板端及所需接口，无需启动 `usb_host.py`。
