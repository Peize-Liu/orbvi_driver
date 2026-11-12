#include "orbvi_sdk/fake.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "orbvi_sdk/client.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk::detail {
namespace {

DeviceInfo make_device(std::size_t index) {
  DeviceInfo device;
  device.id = "fake-" + std::to_string(index);
  device.serial = "FAKE" + std::to_string(1000U + index);
  device.product_id = kProductId4pX5;
  device.firmware_version = "fake-1";
  device.vendor_id = 0x2207U;
  device.product_id_number = 0x0001U;
  device.bus_number = static_cast<std::uint8_t>(index + 1U);
  device.device_address = static_cast<std::uint8_t>(index + 1U);
  device.fake = true;
  device.capabilities = {
      DeviceCapability::imu,
      DeviceCapability::frame_sync,
      DeviceCapability::timing_diagnostics,
      DeviceCapability::control,
  };
  return device;
}

bool selector_matches(const DeviceSelector& selector, const DeviceInfo& device,
                      std::size_t index) {
  if (selector.select_any) {
    return true;
  }
  if (!selector.serial.empty() && selector.serial != device.serial) {
    return false;
  }
  if (!selector.product_id.empty() && selector.product_id != device.product_id) {
    return false;
  }
  return selector.index == index || !selector.serial.empty();
}

}  // namespace

class FakeRuntime final : public Runtime {
 public:
  explicit FakeRuntime(FakeConfig config) : config_(std::move(config)) {
    if (config_.devices.empty()) {
      config_.devices = make_default_fake_config().devices;
    }
  }

