#pragma once
#include "config.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace orbvi_driver {
struct ImuCapability {
  std::filesystem::path path;
  std::string name, scale_file;
  double scale=0, frequency=0;
  std::vector<double> scales, frequencies;
};
std::vector<ImuCapability> probe_imu(const std::filesystem::path& root="/sys/bus/iio/devices");
void check_board(const Config&,const std::vector<ImuCapability>&);
std::string capabilities_json(const std::vector<ImuCapability>&);
class BoardSettings {
 public:
  BoardSettings(const Config&,const std::vector<ImuCapability>&);
  ~BoardSettings();
  BoardSettings(const BoardSettings&)=delete;
  BoardSettings& operator=(const BoardSettings&)=delete;
 private:
  std::vector<std::pair<std::filesystem::path,std::string>> saved_;
  void restore() noexcept;
};
}
