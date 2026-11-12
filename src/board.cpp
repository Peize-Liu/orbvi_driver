#include "orbvi_driver/board.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace orbvi_driver {
static std::string read(const std::filesystem::path& p) {
  std::ifstream f(p); if(!f) throw std::runtime_error("cannot read "+p.string());
  return {std::istreambuf_iterator<char>(f),{}};
}
static void write(const std::filesystem::path& p,const std::string& text) {
  std::ofstream f(p); if(!f || !(f<<text) || !(f.flush())) throw std::runtime_error("cannot write "+p.string());
}
static bool near(double a,double b) { return std::abs(a-b)<=std::max(1e-9,std::abs(b)*1e-5); }
static std::vector<double> list(const std::filesystem::path& p,double fallback) {
  if(!std::filesystem::exists(p)) return {fallback};
  std::istringstream s(read(p)); std::vector<double> v; double x;
  while(s>>x) { if(!std::isfinite(x)||x<=0) throw std::runtime_error("invalid capability "+p.string()); v.push_back(x); }
  if(!s.eof()||v.empty()) throw std::runtime_error("unsupported capability syntax: "+p.string());
  return v;
}
std::vector<ImuCapability> probe_imu(const std::filesystem::path& root) {
  std::vector<ImuCapability> v;
  for(const auto& entry:std::filesystem::directory_iterator(root)) {
    if(!std::filesystem::exists(entry.path()/"name")) continue;
    auto name=read(entry.path()/"name"); name.erase(name.find_last_not_of("\r\n ")+1);
    if(name!="icm42688-gyro" && name!="icm42688-accel") continue;
    ImuCapability d; d.path=entry.path(); d.name=name;
    d.scale_file=name=="icm42688-gyro"?"in_anglvel_scale":"in_accel_scale";
    d.scale=std::stod(read(d.path/d.scale_file)); d.frequency=std::stod(read(d.path/"sampling_frequency"));
    d.scales=list(d.path/(d.scale_file+"_available"),d.scale);
    d.frequencies=list(d.path/"sampling_frequency_available",d.frequency); v.push_back(d);
  }
  return v;
}
static double target_scale(const Config& c,const ImuCapability& d) {
  return d.name=="icm42688-gyro"?c.gyro_range_dps*3.14159265358979323846/180/32768:c.accel_range_g*9.80665/32768;
}
static double supported(const std::vector<double>& values,double target,const std::string& what) {
  for(double x:values) if(near(x,target)) return x;
  throw std::runtime_error("BSP does not expose requested "+what+"; use orbvi_board probe for actual capabilities");
}
void check_board(const Config& c,const std::vector<ImuCapability>& devices) {
  validate(c); bool gyro=false,accel=false;
  for(const auto& d:devices) {
    if(d.name=="icm42688-gyro") { if(gyro) throw std::runtime_error("ambiguous gyro devices"); gyro=true; }
    else { if(accel) throw std::runtime_error("ambiguous accel devices"); accel=true; }
    supported(d.frequencies,c.frequency_hz,d.name+" frequency");
    supported(d.scales,target_scale(c,d),d.name+" range");
  }
  if(!gyro||!accel) throw std::runtime_error("ICM42688 gyro and accel devices are both required");
}
BoardSettings::BoardSettings(const Config& c,const std::vector<ImuCapability>& devices) {
  check_board(c,devices);
  for(const auto& d:devices) if(std::stod(read(d.path/"buffer/enable"))!=0) throw std::runtime_error(d.name+" buffer busy");
  try {
    for(const auto& d:devices) {
      for(const auto& setting:std::vector<std::pair<std::string,double>>{
          {"sampling_frequency",supported(d.frequencies,c.frequency_hz,d.name+" frequency")},
          {d.scale_file,supported(d.scales,target_scale(c,d),d.name+" range")}}) {
        auto path=d.path/setting.first; auto old=read(path);
        if(near(std::stod(old),setting.second)) continue;
        saved_.push_back({path,old}); std::ostringstream value; value<<std::setprecision(16)<<setting.second;
        write(path,value.str()); if(!near(std::stod(read(path)),setting.second)) throw std::runtime_error("readback mismatch: "+path.string());
      }
    }
  } catch(...) { restore(); throw; }
}
void BoardSettings::restore() noexcept {
  for(auto i=saved_.rbegin();i!=saved_.rend();++i) try { write(i->first,i->second); }
    catch(const std::exception& e) { std::cerr<<"IMU restore failed: "<<e.what()<<'\n'; }
  saved_.clear();
}
BoardSettings::~BoardSettings() { restore(); }
std::string capabilities_json(const std::vector<ImuCapability>& devices) {
  std::ostringstream s; s<<std::setprecision(12)<<"{\"schema_version\":1,\"camera\":{\"preset_sizes\":[";
  bool comma=false; for(auto size:camera_sizes()) { if(comma)s<<',';comma=true;s<<'['<<size.first<<','<<size.second<<']'; }
  s<<"],\"candidate_sync_fps\":[25,30,40,50,60],\"note\":\"presets, not proof of hardware validation; consult matrix report\"},\"imu\":[";
  comma=false; for(const auto& d:devices) { if(comma)s<<',';comma=true;
    s<<"{\"name\":"<<json_quote(d.name)<<",\"scale\":"<<d.scale<<",\"frequency_hz\":"<<d.frequency<<",\"available_frequencies\":[";
    bool sep=false; for(double x:d.frequencies) { if(sep)s<<',';sep=true;s<<x; }
    s<<"],\"available_ranges\":["; sep=false;
    for(double x:d.scales) { if(sep)s<<',';sep=true;s<<(d.name=="icm42688-gyro"?x*32768*180/3.14159265358979323846:x*32768/9.80665); }
    s<<"]}";
  } s<<"]}";return s.str();
}
}
