#include "orbvi_sdk/image_stream.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
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

constexpr std::size_t kStartSize = 8U;
constexpr std::size_t kHeaderSize = 48U;
constexpr std::uint32_t kMaxWidth = 4096U;
constexpr std::uint32_t kMaxHeight = 4096U;
constexpr std::size_t kMaxPayload = 32U * 1024U * 1024U;

struct Header {
  ImageStreamFormat format{ImageStreamFormat::nv12};
  std::uint8_t camera{0U};
  std::uint64_t sequence{0U};
  std::uint64_t sensor_timestamp_ns{0U};
  std::uint32_t width{0U};
  std::uint32_t height{0U};
  std::uint32_t stride{0U};
  std::uint32_t payload_size{0U};
  std::uint32_t crc{0U};
};

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

std::int64_t host_time_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool decode_header(const std::uint8_t* bytes, Header& header) {
  if (std::memcmp(bytes, "OIMG", 4U) != 0 || bytes[4] != 1U ||
      bytes[7] != 0U || read_be(bytes + 8U, 2U) != kHeaderSize ||
      read_be(bytes + 10U, 2U) != 0U) {
    return false;
  }
  const std::uint8_t kind = bytes[5];
  if (kind != static_cast<std::uint8_t>(ImageStreamFormat::nv12) &&
      kind != static_cast<std::uint8_t>(ImageStreamFormat::jpeg)) {
    return false;
  }
  header.format = static_cast<ImageStreamFormat>(kind);
  header.camera = bytes[6];
  header.sequence = read_be(bytes + 12U, 8U);
  header.sensor_timestamp_ns = read_be(bytes + 20U, 8U);
  header.width = static_cast<std::uint32_t>(read_be(bytes + 28U, 4U));
  header.height = static_cast<std::uint32_t>(read_be(bytes + 32U, 4U));
  header.stride = static_cast<std::uint32_t>(read_be(bytes + 36U, 4U));
  header.payload_size = static_cast<std::uint32_t>(read_be(bytes + 40U, 4U));
  header.crc = static_cast<std::uint32_t>(read_be(bytes + 44U, 4U));
  return true;
}

bool valid_header(const Header& header, const ImageStreamConfig& config,
                  std::uint32_t camera) {
  if (header.camera != static_cast<std::uint8_t>(camera) ||
      header.format != config.format || header.sequence == 0U ||
      header.sequence > config.frames_per_camera || header.width == 0U ||
      header.height == 0U || header.width > kMaxWidth ||
      header.height > kMaxHeight || header.payload_size == 0U ||
      header.payload_size > kMaxPayload ||
      header.sensor_timestamp_ns >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }

  if (header.format == ImageStreamFormat::jpeg) {
    return header.stride == 0U && header.payload_size >= 4U;
  }

  const std::uint64_t expected = static_cast<std::uint64_t>(header.width) *
                                 static_cast<std::uint64_t>(header.height) *
                                 3U / 2U;
  return (header.width & 1U) == 0U && (header.height & 1U) == 0U &&
         header.stride == header.width && expected == header.payload_size;
}

bool wait_fd(int fd, short events, std::chrono::steady_clock::time_point deadline,
             const std::atomic<bool>& stop_requested) {
  while (!stop_requested.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const int timeout = std::max(1, static_cast<int>(remaining.count()));
    pollfd descriptor{fd, events, 0};
    const int result = poll(&descriptor, 1, timeout);
    if (result > 0) {
      if ((descriptor.revents & events) != 0) {
        return true;
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return false;
      }
      continue;
    }
    if (result == 0) {
      return false;
    }
    if (errno != EINTR) {
      return false;
    }
  }
  return false;
}

