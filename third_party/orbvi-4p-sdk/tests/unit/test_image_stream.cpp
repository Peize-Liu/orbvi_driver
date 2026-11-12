#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/image_stream.hpp"
#include "test_support.hpp"

namespace {

constexpr std::uint16_t kLoopbackPort = 0U;
constexpr std::size_t kOimgHeaderSize = 48U;

struct ServeOptions {
  bool invalid_magic{false};
  bool non_tight_nv12{false};
  bool corrupt_crc{false};
  bool corrupt_first_only{false};
  bool bad_jpeg_markers{false};
  bool truncate_payload{false};
  bool truncate_header{false};
};

struct ServerResult {
  bool accepted{false};
  bool octl_ok{false};
  bool sent{false};
};

void write_be(std::uint8_t* output, std::uint64_t value, unsigned count) {
  for (unsigned index = 0U; index < count; ++index) {
    const unsigned shift = 8U * (count - index - 1U);
    output[index] = static_cast<std::uint8_t>(value >> shift);
  }
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

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t count = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool read_all(int fd, std::uint8_t* data, std::size_t size) {
  std::size_t offset = 0U;
  while (offset < size) {
    const ssize_t count = recv(fd, data + offset, size - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
    } else if (count < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool make_listener(std::uint16_t requested_port, int& fd,
                   std::uint16_t& bound_port) {
  fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  int reuse = 1;
  static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                               sizeof(reuse)));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(requested_port);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(fd);
    fd = -1;
    return false;
  }
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
      listen(fd, 4) != 0) {
    close(fd);
    fd = -1;
    return false;
  }
  bound_port = ntohs(address.sin_port);
  return true;
}

bool make_listener_pair(int& first_fd, int& second_fd,
                        std::uint16_t& base_port) {
  for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
    std::uint16_t first_port = 0U;
    if (!make_listener(kLoopbackPort, first_fd, first_port)) {
      continue;
    }
    if (first_port > 65532U) {
      close(first_fd);
      first_fd = -1;
      continue;
    }
    std::uint16_t second_port = 0U;
    if (make_listener(static_cast<std::uint16_t>(first_port + 1U), second_fd,
                      second_port)) {
      base_port = first_port;
      return true;
    }
    close(first_fd);
    first_fd = -1;
  }
  return false;
}

bool make_four_listeners(std::array<int, 4U>& fds,
                         std::uint16_t& base_port) {
  for (unsigned attempt = 0U; attempt < 64U; ++attempt) {
    std::uint16_t port = 0U;
    if (!make_listener(0U, fds[0], port)) continue;
    bool complete = port <= 65532U;
    for (std::uint32_t camera = 1U; complete && camera < 4U; ++camera) {
      std::uint16_t bound = 0U;
      complete = make_listener(static_cast<std::uint16_t>(port + camera),
                               fds[camera], bound);
    }
    if (complete) {
      base_port = port;
      return true;
    }
    for (int& fd : fds) {
      if (fd >= 0) close(fd);
      fd = -1;
    }
  }
  return false;
}

int accept_with_timeout(int listen_fd, int timeout_ms) {
  pollfd descriptor{listen_fd, POLLIN, 0};
  const int result = poll(&descriptor, 1, timeout_ms);
  if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
    return -1;
  }
  return accept(listen_fd, nullptr, nullptr);
}

std::array<std::uint8_t, kOimgHeaderSize> make_header(
    std::uint8_t kind, std::uint8_t camera, std::uint64_t sequence,
    std::uint64_t sensor_timestamp_ns, std::uint32_t width,
    std::uint32_t height, std::uint32_t stride,
    const std::vector<std::uint8_t>& payload, bool invalid_magic,
    bool corrupt_crc) {
  std::array<std::uint8_t, kOimgHeaderSize> header{};
  const char* magic = invalid_magic ? "OIMX" : "OIMG";
  std::memcpy(header.data(), magic, 4U);
  header[4] = 1U;
  header[5] = kind;
  header[6] = camera;
  write_be(header.data() + 8U, kOimgHeaderSize, 2U);
  write_be(header.data() + 12U, sequence, 8U);
  write_be(header.data() + 20U, sensor_timestamp_ns, 8U);
  write_be(header.data() + 28U, width, 4U);
  write_be(header.data() + 32U, height, 4U);
  write_be(header.data() + 36U, stride, 4U);
  write_be(header.data() + 40U, payload.size(), 4U);
  std::uint32_t crc = crc32_iso_hdlc(payload.data(), payload.size());
  if (corrupt_crc) {
    crc ^= 1U;
  }
  write_be(header.data() + 44U, crc, 4U);
  return header;
}

