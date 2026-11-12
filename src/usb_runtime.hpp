#pragma once
#include "orbvi_driver/config.hpp"
#include <orbvi_sdk/frame.hpp>
#include <orbvi_sdk/imu.hpp>
#include <functional>
#include <memory>
namespace orbvi_driver {
struct UsbStats {uint64_t image_drops=0,imu_drops=0,mux_drops=0,mux_invalid=0;std::string error;};
class UsbRuntime {
 public:
  UsbRuntime(const Config&,std::function<void(const orbvi_sdk::FrameView&)>,std::function<void(const orbvi_sdk::ImuEvent&)>);
  ~UsbRuntime();
  void start();void stop() noexcept;UsbStats stats()const;
 private:
  struct Impl;std::unique_ptr<Impl> p_;
};
}