bool read_exact(int fd, void* destination, std::size_t size,
                std::chrono::milliseconds timeout,
                const std::atomic<bool>& stop_requested) {
  auto* output = static_cast<std::uint8_t*>(destination);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (size > 0U && !stop_requested.load()) {
    if (!wait_fd(fd, POLLIN, deadline, stop_requested)) {
      return false;
    }
    const ssize_t count = recv(fd, output, size, 0);
    if (count > 0) {
      output += count;
      size -= static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return size == 0U;
}

bool write_exact(int fd, const void* source, std::size_t size,
                 std::chrono::milliseconds timeout,
                 const std::atomic<bool>& stop_requested) {
  const auto* input = static_cast<const std::uint8_t*>(source);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (size > 0U && !stop_requested.load()) {
    if (!wait_fd(fd, POLLOUT, deadline, stop_requested)) {
      return false;
    }
    const ssize_t count = send(fd, input, size, MSG_NOSIGNAL);
    if (count > 0) {
      input += count;
      size -= static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return size == 0U;
}

int connect_until(const ImageStreamConfig& config, std::uint32_t camera,
                  const std::atomic<bool>& stop_requested,
                  std::string& error) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port =
      htons(static_cast<std::uint16_t>(config.base_port + camera));
  if (inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1) {
    error = "host is not a valid IPv4 address";
    return -1;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + config.connect_timeout;
  int last_error = 0;
  while (!stop_requested.load() &&
         std::chrono::steady_clock::now() < deadline) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      last_error = errno;
      break;
    }
    timeval timeout{};
    timeout.tv_usec = 250000;
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                 sizeof(timeout)));
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0) {
      return fd;
    }
    last_error = errno;
    ::close(fd);
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto sleep_for =
        std::min(std::chrono::milliseconds{100},
                 std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
    std::this_thread::sleep_for(std::max(sleep_for, std::chrono::milliseconds{1}));
  }
  error = stop_requested.load() ? "connect cancelled"
                                : "connect failed: " + std::to_string(last_error);
  return -1;
}

bool valid_jpeg(const std::vector<std::uint8_t>& payload) {
  return payload.size() >= 4U && payload[0] == 0xffU && payload[1] == 0xd8U &&
         payload[payload.size() - 2U] == 0xffU && payload.back() == 0xd9U;
}

}  // namespace

Status validate_image_stream_config(const ImageStreamConfig& config) {
  in_addr address{};
  if (config.host.empty() ||
      inet_pton(AF_INET, config.host.c_str(), &address) != 1) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig.host must be an IPv4 address"};
  }
  if (config.base_port < 1024U || config.base_port > 65532U) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig.base_port must leave room for cameras 0..3"};
  }
  if (config.camera_mask == 0U || (config.camera_mask & 0xf0U) != 0U) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig.camera_mask must be a non-zero 4-bit mask"};
  }
  if (config.frames_per_camera == 0U || config.frames_per_camera > 100000U) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig.frames_per_camera must be in 1..100000"};
  }
  if (config.format != ImageStreamFormat::nv12 &&
      config.format != ImageStreamFormat::jpeg) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig.format is invalid"};
  }
  if (config.connect_timeout.count() <= 0 ||
      config.connect_timeout.count() > 600000 ||
      config.read_timeout.count() <= 0 ||
      config.read_timeout.count() > 600000) {
    return Status{StatusCode::invalid_argument,
                  "ImageStreamConfig timeouts must be in 1..600000 ms"};
  }
  return Status{};
}

class ImageStreamRuntime final : public Runtime {
 public:
  explicit ImageStreamRuntime(ImageStreamConfig config)
      : config_(std::move(config)) {}

