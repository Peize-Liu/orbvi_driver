#include "orbvi_sdk/udp_imu.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "orbvi_sdk/client.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk {
namespace {
std::uint64_t read_be(const std::uint8_t* p, unsigned count) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i) value = (value << 8U) | p[i];
  return value;
}
std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8U; ++bit)
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
  }
  return ~crc;
}
float read_float(const std::uint8_t* p) {
  const std::uint32_t bits = static_cast<std::uint32_t>(read_be(p, 4U));
  float value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
}  // namespace

Result<ImuSample> parse_oimu_packet(const std::uint8_t* data, std::size_t size,
                                    std::int64_t host_receive_ns) {
  if (data == nullptr || size != 72U) {
    return Status{StatusCode::corrupt_data, "OIMU/1 length must be 72"};
  }
  if (std::memcmp(data, "OIMU", 4U) != 0 || data[4] != 1U ||
      data[5] != 1U || read_be(data + 6, 2U) != 72U ||
      read_be(data + 40, 4U) != 0U) {
    return Status{StatusCode::corrupt_data, "OIMU/1 header or flags invalid"};
  }
  if (crc32(data, 68U) != read_be(data + 68, 4U)) {
    return Status{StatusCode::corrupt_data, "OIMU/1 CRC mismatch"};
  }
  const auto accel_ns = read_be(data + 16, 8U);
  const auto gyro_ns = read_be(data + 24, 8U);
  if (accel_ns == 0U || gyro_ns == 0U ||
      accel_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      gyro_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      host_receive_ns <= 0) {
    return Status{StatusCode::corrupt_data, "OIMU/1 timestamps invalid"};
  }
  std::array<float, 6U> values{};
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = read_float(data + 44U + i * 4U);
    if (!std::isfinite(values[i])) {
      return Status{StatusCode::corrupt_data, "OIMU/1 non-finite sample"};
    }
  }
  ImuSample sample;
  sample.sequence = read_be(data + 8, 8U);
  sample.timestamp_ns = host_receive_ns;
  sample.sensor_timestamp_ns = static_cast<std::int64_t>(std::min(accel_ns, gyro_ns));
  sample.accel_timestamp_ns = static_cast<std::int64_t>(accel_ns);
  sample.gyro_timestamp_ns = static_cast<std::int64_t>(gyro_ns);
  sample.board_send_timestamp_ns = read_be(data + 32, 8U);
  sample.accel_x_mps2 = values[0]; sample.accel_y_mps2 = values[1];
  sample.accel_z_mps2 = values[2]; sample.gyro_x_radps = values[3];
  sample.gyro_y_radps = values[4]; sample.gyro_z_radps = values[5];
  return sample;
}

namespace detail {
Status validate_udp_imu_config(const UdpImuConfig& config) {
  in_addr address{};
  if (config.port == 0U || inet_pton(AF_INET, config.bind_address.c_str(), &address) != 1 ||
      (!config.source_address.empty() &&
       inet_pton(AF_INET, config.source_address.c_str(), &address) != 1)) {
    return Status{StatusCode::invalid_argument, "invalid UDP IMU IPv4 address or port"};
  }
  return Status{};
}

class UdpImuRuntime final : public Runtime {
 public:
  explicit UdpImuRuntime(UdpImuConfig config) : config_(std::move(config)) {}
  ~UdpImuRuntime() override { static_cast<void>(close()); }
  Result<std::vector<DeviceInfo>> enumerate() const override { return std::vector<DeviceInfo>{}; }
  Status open(const DeviceSelector&) override {
    if (fd_ >= 0) return Status{StatusCode::already_open, "UDP IMU already open"};
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return Status{StatusCode::io_error, "UDP IMU socket failed"};
    sockaddr_in bind_to{};
    bind_to.sin_family = AF_INET;
    bind_to.sin_port = htons(config_.port);
    static_cast<void>(inet_pton(AF_INET, config_.bind_address.c_str(), &bind_to.sin_addr));
    if (bind(fd, reinterpret_cast<const sockaddr*>(&bind_to), sizeof(bind_to)) != 0) {
      const int error = errno;
      ::close(fd);
      return Status{StatusCode::io_error, "UDP IMU bind failed: " + std::to_string(error)};
    }
    fd_ = fd;
    return Status{};
  }
  Status start() override {
    if (fd_ < 0) return Status{StatusCode::not_open, "UDP IMU not open"};
    if (started_) return Status{StatusCode::busy, "UDP IMU already started"};
    stop_requested_ = false;
    try { worker_ = std::thread([this] { receive_loop(); }); }
    catch (...) { return Status{StatusCode::internal_error, "UDP IMU thread failed"}; }
    started_ = true;
    return Status{};
  }
  Status stop() override {
    stop_requested_ = true;
    if (worker_.joinable()) worker_.join();
    started_ = false;
    return Status{};
  }
  Status close() noexcept override {
    static_cast<void>(stop());
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    return Status{};
  }
  void set_callbacks(RuntimeCallbacks callbacks) override { callbacks_ = std::move(callbacks); }
  bool started() const noexcept override { return started_; }
 private:
  void receive_loop() {
    in_addr source{};
    const bool filter = !config_.source_address.empty();
    if (filter) static_cast<void>(inet_pton(AF_INET, config_.source_address.c_str(), &source));
    bool have_sequence = false;
    std::uint64_t previous = 0;
    while (!stop_requested_) {
      pollfd pfd{fd_, POLLIN, 0};
      if (poll(&pfd, 1, 100) <= 0) continue;
      std::array<std::uint8_t, 73U> bytes{};
      sockaddr_in peer{};
      socklen_t peer_size = sizeof(peer);
      const ssize_t count = recvfrom(fd_, bytes.data(), bytes.size(), 0,
                                     reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (count < 0) continue;
      if (filter && peer.sin_addr.s_addr != source.s_addr) continue;
      const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      auto parsed = parse_oimu_packet(bytes.data(), static_cast<std::size_t>(count), now);
      if (!parsed.ok()) {
        if (callbacks_.on_imu_packet_error)
          callbacks_.on_imu_packet_error(parsed.status().message().find("CRC") != std::string::npos);
        continue;
      }
      const auto& sample = parsed.value();
      if (have_sequence && sample.sequence > previous &&
          sample.sequence - previous > 1U && callbacks_.on_imu_sequence_gap)
        callbacks_.on_imu_sequence_gap(sample.sequence - previous - 1U);
      // A lower/equal sequence indicates restart or reordering; reset baseline.
      previous = sample.sequence;
      have_sequence = true;
      if (callbacks_.on_imu) callbacks_.on_imu(ImuEvent{sample, {}});
    }
  }
  UdpImuConfig config_;
  RuntimeCallbacks callbacks_;
  int fd_{-1};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> started_{false};
  std::thread worker_;
};
std::unique_ptr<Runtime> make_udp_imu_runtime(const UdpImuConfig& config) {
  return std::make_unique<UdpImuRuntime>(config);
}
}  // namespace detail

Client make_udp_imu_client(const UdpImuConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::udp_imu;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<UdpImuConfig>(config);
  return Client{std::move(options)};
}
}  // namespace orbvi_sdk
