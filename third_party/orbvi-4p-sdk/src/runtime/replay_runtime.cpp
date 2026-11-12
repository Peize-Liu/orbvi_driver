#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/replay.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk::detail {
namespace {

bool parse_u64(const std::string& text, std::uint64_t& value) {
  try {
    std::size_t consumed = 0U;
    const auto parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) {
      return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_u32(const std::string& text, std::uint32_t& value) {
  std::uint64_t parsed = 0U;
  if (!parse_u64(text, parsed) || parsed > 0xffffffffULL) {
    return false;
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_i64(const std::string& text, std::int64_t& value) {
  try {
    std::size_t consumed = 0U;
    const auto parsed = std::stoll(text, &consumed, 0);
    if (consumed != text.size()) {
      return false;
    }
    value = static_cast<std::int64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_double(const std::string& text, double& value) {
  try {
    std::size_t consumed = 0U;
    value = std::stod(text, &consumed);
    return consumed == text.size();
  } catch (...) {
    return false;
  }
}

std::vector<std::string> split_tokens(const std::string& line) {
  std::istringstream stream(line);
  std::vector<std::string> result;
  std::string token;
  while (stream >> token) {
    result.push_back(token);
  }
  return result;
}

std::map<std::string, std::string> parse_fields(
    const std::vector<std::string>& tokens) {
  std::map<std::string, std::string> result;
  for (std::size_t index = 0U; index < tokens.size(); ++index) {
    const std::size_t separator = tokens[index].find('=');
    if (separator == std::string::npos) {
      continue;
    }
    result[tokens[index].substr(0U, separator)] =
        tokens[index].substr(separator + 1U);
  }
  return result;
}

std::string field_or(const std::map<std::string, std::string>& fields,
                     const std::string& name, const std::string& fallback = {}) {
  const auto it = fields.find(name);
  return it == fields.end() ? fallback : it->second;
}

bool parse_hex_bytes(const std::string& text, std::vector<std::uint8_t>& bytes) {
  if (text.size() % 2U != 0U) {
    return false;
  }
  bytes.clear();
  bytes.reserve(text.size() / 2U);
  for (std::size_t index = 0U; index < text.size(); index += 2U) {
    const auto hex_value = [](char value) -> int {
      if (value >= '0' && value <= '9') {
        return value - '0';
      }
      if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
      }
      if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
      }
      return -1;
    };
    const int high = hex_value(text[index]);
    const int low = hex_value(text[index + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

struct ReplayEvent {
  enum class Kind { frame, imu, sync };
  Kind kind{Kind::frame};
  FrameView frame;
  std::vector<std::uint8_t> frame_data;
  ImuEvent imu;
  FrameSync sync;
};

DeviceInfo replay_device() {
  DeviceInfo device;
  device.id = "replay-0";
  device.serial = "REPLAY0001";
  device.product_id = kProductId4pX5;
  device.firmware_version = "replay-v1";
  device.fake = true;
  device.capabilities = {
      DeviceCapability::imu,
      DeviceCapability::frame_sync,
      DeviceCapability::timing_diagnostics,
  };
  return device;
}

}  // namespace

class ReplayRuntime final : public Runtime {
 public:
  explicit ReplayRuntime(ReplayConfig config) : config_(std::move(config)) {}

  ~ReplayRuntime() override {
    stop();
    close();
  }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    return std::vector<DeviceInfo>{replay_device()};
  }

  Status open(const DeviceSelector&) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
      return Status{};
    }
    if (config_.path.empty()) {
      return Status{StatusCode::invalid_argument, "replay path is empty"};
    }

    std::ifstream input(config_.path);
    if (!input) {
      return Status{StatusCode::io_error, "cannot open replay fixture: " + config_.path};
    }

    std::vector<ReplayEvent> parsed;
    std::string line;
    bool version_seen = false;
    bool contract_seen = false;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
      ++line_number;
      const std::size_t first = line.find_first_not_of(" \t\r\n");
      if (first == std::string::npos || line[first] == '#') {
        continue;
      }
      const std::vector<std::string> tokens = split_tokens(line);
      if (tokens.empty()) {
        continue;
      }
      if (!version_seen) {
        if (tokens[0] != kDefaultReplayFormat) {
          return Status{StatusCode::corrupt_data,
                        "replay fixture has no " + std::string{kDefaultReplayFormat} +
                            " header at line " + std::to_string(line_number)};
        }
        version_seen = true;
        continue;
      }

      const std::map<std::string, std::string> fields = parse_fields(tokens);
      if (!contract_seen) {
        if (field_or(fields, "product").empty()) {
          return Status{StatusCode::corrupt_data,
                        "replay fixture has no contract fingerprint at line " +
                            std::to_string(line_number)};
        }
        const std::string product = field_or(fields, "product");
        const std::string layout = field_or(fields, "layout");
        const std::string protocol = field_or(fields, "protocol");
        const std::size_t dot = protocol.find('.');
        std::uint32_t protocol_major = 0U;
        std::uint32_t protocol_minor = 0U;
        if (product != kProductId4pX5 ||
            layout != "grid_2x2_1280x1088_ports_v1" || dot == std::string::npos ||
            !parse_u32(protocol.substr(0U, dot), protocol_major) ||
            !parse_u32(protocol.substr(dot + 1U), protocol_minor) ||
            protocol_major != kDraftProtocolMajor ||
            protocol_minor != kDraftProtocolMinor) {
          return Status{StatusCode::corrupt_data,
                        "unsupported replay contract fingerprint at line " +
                            std::to_string(line_number)};
        }
        contract_seen = true;
        continue;
      }

      if (tokens[0] == "frame") {
        ReplayEvent event;
        event.kind = ReplayEvent::Kind::frame;
        std::uint32_t camera_index = 0U;
        if (!parse_u32(field_or(fields, "camera", "0"), camera_index)) {
          return Status{StatusCode::corrupt_data,
                        "invalid camera field at line " + std::to_string(line_number)};
        }
        event.frame.camera_index = camera_index;
        if (!parse_u64(field_or(fields, "sequence", "0"), event.frame.sequence) ||
            !parse_u64(field_or(fields, "group_id", "0"), event.frame.group_id) ||
            !parse_i64(field_or(fields, "timestamp_ns", "0"),
                       event.frame.timestamp_ns) ||
            !parse_i64(field_or(fields, "sensor_timestamp_ns", "0"),
                       event.frame.sensor_timestamp_ns) ||
            !parse_u32(field_or(fields, "generation", "0"),
                       event.frame.generation) ||
            !parse_u32(field_or(fields, "width", "0"), event.frame.width) ||
            !parse_u32(field_or(fields, "height", "0"), event.frame.height) ||
            !parse_u32(field_or(fields, "stride", "0"), event.frame.stride)) {
          return Status{StatusCode::corrupt_data,
                        "invalid frame field at line " + std::to_string(line_number)};
        }
        const std::string format = field_or(fields, "format", "unknown");
        if (format == "mjpeg") {
          event.frame.pixel_format = PixelFormat::mjpeg;
        } else if (format == "gray8") {
          event.frame.pixel_format = PixelFormat::gray8;
        } else if (format == "yuyv") {
          event.frame.pixel_format = PixelFormat::yuyv;
        } else if (format == "nv12") {
          event.frame.pixel_format = PixelFormat::nv12;
        }
        event.frame.layout = FrameLayout::single_plane;
        event.frame.contract = ContractFingerprint{};
        event.frame.crc_valid = field_or(fields, "crc", "1") != "0";
        if (!parse_hex_bytes(field_or(fields, "data", ""), event.frame_data)) {
          return Status{StatusCode::corrupt_data,
                        "invalid frame data at line " + std::to_string(line_number)};
        }
        parsed.push_back(std::move(event));
      } else if (tokens[0] == "imu") {
        ReplayEvent event;
        event.kind = ReplayEvent::Kind::imu;
        if (!parse_u64(field_or(fields, "sequence", "0"),
                       event.imu.sample.sequence) ||
            !parse_i64(field_or(fields, "timestamp_ns", "0"),
                       event.imu.sample.timestamp_ns) ||
            !parse_i64(field_or(fields, "sensor_timestamp_ns", "0"),
                       event.imu.sample.sensor_timestamp_ns) ||
            !parse_u32(field_or(fields, "generation", "0"),
                       event.imu.sample.generation)) {
          return Status{StatusCode::corrupt_data,
                        "invalid imu field at line " + std::to_string(line_number)};
        }
        if (!parse_double(field_or(fields, "accel_x", "0"),
                          event.imu.sample.accel_x_mps2) ||
            !parse_double(field_or(fields, "accel_y", "0"),
                          event.imu.sample.accel_y_mps2) ||
            !parse_double(field_or(fields, "accel_z", "0"),
                          event.imu.sample.accel_z_mps2) ||
            !parse_double(field_or(fields, "gyro_x", "0"),
                          event.imu.sample.gyro_x_radps) ||
            !parse_double(field_or(fields, "gyro_y", "0"),
                          event.imu.sample.gyro_y_radps) ||
            !parse_double(field_or(fields, "gyro_z", "0"),
                          event.imu.sample.gyro_z_radps)) {
          return Status{StatusCode::corrupt_data,
                        "invalid imu sample field at line " +
                            std::to_string(line_number)};
        }
        event.imu.sample.contract = ContractFingerprint{};
        event.imu.sample.crc_valid = field_or(fields, "crc", "1") != "0";
        parsed.push_back(std::move(event));
      } else if (tokens[0] == "sync") {
        ReplayEvent event;
        event.kind = ReplayEvent::Kind::sync;
        if (!parse_u64(field_or(fields, "sequence", "0"), event.sync.sequence) ||
            !parse_u64(field_or(fields, "group_id", "0"), event.sync.group_id) ||
            !parse_i64(field_or(fields, "timestamp_ns", "0"),
                       event.sync.host_timestamp_ns) ||
            !parse_i64(field_or(fields, "sensor_timestamp_ns", "0"),
                       event.sync.sensor_timestamp_ns) ||
            !parse_u32(field_or(fields, "mask", "0f"), event.sync.mask)) {
          return Status{StatusCode::corrupt_data,
                        "invalid sync field at line " + std::to_string(line_number)};
        }
        event.sync.contract = ContractFingerprint{};
        event.sync.crc_valid = field_or(fields, "crc", "1") != "0";
        parsed.push_back(std::move(event));
      }
    }

    if (!version_seen) {
      return Status{StatusCode::corrupt_data, "empty replay fixture"};
    }
    if (!contract_seen) {
      return Status{StatusCode::corrupt_data,
                    "replay fixture has no contract fingerprint"};
    }
    if (parsed.empty()) {
      return Status{StatusCode::corrupt_data, "replay fixture has no events"};
    }

    events_ = std::move(parsed);
    opened_ = true;
    return Status{};
  }

  Status start() override {
    reap_finished_worker();

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
    }
    worker_ = std::thread([this]() { replay_loop(); });
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
                      "replay runtime stop threw during close"};
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

  void replay_loop() noexcept {
    try {
      std::int64_t previous_timestamp = 0;
      for (std::size_t index = 0U; index < events_.size(); ++index) {
        if (stop_requested_.load()) {
          return;
        }
        ReplayEvent& event = events_[index];
        const RuntimeCallbacks callbacks = callbacks_snapshot();

        if (config_.mode == ReplayMode::preserve_timestamp_delta &&
            index > 0U) {
          const std::int64_t timestamp =
              event.kind == ReplayEvent::Kind::frame
                  ? event.frame.timestamp_ns
                  : (event.kind == ReplayEvent::Kind::imu
                         ? event.imu.sample.timestamp_ns
                         : event.sync.host_timestamp_ns);
          const std::int64_t delta =
              std::max<std::int64_t>(timestamp - previous_timestamp, 0);
          if (delta > 0) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (cv_.wait_for(lock, std::chrono::nanoseconds(delta),
                             [this]() { return stop_requested_.load(); })) {
              return;
            }
          }
          previous_timestamp = timestamp;
        } else if (config_.speed_limit_us > 0U) {
          std::unique_lock<std::mutex> lock(mutex_);
          if (cv_.wait_for(
                  lock, std::chrono::microseconds(config_.speed_limit_us),
                  [this]() { return stop_requested_.load(); })) {
            return;
          }
        }

        if (stop_requested_.load()) {
          return;
        }

        if (event.kind == ReplayEvent::Kind::frame && callbacks.on_frame) {
          event.frame.data =
              event.frame_data.empty() ? nullptr : event.frame_data.data();
          event.frame.size = event.frame_data.size();
          callbacks.on_frame(event.frame);
        } else if (event.kind == ReplayEvent::Kind::imu && callbacks.on_imu) {
          callbacks.on_imu(event.imu);
        } else if (event.kind == ReplayEvent::Kind::sync && callbacks.on_sync) {
          callbacks.on_sync(event.sync);
        }
      }

      if (config_.stop_at_end) {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
      }
    } catch (...) {
      const std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
  }

  ReplayConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  RuntimeCallbacks callbacks_;
  std::vector<ReplayEvent> events_;
  std::thread worker_;
  bool opened_{false};
  bool running_{false};
  std::atomic<bool> stop_requested_{false};
};

std::unique_ptr<Runtime> make_replay_runtime(const ReplayConfig& config) {
  return std::make_unique<ReplayRuntime>(config);
}

}  // namespace orbvi_sdk::detail

namespace orbvi_sdk {

Client make_replay_client(const ReplayConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::replay;
  options.transport_mode = TransportMode::replay;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<ReplayConfig>(config);
  return Client{std::move(options)};
}

}  // namespace orbvi_sdk