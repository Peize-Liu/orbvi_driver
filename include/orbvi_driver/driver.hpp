#pragma once
#include "config.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace orbvi_driver {
struct Image {
  unsigned camera=0,width=0,height=0;
  uint64_t sequence=0;
  int64_t timestamp_ns=0;
  std::vector<uint8_t> rgb;
};
struct Imu {
  uint64_t sequence=0;
  int64_t timestamp_ns=0,accel_timestamp_ns=0,gyro_timestamp_ns=0;
  std::array<double,3> acceleration{},angular_velocity{};
};
struct StreamStats {
  uint64_t count=0,gaps=0,discontinuities=0,invalid=0,last_sequence=0;
  uint64_t duplicates_filtered=0;
  int64_t last_timestamp_ns=0;
};
struct Stats {
  std::array<StreamStats,4> cameras;
  StreamStats imu;
  uint64_t image_callback_drops=0,imu_callback_drops=0,callback_failures=0;
  uint64_t sync_groups=0,max_camera_skew_ns=0,imu_pairing_errors=0;
  uint64_t sync_skew_errors=0,last_camera_skew_ns=0;
  uint64_t sync_unmatched_frames=0;
  std::string error;
};
struct Callbacks {
  // Image pixels are owned and may be moved into an application queue/message.
  // Callbacks run on SDK workers; keep them short. Do not call stop() inside one.
  std::function<void(Image)> image;
  std::function<void(Imu)> imu;
};
class Driver {
 public:
  explicit Driver(Config config);
  ~Driver();
  Driver(const Driver&)=delete;
  Driver& operator=(const Driver&)=delete;
  void start(Callbacks callbacks={});
  void stop() noexcept;
  Stats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Reports a measured interval, excluding startup via the baseline snapshot.
// camera rate tolerance: 2%; IMU: 5%; gaps/invalid/callback drops must be zero.
bool capture_report(const Config&,const Stats& baseline,const Stats& end,double wall_seconds,std::string& json);
}