void serve_camera(int listen_fd, std::uint8_t camera, std::uint8_t kind,
                  std::uint32_t frame_count, ServeOptions options,
                  ServerResult& result) {
  const int connection = accept_with_timeout(listen_fd, 3000);
  if (connection < 0) {
    return;
  }
  result.accepted = true;

  std::array<std::uint8_t, 8U> control{};
  if (!read_all(connection, control.data(), control.size())) {
    close(connection);
    return;
  }
  result.octl_ok =
      std::memcmp(control.data(), "OCTL", 4U) == 0 && control[4] == 1U &&
      control[5] == 1U && control[6] == camera && control[7] == 0U;

  bool all_sent = true;
  for (std::uint32_t index = 0U; index < frame_count; ++index) {
    const bool jpeg = kind == 2U;
    std::vector<std::uint8_t> payload =
        jpeg ? std::vector<std::uint8_t>{0xffU, 0xd8U, 0xffU, 0xd9U}
             : std::vector<std::uint8_t>{0x10U, 0x20U, 0x30U, 0x40U,
                                         0x50U, 0x60U, 0x70U, 0x80U,
                                         0x90U, 0xa0U, 0xb0U, 0xc0U};
    const std::uint32_t width = jpeg ? 8U : 4U;
    const std::uint32_t height = jpeg ? 8U : 2U;
    std::uint32_t stride = jpeg ? 0U : width;
    if (!jpeg && options.non_tight_nv12) {
      stride = width + 4U;
    }
    if (jpeg && options.bad_jpeg_markers) {
      payload.assign(payload.size(), 0U);
    }

    const auto header = make_header(
        kind, camera, static_cast<std::uint64_t>(index) + 1U,
        0x0102030405060708ULL + index, width, height, stride, payload,
        options.invalid_magic,
        options.corrupt_crc || (options.corrupt_first_only && index == 0U));
    if (options.truncate_header) {
      all_sent = write_all(connection, header.data(), 8U);
      break;
    }
    if (!write_all(connection, header.data(), header.size())) {
      all_sent = false;
      break;
    }
    if (options.truncate_payload) {
      const std::size_t truncated = payload.size() > 4U ? 4U : payload.size();
      if (!write_all(connection, payload.data(), truncated)) {
        all_sent = false;
      }
      break;
    }
    if (!write_all(connection, payload.data(), payload.size())) {
      all_sent = false;
      break;
    }
  }
  result.sent = all_sent;
  close(connection);
}

bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  return predicate();
}

void close_listener(int& fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

void expect_invalid_config(orbvi_sdk::ImageStreamConfig config,
                           const std::string& expected_message) {
  orbvi_sdk::Client client = orbvi_sdk::make_image_stream_client(config);
  const orbvi_sdk::Status status = client.open();
  CHECK_EQ(status.code(), orbvi_sdk::StatusCode::invalid_argument);
  CHECK_TRUE(status.message().find(expected_message) != std::string::npos);
}

}  // namespace

