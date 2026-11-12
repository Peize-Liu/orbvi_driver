#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "orbvi_sdk/image_stream.hpp"
#include "orbvi_sdk/replay.hpp"
#include "orbvi_sdk/rtsp.hpp"
#include "orbvi_sdk/temperature_stream.hpp"
#include "orbvi_sdk/udp_imu.hpp"
#include "orbvi_sdk/device.hpp"
#include "orbvi_sdk/status.hpp"

namespace orbvi_sdk::detail {

struct RtspOutputConfig final : RuntimeConfig {
  RtspConfig source;
  PixelFormat output_format{PixelFormat::gray8};
};

struct RuntimeCallbacks {
  FrameCallback on_frame;
  CompressedFrameCallback on_compressed_frame;
  TemperatureCallback on_temperature;
  std::function<void(std::uint32_t, bool, std::string)> on_image_error;
  std::function<void(std::uint32_t, std::uint64_t)> on_image_sequence_gap;
  std::function<void(bool, std::string)> on_temperature_error;
  std::function<void(std::uint64_t)> on_temperature_sequence_gap;
  ImuCallback on_imu;
  SyncCallback on_sync;
  DiagnosticsCallback on_diagnostics;
  std::function<void(bool)> on_imu_packet_error;
  std::function<void(std::uint64_t)> on_imu_sequence_gap;
};

class Runtime {
 public:
  virtual ~Runtime() = default;

  virtual Result<std::vector<DeviceInfo>> enumerate() const = 0;
  virtual Status open(const DeviceSelector& selector) = 0;
  virtual Status start() = 0;
  virtual Status stop() = 0;
  virtual Status close() noexcept = 0;
  virtual void set_callbacks(RuntimeCallbacks callbacks) = 0;
  [[nodiscard]] virtual bool started() const noexcept = 0;
};

Status validate_image_stream_config(const ImageStreamConfig& config);
std::unique_ptr<Runtime> make_image_stream_runtime(const ImageStreamConfig& config);
Status validate_temperature_stream_config(const TemperatureStreamConfig& config);
std::unique_ptr<Runtime> make_temperature_stream_runtime(const TemperatureStreamConfig& config);
std::unique_ptr<Runtime> make_unsupported_runtime();
std::unique_ptr<Runtime> make_fake_runtime(const FakeConfig& config);
std::unique_ptr<Runtime> make_replay_runtime(const ReplayConfig& config);
Status validate_rtsp_config(const RtspConfig& config);
std::unique_ptr<Runtime> make_rtsp_runtime(const RtspConfig& config,
                                           PixelFormat output_format);
Status validate_udp_imu_config(const UdpImuConfig& config);
std::unique_ptr<Runtime> make_udp_imu_runtime(const UdpImuConfig& config);

}  // namespace orbvi_sdk::detail
