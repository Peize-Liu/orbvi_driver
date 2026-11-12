#include "orbvi_sdk/temperature_stream.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "orbvi_sdk/client.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk {
namespace {

constexpr std::size_t kOcpuPacketSize = 32U;

std::uint64_t read_be(const std::uint8_t* data, unsigned count) {
  std::uint64_t value = 0U;
  for (unsigned index = 0U; index < count; ++index) {
    value = (value << 8U) | data[index];
  }
  return value;
}

std::uint32_t crc32_iso_hdlc(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t index = 0U; index < size; ++index) {
    crc ^= data[index];
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xedb88320U : 0U);
    }
  }
  return ~crc;
}

float read_float_be(const std::uint8_t* data) {
  const std::uint32_t bits = static_cast<std::uint32_t>(read_be(data, 4U));
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

Result<TemperatureSample> parse_ocpu_packet(const std::uint8_t* data,
                                             std::size_t size) {
  if (data == nullptr || size != kOcpuPacketSize) {
    return Status{StatusCode::corrupt_data, "OCPU/1 length must be 32"};
  }
  if (std::memcmp(data, "OCPU", 4U) != 0 || data[4] != 1U ||
      data[5] != 1U || read_be(data + 6U, 2U) != 0U) {
    return Status{StatusCode::corrupt_data,
                  "OCPU/1 magic/version/type/reserved invalid"};
  }
  if (crc32_iso_hdlc(data, 28U) !=
      static_cast<std::uint32_t>(read_be(data + 28U, 4U))) {
    return Status{StatusCode::corrupt_data, "OCPU/1 CRC mismatch"};
  }

  TemperatureSample sample;
  sample.celsius = read_float_be(data + 24U);
  if (!std::isfinite(sample.celsius)) {
    return Status{StatusCode::corrupt_data, "OCPU/1 temperature is not finite"};
  }
  sample.sequence = read_be(data + 8U, 8U);
  sample.monotonic_ns = read_be(data + 16U, 8U);
  return sample;
}

namespace detail {
namespace {

class TemperatureStreamRuntime final : public Runtime {
 public:
  explicit TemperatureStreamRuntime(TemperatureStreamConfig config)
      : config_(std::move(config)) {}

  ~TemperatureStreamRuntime() override { static_cast<void>(close()); }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    return std::vector<DeviceInfo>{};
  }

  Status open(const DeviceSelector&) override {
    if (fd_ >= 0) {
      return Status{StatusCode::already_open,
                    "temperature stream already open"};
    }
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return Status{StatusCode::io_error,
                    "cannot create temperature UDP socket"};
    }
    sockaddr_in bind_to{};
    bind_to.sin_family = AF_INET;
    bind_to.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.bind_address.c_str(), &bind_to.sin_addr) !=
        1) {
      ::close(fd);
      return Status{StatusCode::invalid_argument,
                    "invalid temperature bind IPv4 address"};
    }
    if (bind(fd, reinterpret_cast<const sockaddr*>(&bind_to),
             sizeof(bind_to)) != 0) {
      const int error = errno;
      ::close(fd);
      return Status{StatusCode::io_error,
                    "temperature UDP bind failed: " + std::to_string(error)};
    }
    fd_ = fd;
    return Status{};
  }

  Status start() override {
    if (fd_ < 0) {
      return Status{StatusCode::not_open,
                    "temperature stream is not open"};
    }
    if (started_.load()) {
      return Status{StatusCode::busy,
                    "temperature stream already started"};
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    stop_requested_.store(false);
    samples_delivered_.store(0U);
    // Publish started before the worker can observe a datagram and exit. If
    // the thread starts after this store, its final state update wins.
    started_.store(true);
    try {
      worker_ = std::thread([this]() {
        try {
          receive_loop();
        } catch (...) {
          // A callback/allocator failure must not terminate the host process.
          try {
            report_error(callbacks_snapshot(), false,
                         "temperature receive worker threw an exception");
          } catch (...) {
          }
          started_.store(false);
        }
      });
    } catch (...) {
      started_.store(false);
      return Status{StatusCode::internal_error,
                    "cannot start temperature receive thread"};
    }
    return Status{};
  }

  Status stop() override {
    stop_requested_.store(true);
    if (worker_.joinable() &&
        worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
    }
    started_.store(false);
    return Status{};
  }

  Status close() noexcept override {
    Status status;
    try {
      status = stop();
    } catch (...) {
      status = Status{StatusCode::internal_error,
                      "temperature stop threw during close"};
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    return status;
  }

  void set_callbacks(RuntimeCallbacks callbacks) override {
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_ = std::move(callbacks);
  }

  [[nodiscard]] bool started() const noexcept override {
    return started_.load();
  }

 private:
  RuntimeCallbacks callbacks_snapshot() const {
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    return callbacks_;
  }

  void report_error(const RuntimeCallbacks& callbacks, bool crc,
                    std::string message) {
    if (callbacks.on_temperature_error) {
      callbacks.on_temperature_error(crc, std::move(message));
    }
  }

  void receive_loop() {
    const RuntimeCallbacks callbacks = callbacks_snapshot();
    bool have_sequence = false;
    std::uint64_t previous_sequence = 0U;
    std::uint32_t delivered = 0U;

    while (!stop_requested_.load()) {
      pollfd descriptor{fd_, POLLIN, 0};
      const int poll_result = poll(&descriptor, 1, 100);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        report_error(callbacks, false, "temperature UDP poll failed");
        break;
      }
      if (poll_result == 0) {
        continue;
      }
      if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
        report_error(callbacks, false, "temperature UDP socket error");
        break;
      }
      if ((descriptor.revents & POLLIN) == 0) {
        continue;
      }

      std::array<std::uint8_t, kOcpuPacketSize + 1U> packet{};
      const ssize_t count =
          recvfrom(fd_, packet.data(), packet.size(), 0, nullptr, nullptr);
      if (count < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        report_error(callbacks, false, "temperature UDP receive failed");
        continue;
      }

      const auto parsed = parse_ocpu_packet(
          packet.data(), static_cast<std::size_t>(count));
      if (!parsed.ok()) {
        const bool crc =
            parsed.status().message().find("CRC") != std::string::npos;
        report_error(callbacks, crc, parsed.status().message());
        continue;
      }

      const TemperatureSample& sample = parsed.value();
      if (have_sequence && sample.sequence <= previous_sequence) {
        report_error(callbacks, false,
                     "OCPU/1 sequence repeated or moved backwards");
        continue;
      }
      if (have_sequence && sample.sequence > previous_sequence + 1U) {
        const std::uint64_t gap =
            sample.sequence - previous_sequence - 1U;
        if (callbacks.on_temperature_sequence_gap) {
          callbacks.on_temperature_sequence_gap(gap);
        }
      }
      previous_sequence = sample.sequence;
      have_sequence = true;
      if (callbacks.on_temperature) {
        callbacks.on_temperature(sample);
      }
      ++delivered;
      samples_delivered_.store(delivered);
      if (config_.samples != 0U && delivered >= config_.samples) {
        break;
      }
    }

    started_.store(false);
  }

  TemperatureStreamConfig config_;
  mutable std::mutex callback_mutex_;
  RuntimeCallbacks callbacks_;
  int fd_{-1};
  std::thread worker_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint32_t> samples_delivered_{0U};
};

}  // namespace

Status validate_temperature_stream_config(
    const TemperatureStreamConfig& config) {
  in_addr address{};
  if (config.bind_address.empty() ||
      inet_pton(AF_INET, config.bind_address.c_str(), &address) != 1) {
    return Status{StatusCode::invalid_argument,
                  "TemperatureStreamConfig.bind_address must be IPv4"};
  }
  if (config.port == 0U) {
    return Status{StatusCode::invalid_argument,
                  "TemperatureStreamConfig.port must be non-zero"};
  }
  if (config.samples > 100000U) {
    return Status{StatusCode::invalid_argument,
                  "TemperatureStreamConfig.samples must be 0..100000"};
  }
  return Status{};
}

std::unique_ptr<Runtime> make_temperature_stream_runtime(
    const TemperatureStreamConfig& config) {
  return std::make_unique<TemperatureStreamRuntime>(config);
}

}  // namespace detail

Client make_temperature_stream_client(const TemperatureStreamConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::temperature_stream;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<TemperatureStreamConfig>(config);
  return Client{std::move(options)};
}

}  // namespace orbvi_sdk