int main() {
  using namespace orbvi_sdk;
  static_assert(static_cast<std::uint8_t>(ImageStreamFormat::nv12) == 1U,
                "OIMG/1 NV12 kind must be 1");
  static_assert(static_cast<std::uint8_t>(ImageStreamFormat::jpeg) == 2U,
                "OIMG/1 JPEG kind must be 2");

  for (const ImageStreamFormat format : {ImageStreamFormat::nv12,
                                         ImageStreamFormat::jpeg}) {
    // Four independent TCP ports, one START per camera, finite N=2.
    std::array<int, 4U> listeners{-1, -1, -1, -1};
    std::uint16_t base_port = 0U;
    CHECK_TRUE(make_four_listeners(listeners, base_port));
    std::array<ServerResult, 4U> results{};
    std::vector<std::thread> servers;
    for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
      servers.emplace_back(serve_camera, listeners[camera],
                           static_cast<std::uint8_t>(camera),
                           static_cast<std::uint8_t>(format), 2U,
                           ServeOptions{}, std::ref(results[camera]));
    }
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = base_port;
    config.camera_mask = 0x0fU;
    config.frames_per_camera = 2U;
    config.format = format;
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    if (format == ImageStreamFormat::nv12) {
      callbacks.on_frame = [](const FrameView& view) {
        CHECK_EQ(view.pixel_format, PixelFormat::nv12);
        CHECK_EQ(view.size, 12U);
      };
    } else {
      callbacks.on_compressed_frame = [](const CompressedFrameView& view) {
        CHECK_EQ(view.size, 4U);
        CHECK_TRUE(view.crc_valid);
      };
    }
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(8U,
                                                      std::chrono::seconds{3}));
    CHECK_TRUE(wait_for([&] { return client.state() == ClientState::stopped; },
                        std::chrono::seconds{2}));
    CHECK_TRUE(client.close().ok());
    for (auto& server : servers) server.join();
    for (int fd : listeners) close_listener(fd);
    const auto diagnostics = client.diagnostics();
    for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
      CHECK_TRUE(results[camera].octl_ok);
      CHECK_TRUE(results[camera].sent);
      CHECK_EQ(diagnostics.image_delivered[camera], 2U);
      CHECK_EQ(diagnostics.image_dropped[camera], 0U);
    }
  }

  {
    int first_fd = -1;
    int second_fd = -1;
    std::uint16_t base_port = 0U;
    CHECK_TRUE(make_listener_pair(first_fd, second_fd, base_port));
    ServerResult first_result;
    ServerResult second_result;
    std::thread first_server(serve_camera, first_fd, 0U, 1U, 2U,
                             ServeOptions{}, std::ref(first_result));
    std::thread second_server(serve_camera, second_fd, 1U, 1U, 2U,
                              ServeOptions{}, std::ref(second_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = base_port;
    config.camera_mask = 0x03U;
    config.frames_per_camera = 2U;
    config.format = ImageStreamFormat::nv12;
    config.read_timeout = std::chrono::milliseconds{1000};
    Client client = make_image_stream_client(config);

    std::mutex mutex;
    std::array<std::uint32_t, 4U> frames{};
    CallbackSet callbacks;
    callbacks.on_frame = [&](const FrameView& view) {
      const std::lock_guard<std::mutex> lock(mutex);
      if (view.camera_index < frames.size()) {
        ++frames[view.camera_index];
      }
      CHECK_EQ(view.pixel_format, PixelFormat::nv12);
      CHECK_EQ(view.stride, view.width);
      CHECK_EQ(view.size,
               static_cast<std::size_t>(view.width) * view.height * 3U / 2U);
      CHECK_TRUE(view.timestamp_ns > 0);
      CHECK_EQ(view.sensor_timestamp_ns,
               0x0102030405060708LL +
                   static_cast<std::int64_t>(view.sequence - 1U));
    };
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(4U,
                                                      std::chrono::seconds{3}));
    CHECK_TRUE(wait_for([&]() { return client.state() == ClientState::stopped; },
                        std::chrono::seconds{1}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    first_server.join();
    second_server.join();
    close_listener(first_fd);
    close_listener(second_fd);

    CHECK_TRUE(first_result.accepted);
    CHECK_TRUE(first_result.octl_ok);
    CHECK_TRUE(first_result.sent);
    CHECK_TRUE(second_result.accepted);
    CHECK_TRUE(second_result.octl_ok);
    CHECK_TRUE(second_result.sent);
    std::array<std::uint32_t, 4U> frame_snapshot{};
    {
      const std::lock_guard<std::mutex> lock(mutex);
      frame_snapshot = frames;
    }
    CHECK_EQ(frame_snapshot[0], 2U);
    CHECK_EQ(frame_snapshot[1], 2U);
    CHECK_EQ(frame_snapshot[2], 0U);
    CHECK_EQ(frame_snapshot[3], 0U);
    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.image_delivered[0], 2U);
    CHECK_EQ(diagnostics.image_delivered[1], 2U);
    CHECK_EQ(diagnostics.image_dropped[0], 0U);
    CHECK_EQ(diagnostics.image_dropped[1], 0U);
    CHECK_EQ(diagnostics.image_crc_errors[0], 0U);
    CHECK_EQ(diagnostics.image_crc_errors[1], 0U);
  }

  {
    // Camera 1's incoming frame evicts camera 0's queued frame. Attribute the
    // queue drop to the evicted camera, not the camera of the new frame.
    int first_fd = -1;
    int second_fd = -1;
    std::uint16_t base_port = 0U;
    CHECK_TRUE(make_listener_pair(first_fd, second_fd, base_port));
    ServerResult first_result;
    ServerResult second_result;
    std::thread first_server(serve_camera, first_fd, 0U, 1U, 2U,
                             ServeOptions{}, std::ref(first_result));
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = base_port;
    config.camera_mask = 0x03U;
    config.frames_per_camera = 2U;
    config.read_timeout = std::chrono::milliseconds{1000};
    Client client = make_image_stream_client(config);
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool inside_callback = false;
    bool release_callback = false;
    CallbackSet callbacks;
    callbacks.on_frame = [&](const FrameView&) {
      std::unique_lock<std::mutex> lock(gate_mutex);
      if (!inside_callback) {
        inside_callback = true;
        gate.notify_all();
        gate.wait_for(lock, std::chrono::seconds{3}, [&] { return release_callback; });
      }
    };
    auto subscription = client.subscribe({1U, DropPolicy::drop_oldest},
                                         std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      CHECK_TRUE(gate.wait_for(lock, std::chrono::seconds{2},
                               [&] { return inside_callback; }));
    }
    CHECK_TRUE(wait_for([&] { return client.diagnostics().frames_received >= 2U; },
                        std::chrono::seconds{2}));
    std::thread second_server(serve_camera, second_fd, 1U, 1U, 1U,
                              ServeOptions{}, std::ref(second_result));
    CHECK_TRUE(wait_for([&] { return client.diagnostics().frames_received >= 3U; },
                        std::chrono::seconds{2}));
    {
      const std::lock_guard<std::mutex> lock(gate_mutex);
      release_callback = true;
    }
    gate.notify_all();
    CHECK_TRUE(subscription.value().wait_for_delivery(2U,
                                                      std::chrono::seconds{3}));
    CHECK_TRUE(wait_for([&] { return client.state() == ClientState::stopped; },
                        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    first_server.join();
    second_server.join();
    close_listener(first_fd);
    close_listener(second_fd);
    const auto diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.image_delivered[0], 1U);
    CHECK_EQ(diagnostics.image_delivered[1], 1U);
    CHECK_EQ(diagnostics.image_dropped[0], 1U);
    CHECK_EQ(diagnostics.image_dropped[1], 1U); // camera 1 sent only one of two
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    std::thread server(serve_camera, listen_fd, 0U, 2U, 1U, ServeOptions{},
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::jpeg;
    config.read_timeout = std::chrono::milliseconds{1000};
    Client client = make_image_stream_client(config);

    std::mutex mutex;
    std::uint32_t frames = 0U;
    CallbackSet callbacks;
    callbacks.on_compressed_frame = [&](const CompressedFrameView& view) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++frames;
      CHECK_EQ(view.camera_index, 0U);
      CHECK_EQ(view.size, 4U);
      CHECK_EQ(view.width, 8U);
      CHECK_EQ(view.height, 8U);
      CHECK_TRUE(view.timestamp_ns > 0);
      CHECK_TRUE(view.crc_valid);
    };
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(1U,
                                                      std::chrono::seconds{3}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    server.join();
    close_listener(listen_fd);
    CHECK_TRUE(server_result.accepted);
    CHECK_TRUE(server_result.octl_ok);
    CHECK_TRUE(server_result.sent);
    CHECK_EQ(frames, 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.corrupt_crc = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::nv12;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    std::atomic<std::uint32_t> frames{0U};
    CallbackSet callbacks;
    callbacks.on_frame = [&](const FrameView&) { ++frames; };
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for(
        [&]() { return client.diagnostics().image_crc_errors[0] == 1U; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_TRUE(server_result.octl_ok);
    CHECK_EQ(frames.load(), 0U);
    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.image_delivered[0], 0U);
    CHECK_EQ(diagnostics.image_dropped[0], 1U);
    CHECK_EQ(diagnostics.image_crc_errors[0], 1U);
  }

  {
    // A bad first payload is one lost sequence, not both a CRC drop and a
    // sequence-gap drop when the next valid frame arrives.
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.corrupt_first_only = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 2U, options,
                       std::ref(server_result));
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 2U;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {};
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(1U,
                                                      std::chrono::seconds{3}));
    CHECK_TRUE(wait_for([&]() { return client.state() == ClientState::stopped; },
                        std::chrono::seconds{1}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.image_delivered[0], 1U);
    CHECK_EQ(diagnostics.image_dropped[0], 1U);
    CHECK_EQ(diagnostics.image_crc_errors[0], 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.non_tight_nv12 = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::nv12;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {};
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for(
        [&]() { return client.diagnostics().image_dropped[0] == 1U; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_EQ(client.diagnostics().image_delivered[0], 0U);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.bad_jpeg_markers = true;
    std::thread server(serve_camera, listen_fd, 0U, 2U, 1U, options,
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::jpeg;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    callbacks.on_compressed_frame = [](const CompressedFrameView&) {};
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for(
        [&]() { return client.diagnostics().image_dropped[0] == 1U; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_EQ(client.diagnostics().image_delivered[0], 0U);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.invalid_magic = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::nv12;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {};
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for(
        [&]() { return client.diagnostics().image_dropped[0] == 1U; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_EQ(client.diagnostics().image_delivered[0], 0U);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult server_result;
    ServeOptions options;
    options.truncate_payload = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(server_result));

    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 0x01U;
    config.frames_per_camera = 1U;
    config.format = ImageStreamFormat::nv12;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {};
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for(
        [&]() { return client.diagnostics().image_dropped[0] == 1U; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_EQ(client.diagnostics().image_delivered[0], 0U);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
  }

  {
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult result;
    ServeOptions options;
    options.truncate_header = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(result));
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 1U;
    config.frames_per_camera = 1U;
    config.read_timeout = std::chrono::milliseconds{300};
    Client client = make_image_stream_client(config);
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for([&] { return client.state() == ClientState::stopped; },
                        std::chrono::seconds{2}));
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_TRUE(result.octl_ok);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
    CHECK_EQ(client.diagnostics().parse_errors, 1U);
  }

  {
    // Corrupt the final wire frame: no valid callback, but the finite wire
    // range is complete and must not wait for another read timeout.
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    ServerResult result;
    ServeOptions options;
    options.corrupt_crc = true;
    std::thread server(serve_camera, listen_fd, 0U, 1U, 1U, options,
                       std::ref(result));
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 1U;
    config.frames_per_camera = 1U;
    config.read_timeout = std::chrono::seconds{3};
    Client client = make_image_stream_client(config);
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for([&] { return client.state() == ClientState::stopped; },
                        std::chrono::milliseconds{700}));
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_TRUE(result.octl_ok);
    CHECK_EQ(client.diagnostics().image_dropped[0], 1U);
    CHECK_EQ(client.diagnostics().image_crc_errors[0], 1U);
    CHECK_EQ(client.diagnostics().parse_errors, 0U);
  }

  {
    // Explicit cancellation must not invent missing frames or parse errors.
    int listen_fd = -1;
    std::uint16_t port = 0U;
    CHECK_TRUE(make_listener(kLoopbackPort, listen_fd, port));
    std::atomic<bool> got_start{false};
    std::thread server([&] {
      const int fd = accept_with_timeout(listen_fd, 3000);
      if (fd >= 0) {
        std::array<std::uint8_t, 8U> start{};
        got_start = read_all(fd, start.data(), start.size());
        std::array<std::uint8_t, 1U> unused{};
        static_cast<void>(recv(fd, unused.data(), unused.size(), 0));
        close(fd);
      }
    });
    ImageStreamConfig config;
    config.host = "127.0.0.1";
    config.base_port = port;
    config.camera_mask = 1U;
    config.frames_per_camera = 10U;
    config.read_timeout = std::chrono::seconds{3};
    Client client = make_image_stream_client(config);
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_for([&] { return got_start.load(); },
                        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    server.join();
    close_listener(listen_fd);
    CHECK_EQ(client.diagnostics().image_dropped[0], 0U);
    CHECK_EQ(client.diagnostics().parse_errors, 0U);
  }

  {
    ImageStreamConfig config;
    config.host = "localhost";
    expect_invalid_config(config, "host");

    config.host = "127.0.0.1";
    config.camera_mask = 0U;
    expect_invalid_config(config, "camera_mask");

    config.camera_mask = 0x0fU;
    config.frames_per_camera = 0U;
    expect_invalid_config(config, "frames_per_camera");

    config.frames_per_camera = 1U;
    config.format = static_cast<ImageStreamFormat>(3U);
    expect_invalid_config(config, "format");
  }

  return orbvi_test::finish();
}
