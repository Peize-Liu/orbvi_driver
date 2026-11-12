#include "orbvi_driver/config.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace orbvi_driver {
const std::vector<std::pair<int,int>>& camera_sizes() {
  // The fixed 90-degree board pipeline scales before rotation. Portrait
  // height 1280 would require width upscaling on a downscale-only VSE channel.
  static const std::vector<std::pair<int,int>> values{{1280,1088},{1280,720},{720,480},{640,480},{480,720},{480,640}};
  return values;
}
const std::vector<int>& camera_rates() { static const std::vector<int> v{25,30,40,50,60}; return v; }
void validate(const Config& c) {
  if(c.transport_mode!="adb"&&c.transport_mode!="usb")throw std::runtime_error("transport.mode must be adb or usb");
  if(c.usb_backend!="direct"&&c.usb_backend!="bridge")throw std::runtime_error("transport.usb_backend must be direct or bridge");
  if(c.usb_port<1||c.usb_port>65535)throw std::runtime_error("invalid USB bridge port");
  if (std::find(camera_sizes().begin(),camera_sizes().end(),std::make_pair(c.width,c.height))==camera_sizes().end())
    throw std::runtime_error("camera resolution is not validated: use 1280x1088, 1280x720, 720x480, 640x480, 480x720 or 480x640; portrait height 1280 fails this BSP pipeline");
  if (std::find(camera_rates().begin(),camera_rates().end(),c.fps)==camera_rates().end())
    throw std::runtime_error("camera.fps must be 25, 30, 40, 50 or 60 (see hardware test report)");
  if (c.bitrate_kbps<1 || c.bitrate_kbps>100000) throw std::runtime_error("invalid bitrate_kbps");
  if (!std::isfinite(c.frequency_hz)||c.frequency_hz<1||c.frequency_hz>2000||std::floor(c.frequency_hz)!=c.frequency_hz)
    throw std::runtime_error("invalid IMU frequency; board capabilities must also be checked");
  if (c.accel_range_g!=2 && c.accel_range_g!=4 && c.accel_range_g!=8 && c.accel_range_g!=16)
    throw std::runtime_error("accel_range_g must be 2, 4, 8 or 16; BSP support is checked on board");
  const std::set<double> gyro{15.625,31.25,62.5,125,250,500,1000,2000};
  if (!gyro.count(c.gyro_range_dps)) throw std::runtime_error("invalid gyro_range_dps");
  if (c.host.empty()||c.host.find_first_of(" /\r\n\t")!=std::string::npos ||
      c.rtsp_path.size()<2||c.rtsp_path[0]!='/'||c.rtsp_path.find_first_of(" \r\n\t")!=std::string::npos ||
      c.rtsp_base_port<1||c.rtsp_base_port>65532||c.imu_port<1||c.imu_port>65535||c.control_port<1||c.control_port>65535)
    throw std::runtime_error("invalid transport settings");
  if(c.image_queue<1||c.image_queue>256||c.imu_queue<1||c.imu_queue>65536||
     c.ros_image_queue<1||c.ros_image_queue>64||c.ros_imu_queue<1||c.ros_imu_queue>65536)
    throw std::runtime_error("queue outside bounded range");
  if(!std::regex_match(c.topic_prefix,std::regex("(/[A-Za-z_][A-Za-z_0-9]*)+")))
    throw std::runtime_error("invalid ros.topic_prefix");
  if(c.rtsp_path!="/live/PRR")throw std::runtime_error("USB board profile requires rtsp_path=/live/PRR");
}
static void keys(const YAML::Node& n, std::initializer_list<const char*> allowed, const std::string& path,std::initializer_list<const char*> optional={}) {
  if(!n.IsMap()) throw std::runtime_error(path+" must be a map");
  std::set<std::string> valid,seen; for(auto k:allowed) valid.insert(k);
  for(auto k:optional)valid.insert(k);
  for(auto entry:n) { auto k=entry.first.as<std::string>();
    if(!valid.count(k)||!seen.insert(k).second) throw std::runtime_error("unknown or duplicate key: "+path+"."+k); }
  for(auto k:allowed) if(!n[k]) throw std::runtime_error("missing key: "+path+"."+k);
}
Config load_config(const std::string& path) {
  auto root=YAML::LoadFile(path);
  keys(root,{"schema_version","camera","imu","transport","runtime","ros"},"config");
  if(root["schema_version"].as<int>()!=1) throw std::runtime_error("unsupported schema_version");
  auto a=root["camera"], b=root["imu"], t=root["transport"], r=root["runtime"], s=root["ros"];
  keys(a,{"width","height","fps","bitrate_kbps"},"camera");
  keys(b,{"frequency_hz","accel_range_g","gyro_range_dps"},"imu");
  keys(t,{"host","rtsp_base_port","rtsp_path","imu_bind_address","imu_port","control_port"},"transport",{"mode","usb_backend","usb_serial","usb_port"});
  keys(r,{"image_queue","imu_queue"},"runtime"); keys(s,{"topic_prefix","image_queue","imu_queue"},"ros");
  Config c;
  if(t["mode"])c.transport_mode=t["mode"].as<std::string>();
  if(t["usb_backend"])c.usb_backend=t["usb_backend"].as<std::string>();
  if(t["usb_serial"])c.usb_serial=t["usb_serial"].as<std::string>();
  if(t["usb_port"])c.usb_port=t["usb_port"].as<int>();
  c.width=a["width"].as<int>(); c.height=a["height"].as<int>(); c.fps=a["fps"].as<int>(); c.bitrate_kbps=a["bitrate_kbps"].as<int>();
  c.frequency_hz=b["frequency_hz"].as<double>(); c.accel_range_g=b["accel_range_g"].as<double>(); c.gyro_range_dps=b["gyro_range_dps"].as<double>();
  c.host=t["host"].as<std::string>(); c.rtsp_base_port=t["rtsp_base_port"].as<int>(); c.rtsp_path=t["rtsp_path"].as<std::string>();
  c.control_port=t["control_port"].as<int>();
  c.imu_bind_address=t["imu_bind_address"].as<std::string>(); c.imu_port=t["imu_port"].as<int>();
  c.image_queue=r["image_queue"].as<int>(); c.imu_queue=r["imu_queue"].as<int>();
  c.topic_prefix=s["topic_prefix"].as<std::string>(); c.ros_image_queue=s["image_queue"].as<int>(); c.ros_imu_queue=s["imu_queue"].as<int>();
  validate(c); return c;
}
std::string json_quote(const std::string& text) {
  std::ostringstream out; out<<'"';
  for(unsigned char c:text) { if(c=='"'||c=='\\') out<<'\\'<<c;
    else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<int(c)<<std::dec;
    else out<<c; }
  out<<'"'; return out.str();
}
}