  ~ImageStreamRuntime() override { static_cast<void>(close()); }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    return std::vector<DeviceInfo>{};
  }

  Status open(const DeviceSelector&) override {
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (opened_) {
      return Status{StatusCode::already_open, "image stream already open"};
    }
    opened_ = true;
    return Status{};
  }

  Status start() override {
    {
      const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (!opened_) {
        return Status{StatusCode::not_open,
                      "open() must succeed before start()"};
      }
      if (started_.load()) {
        return Status{StatusCode::busy, "image stream already started"};
      }
    }

    join_workers();
    stop_requested_.store(false);

    std::vector<std::pair<std::uint32_t, std::thread>> workers;
    std::uint32_t selected = 0U;
    for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
      if ((config_.camera_mask & (1U << camera)) != 0U) {
        ++selected;
      }
    }
    active_workers_.store(selected);
    {
      const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      started_.store(true);
    }

    try {
      for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
        if ((config_.camera_mask & (1U << camera)) == 0U) {
          continue;
        }
        workers.emplace_back(camera,
                             std::thread([this, camera]() { camera_worker(camera); }));
      }
    } catch (...) {
      stop_requested_.store(true);
      for (auto& worker : workers) {
        if (worker.second.joinable()) {
          worker.second.join();
        }
      }
      active_workers_.store(0U);
      started_.store(false);
      return Status{StatusCode::internal_error,
                    "cannot start image stream workers"};
    }

    {
      const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      for (auto& worker : workers) {
        workers_.push_back(std::move(worker.second));
      }
    }
    return Status{};
  }

  Status stop() override {
    stop_requested_.store(true);
    {
      const std::lock_guard<std::mutex> lock(socket_mutex_);
      for (const int fd : sockets_) {
        if (fd >= 0) {
          static_cast<void>(shutdown(fd, SHUT_RDWR));
        }
      }
    }
    join_workers();
    started_.store(false);
    return Status{};
  }

  Status close() noexcept override {
    Status status;
    try {
      status = stop();
    } catch (...) {
      status = Status{StatusCode::internal_error,
                      "image stream stop threw during close"};
    }
    const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    opened_ = false;
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

  void join_workers() {
    std::vector<std::thread> workers;
    {
      const std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      workers.swap(workers_);
    }
    for (auto& worker : workers) {
      if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
        worker.join();
      }
    }
  }

  void finish_worker() noexcept {
    if (active_workers_.fetch_sub(1U) == 1U) {
      started_.store(false);
    }
  }

  void set_socket(std::uint32_t camera, int fd) {
    const std::lock_guard<std::mutex> lock(socket_mutex_);
    sockets_[camera] = fd;
  }

  void clear_socket(std::uint32_t camera, int fd) {
    const std::lock_guard<std::mutex> lock(socket_mutex_);
    if (sockets_[camera] == fd) {
      sockets_[camera] = -1;
    }
  }

  void report_error(const RuntimeCallbacks& callbacks, std::uint32_t camera,
                    bool crc, std::string message) {
    if (callbacks.on_image_error) {
      callbacks.on_image_error(camera, crc, std::move(message));
    }
  }

  void report_loss(const RuntimeCallbacks& callbacks, std::uint32_t camera,
                   std::uint64_t count) {
    if (count > 0U && callbacks.on_image_sequence_gap) {
      callbacks.on_image_sequence_gap(camera, count);
    }
  }

  void camera_worker(std::uint32_t camera) noexcept {
    try {
      receive_camera(camera);
    } catch (...) {
      try {
        report_error(callbacks_snapshot(), camera, false,
                     "image stream worker threw an exception");
      } catch (...) {
      }
    }
    finish_worker();
  }

  void receive_camera(std::uint32_t camera) {
    const RuntimeCallbacks callbacks = callbacks_snapshot();
    std::string error;
    const int fd = connect_until(config_, camera, stop_requested_, error);
    if (fd < 0) {
      if (!stop_requested_.load()) {
        report_error(callbacks, camera, false, std::move(error));
        report_loss(callbacks, camera, config_.frames_per_camera - 1U);
      }
      return;
    }
    set_socket(camera, fd);
    if (stop_requested_.load()) {
      clear_socket(camera, fd);
      ::close(fd);
      return;
    }

    const std::array<std::uint8_t, kStartSize> start{
        {'O', 'C', 'T', 'L', 1U, 1U, static_cast<std::uint8_t>(camera), 0U}};
    if (!write_exact(fd, start.data(), start.size(), config_.read_timeout,
                     stop_requested_)) {
      clear_socket(camera, fd);
      ::close(fd);
      if (!stop_requested_.load()) {
        report_error(callbacks, camera, false, "OCTL START send failed");
        report_loss(callbacks, camera, config_.frames_per_camera - 1U);
      }
      return;
    }

    std::uint64_t next_sequence = 1U;
    std::uint32_t delivered = 0U;
    std::uint32_t accounted_drops = 0U;

    // N denotes the finite wire sequence range, not N valid callbacks. A bad
    // CRC must not keep us waiting for an (N+1)th frame that will never come.
    while (!stop_requested_.load() &&
           next_sequence <= config_.frames_per_camera) {
      std::array<std::uint8_t, kHeaderSize> bytes{};
      if (!read_exact(fd, bytes.data(), bytes.size(), config_.read_timeout,
                      stop_requested_)) {
        if (!stop_requested_.load() &&
            config_.frames_per_camera - delivered > accounted_drops) {
          ++accounted_drops;
          report_error(callbacks, camera, false,
                       "OIMG header missing, truncated or timed out");
        }
        break;
      }

      Header header;
      if (!decode_header(bytes.data(), header)) {
        ++accounted_drops;
        report_error(callbacks, camera, false,
                     "invalid OIMG header magic/version/kind/reserved");
        break;
      }
      if (!valid_header(header, config_, camera)) {
        ++accounted_drops;
        report_error(callbacks, camera, false, "invalid OIMG header fields");
        break;
      }

      std::vector<std::uint8_t> payload(header.payload_size);
      if (!read_exact(fd, payload.data(), payload.size(), config_.read_timeout,
                      stop_requested_)) {
        if (!stop_requested_.load()) {
          ++accounted_drops;
          report_error(callbacks, camera, false, "truncated OIMG payload");
        }
        break;
      }

      if (header.sequence < next_sequence) {
        ++accounted_drops;
        report_error(callbacks, camera, false,
                     "OIMG sequence repeated or moved backwards");
        continue;
      }
      if (header.sequence > next_sequence) {
        const std::uint64_t gap = header.sequence - next_sequence;
        accounted_drops += static_cast<std::uint32_t>(gap);
        report_loss(callbacks, camera, gap);
      }
      next_sequence = header.sequence + 1U;

      if (crc32_iso_hdlc(payload.data(), payload.size()) != header.crc) {
        ++accounted_drops;
        report_error(callbacks, camera, true, "OIMG payload CRC mismatch");
        continue;
      }
      if (header.format == ImageStreamFormat::jpeg && !valid_jpeg(payload)) {
        ++accounted_drops;
        report_error(callbacks, camera, false, "OIMG JPEG markers invalid");
        continue;
      }

      const std::int64_t received_ns = host_time_ns();
      const std::int64_t sensor_ns =
          static_cast<std::int64_t>(header.sensor_timestamp_ns);
      if (header.format == ImageStreamFormat::nv12) {
        if (callbacks.on_frame) {
          FrameView view;
          view.data = payload.data();
          view.size = payload.size();
          view.camera_index = camera;
          view.sequence = header.sequence;
          view.timestamp_ns = received_ns;
          view.sensor_timestamp_ns = sensor_ns;
          view.width = header.width;
          view.height = header.height;
          view.stride = header.stride;
          view.pixel_format = PixelFormat::nv12;
          view.layout = FrameLayout::single_plane;
          view.crc_valid = true;
          callbacks.on_frame(view);
        }
      } else if (callbacks.on_compressed_frame) {
        CompressedFrameView view;
        view.data = payload.data();
        view.size = payload.size();
        view.camera_index = camera;
        view.width = header.width;
        view.height = header.height;
        view.sequence = header.sequence;
        view.timestamp_ns = received_ns;
        view.sensor_timestamp_ns = sensor_ns;
        view.crc_valid = true;
        callbacks.on_compressed_frame(view);
      }
      ++delivered;
    }

    const std::uint32_t remaining =
        delivered < config_.frames_per_camera
            ? config_.frames_per_camera - delivered
            : 0U;
    if (!stop_requested_.load() && remaining > accounted_drops) {
      report_loss(callbacks, camera, remaining - accounted_drops);
    }

    clear_socket(camera, fd);
    ::close(fd);
  }

  ImageStreamConfig config_;
  mutable std::mutex callback_mutex_;
  RuntimeCallbacks callbacks_;
  std::mutex lifecycle_mutex_;
  std::mutex socket_mutex_;
  std::array<int, 4U> sockets_{-1, -1, -1, -1};
  std::vector<std::thread> workers_;
  std::atomic<bool> opened_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint32_t> active_workers_{0U};
};

std::unique_ptr<Runtime> make_image_stream_runtime(
    const ImageStreamConfig& config) {
  return std::make_unique<ImageStreamRuntime>(config);
}

}  // namespace orbvi_sdk::detail

namespace orbvi_sdk {

Client make_image_stream_client(const ImageStreamConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::image_stream;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<ImageStreamConfig>(config);
  return Client{std::move(options)};
}

}  // namespace orbvi_sdk
