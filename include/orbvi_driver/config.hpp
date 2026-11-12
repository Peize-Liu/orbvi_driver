#pragma once
#include <string>
#include <utility>
#include <vector>

namespace orbvi_driver {
struct Config {
  int width=1280, height=1088, fps=60, bitrate_kbps=4000;
  double frequency_hz=1000, accel_range_g=16, gyro_range_dps=2000;
  std::string host="host.docker.internal", rtsp_path="/live/PRR", imu_bind_address="0.0.0.0";
  std::string transport_mode="adb",usb_backend="bridge",usb_serial;
  int usb_port=15700;
  int rtsp_base_port=15554, imu_port=5600, control_port=15602;
  int image_queue=64, imu_queue=2048, ros_image_queue=16, ros_imu_queue=2048;
  std::string topic_prefix="/orbvi/raw";
};
Config load_config(const std::string& path);
void validate(const Config& config);
const std::vector<std::pair<int,int>>& camera_sizes();
const std::vector<int>& camera_rates();
std::string json_quote(const std::string& text);
}