  ~FakeRuntime() override {
    static_cast<void>(stop());
    static_cast<void>(close());
  }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    return config_.devices;
  }

  Status open(const DeviceSelector& selector) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
      return Status{};
    }
    for (std::size_t index = 0U; index < config_.devices.size(); ++index) {
      if (selector_matches(selector, config_.devices[index], index)) {
        selected_ = config_.devices[index];
        opened_ = true;
        return Status{};
      }
    }
    return Status{StatusCode::not_found, "no fake device matches selector"};
  }

  Status start() override {
    reap_finished_worker();

    bool auto_emit = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!opened_) {
        return Status{StatusCode::not_open,
                      "open() must succeed before start()"};
      }
      if (running_) {
        return Status{};
      }
      running_ = true;
      stop_requested_ = false;
      auto_emit = config_.auto_emit;
    }

    if (auto_emit) {
      worker_ = std::thread([this]() { emit_loop(); });
    }
    return Status{};
  }

  Status stop() override {
    bool join_worker = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && !worker_.joinable()) {
        return Status{};
      }
      stop_requested_ = true;
      running_ = false;
      join_worker =
          worker_.joinable() && worker_.get_id() != std::this_thread::get_id();
    }

    cv_.notify_all();
    if (join_worker) {
      worker_.join();
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    return Status{};
  }

  Status close() noexcept override {
    Status status;
    try {
      status = stop();
    } catch (...) {
      status = Status{StatusCode::internal_error,
                      "fake runtime stop threw during close"};
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    opened_ = false;
    return status;
  }

  void set_callbacks(RuntimeCallbacks callbacks) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    callbacks_ = std::move(callbacks);
  }

  [[nodiscard]] bool started() const noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

 private:
  RuntimeCallbacks callbacks_snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
  }

  void reap_finished_worker() {
    std::thread finished;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && worker_.joinable() &&
          worker_.get_id() != std::this_thread::get_id()) {
        finished = std::move(worker_);
      }
    }
    if (finished.joinable()) {
      finished.join();
    }
  }

  void emit_loop() noexcept {
    try {
      const std::uint64_t frame_count = config_.frames_per_camera;
      const std::uint64_t imu_count = config_.imu_samples;
      const std::uint64_t total_count = std::max(frame_count, imu_count);

      for (std::uint64_t sequence = 1U; sequence <= total_count; ++sequence) {
        if (stop_requested_.load()) {
          break;
        }

        if (sequence <= frame_count) {
          for (std::size_t camera = 0U; camera < config_.devices.size();
               ++camera) {
            if (stop_requested_.load()) {
              break;
            }
            const bool injected =
                camera == static_cast<std::size_t>(config_.fault_camera) &&
                sequence >= config_.fault_after_frames;
            if (injected && config_.fault == FakeFault::missing_sequence) {
              continue;
            }

            std::uint64_t emitted_sequence = sequence;
            std::int64_t timestamp =
                static_cast<std::int64_t>(sequence) * 1'000'000LL +
                static_cast<std::int64_t>(camera);
            std::uint32_t generation = 0U;
            bool crc_valid = true;
            std::size_t payload_size = static_cast<std::size_t>(
                config_.frame_width) * static_cast<std::size_t>(
                config_.frame_height);
            if (payload_size == 0U) {
              payload_size = 1U;
            }

            if (injected && config_.fault == FakeFault::out_of_order) {
              emitted_sequence = sequence > 2U ? sequence - 2U : 1U;
            } else if (injected && config_.fault == FakeFault::crc_error) {
              crc_valid = false;
            } else if (injected && config_.fault == FakeFault::truncated_jpeg) {
              payload_size = std::max<std::size_t>(payload_size / 8U, 1U);
            } else if (injected && config_.fault == FakeFault::time_rollback) {
              timestamp = -1;
            } else if (injected &&
                       config_.fault == FakeFault::generation_change) {
              generation = 1U;
            } else if (injected &&
                       config_.fault == FakeFault::disconnect_reconnect) {
              generation = 1U;
            }

            const RuntimeCallbacks callbacks = callbacks_snapshot();
            if (!callbacks.on_frame) {
              continue;
            }

            std::vector<std::uint8_t> payload(
                payload_size, static_cast<std::uint8_t>(camera + 1U));
            FrameView view;
            view.data = payload.data();
            view.size = payload.size();
            view.camera_index = static_cast<std::uint32_t>(camera);
            view.sequence = emitted_sequence;
            view.group_id = sequence;
            view.timestamp_ns = timestamp;
            view.sensor_timestamp_ns = timestamp;
            view.generation = generation;
            view.width = config_.frame_width;
            view.height = config_.frame_height;
            view.stride = config_.frame_stride;
            view.pixel_format = PixelFormat::gray8;
            view.layout = FrameLayout::single_plane;
            view.contract = ContractFingerprint{};
            view.crc_valid = crc_valid;
            callbacks.on_frame(view);
          }
        }

        const RuntimeCallbacks callbacks = callbacks_snapshot();
        if (sequence <= frame_count && config_.emit_frame_sync &&
            callbacks.on_sync) {
          FrameSync sync;
          sync.sequence = sequence;
          sync.group_id = sequence;
          sync.host_timestamp_ns =
              static_cast<std::int64_t>(sequence) * 1'000'000LL;
          sync.sensor_timestamp_ns = sync.host_timestamp_ns;
          sync.mask = 0x0fU;
          sync.contract = ContractFingerprint{};
          callbacks.on_sync(sync);
        }

        if (sequence <= imu_count && callbacks.on_imu) {
          ImuEvent event;
          event.sample.sequence = sequence;
          event.sample.timestamp_ns =
              static_cast<std::int64_t>(sequence) * 1'000'000LL;
          event.sample.sensor_timestamp_ns = event.sample.timestamp_ns;
          event.sample.accel_z_mps2 = 9.80665;
          event.sample.gyro_x_radps = 0.01;
          event.sample.contract = ContractFingerprint{};
          if (config_.emit_timing_health) {
            event.timing.sample_count = sequence;
          }
          callbacks.on_imu(event);
        }

        if (config_.fault == FakeFault::disconnect_reconnect &&
            sequence <= frame_count &&
            sequence >= config_.fault_after_frames &&
            callbacks.on_diagnostics) {
          DiagnosticsSnapshot diagnostics;
          diagnostics.state = ClientState::started;
          diagnostics.reconnects = 1U;
          diagnostics.last_error = "fake disconnect/reconnect";
          callbacks.on_diagnostics(diagnostics);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::microseconds(config_.emit_interval_us),
                     [this]() { return stop_requested_.load(); });
        if (stop_requested_.load()) {
          break;
        }
      }
    } catch (...) {
      // FakeRuntime is a test double; never let an exception escape its worker.
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }

  FakeConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  RuntimeCallbacks callbacks_;
  DeviceInfo selected_;
  std::thread worker_;
  bool opened_{false};
  bool running_{false};
  std::atomic<bool> stop_requested_{false};
};

std::unique_ptr<Runtime> make_fake_runtime(const FakeConfig& config) {
  return std::make_unique<FakeRuntime>(config);
}

FakeConfig make_default_fake_config(std::uint32_t device_count) {
  FakeConfig config;
  for (std::uint32_t index = 0U; index < device_count; ++index) {
    config.devices.push_back(make_device(static_cast<std::size_t>(index)));
  }
  return config;
}

}  // namespace orbvi_sdk::detail

namespace orbvi_sdk {

Client make_fake_client(const FakeConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::fake;
  options.transport_mode = TransportMode::fake;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<FakeConfig>(config);
  return Client{std::move(options)};
}

FakeConfig make_default_fake_config(std::uint32_t device_count) {
  return detail::make_default_fake_config(device_count);
}

}  // namespace orbvi_sdk
