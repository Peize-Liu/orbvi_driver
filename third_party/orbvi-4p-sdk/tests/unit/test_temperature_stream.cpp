#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
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
#include "orbvi_sdk/temperature_stream.hpp"
#include "test_support.hpp"

namespace {

constexpr std::size_t kOcpuPacketSize = 32U;

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

void update_crc(std::array<std::uint8_t, kOcpuPacketSize>& packet) {
  write_be(packet.data() + 28U, crc32_iso_hdlc(packet.data(), 28U), 4U);
}

std::array<std::uint8_t, kOcpuPacketSize> make_packet_bits(
    std::uint32_t temperature_bits, std::uint64_t sequence,
    std::uint64_t monotonic_ns) {
  std::array<std::uint8_t, kOcpuPacketSize> packet{};
  std::memcpy(packet.data(), "OCPU", 4U);
  packet[4] = 1U;
  packet[5] = 1U;
  write_be(packet.data() + 8U, sequence, 8U);
  write_be(packet.data() + 16U, monotonic_ns, 8U);
  write_be(packet.data() + 24U, temperature_bits, 4U);
  update_crc(packet);
  return packet;
}

std::array<std::uint8_t, kOcpuPacketSize> make_packet(
    float celsius, std::uint64_t sequence, std::uint64_t monotonic_ns) {
  std::uint32_t bits = 0U;
  std::memcpy(&bits, &celsius, sizeof(bits));
  return make_packet_bits(bits, sequence, monotonic_ns);
}

bool send_packet(std::uint16_t port,
                 const std::array<std::uint8_t, kOcpuPacketSize>& packet) {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
    close(fd);
    return false;
  }
  const ssize_t sent =
      sendto(fd, packet.data(), packet.size(), 0,
             reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  close(fd);
  return sent == static_cast<ssize_t>(packet.size());
}

std::uint16_t reserve_udp_port() {
  const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return 0U;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(0U);
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(fd);
    return 0U;
  }
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    close(fd);
    return 0U;
  }
  const std::uint16_t port = ntohs(address.sin_port);
  close(fd);
  return port;
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

void expect_invalid_config(const orbvi_sdk::TemperatureStreamConfig& config,
                           const std::string& expected_message) {
  orbvi_sdk::Client client = orbvi_sdk::make_temperature_stream_client(config);
  const orbvi_sdk::Status status = client.open();
  CHECK_EQ(status.code(), orbvi_sdk::StatusCode::invalid_argument);
  CHECK_TRUE(status.message().find(expected_message) != std::string::npos);
}

}  // namespace

int main() {
  using namespace orbvi_sdk;

  {
    const TemperatureStreamConfig defaults;
    CHECK_EQ(defaults.port, 5620U);
    CHECK_EQ(defaults.samples, 0U);
  }

  {
    const auto packet = make_packet(42.5F, 0x0102030405060708ULL,
                                    0x1112131415161718ULL);
    const auto parsed = parse_ocpu_packet(packet.data(), packet.size());
    CHECK_TRUE(parsed.ok());
    CHECK_EQ(parsed.value().celsius, 42.5F);
    CHECK_EQ(parsed.value().sequence, 0x0102030405060708ULL);
    CHECK_EQ(parsed.value().monotonic_ns, 0x1112131415161718ULL);

    CHECK_FALSE(parse_ocpu_packet(nullptr, packet.size()).ok());
    CHECK_FALSE(parse_ocpu_packet(packet.data(), packet.size() - 1U).ok());
    std::array<std::uint8_t, kOcpuPacketSize + 1U> oversized{};
    std::memcpy(oversized.data(), packet.data(), packet.size());
    CHECK_FALSE(parse_ocpu_packet(oversized.data(), oversized.size()).ok());

    auto corrupted = packet;
    corrupted[28] ^= 1U;
    CHECK_FALSE(parse_ocpu_packet(corrupted.data(), corrupted.size()).ok());

    corrupted = packet;
    corrupted[6] = 1U;
    update_crc(corrupted);
    CHECK_FALSE(parse_ocpu_packet(corrupted.data(), corrupted.size()).ok());

    corrupted = packet;
    corrupted[5] = 2U;
    update_crc(corrupted);
    CHECK_FALSE(parse_ocpu_packet(corrupted.data(), corrupted.size()).ok());

    corrupted = packet;
    corrupted[4] = 2U;
    update_crc(corrupted);
    CHECK_FALSE(parse_ocpu_packet(corrupted.data(), corrupted.size()).ok());

    corrupted = packet;
    corrupted[0] = 'X';
    update_crc(corrupted);
    CHECK_FALSE(parse_ocpu_packet(corrupted.data(), corrupted.size()).ok());

    const auto nan_packet = make_packet_bits(0x7fc00000U, 1U, 1U);
    CHECK_FALSE(parse_ocpu_packet(nan_packet.data(), nan_packet.size()).ok());
    const auto inf_packet = make_packet_bits(0x7f800000U, 1U, 1U);
    CHECK_FALSE(parse_ocpu_packet(inf_packet.data(), inf_packet.size()).ok());
  }

  {
    TemperatureStreamConfig invalid;
    invalid.bind_address = "localhost";
    expect_invalid_config(invalid, "bind_address");

    invalid.bind_address = "127.0.0.1";
    invalid.port = 0U;
    expect_invalid_config(invalid, "port");

    invalid.port = 5620U;
    invalid.samples = 100001U;
    expect_invalid_config(invalid, "samples");
  }

  {
    const std::uint16_t port = reserve_udp_port();
    CHECK_TRUE(port != 0U);
    TemperatureStreamConfig config;
    config.bind_address = "127.0.0.1";
    config.port = port;
    config.samples = 2U;
    Client client = make_temperature_stream_client(config);

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<TemperatureSample> samples;
    std::vector<std::int64_t> host_stamps;
    CallbackSet callbacks;
    callbacks.on_temperature = [&](const TemperatureSample& sample) {
      const std::int64_t host_stamp =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
      {
        const std::lock_guard<std::mutex> lock(mutex);
        samples.push_back(sample);
        host_stamps.push_back(host_stamp);
      }
      ready.notify_all();
    };
    auto subscription = client.subscribe({}, std::move(callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());

    auto bad_crc = make_packet(20.0F, 0U, 100U);
    bad_crc[28] ^= 1U;
    CHECK_TRUE(send_packet(config.port, bad_crc));

    auto bad_reserved = make_packet(21.0F, 0U, 101U);
    bad_reserved[6] = 1U;
    update_crc(bad_reserved);
    CHECK_TRUE(send_packet(config.port, bad_reserved));

    CHECK_TRUE(send_packet(config.port, make_packet(22.5F, 0U, 102U)));
    CHECK_TRUE(send_packet(config.port, make_packet(23.75F, 2U, 104U)));

    {
      std::unique_lock<std::mutex> lock(mutex);
      CHECK_TRUE(ready.wait_for(lock, std::chrono::seconds{3},
                                [&]() { return samples.size() >= 2U; }));
    }
    CHECK_TRUE(wait_for(
        [&]() { return client.state() == ClientState::stopped; },
        std::chrono::seconds{2}));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    std::vector<TemperatureSample> sample_snapshot;
    std::vector<std::int64_t> stamp_snapshot;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      sample_snapshot = samples;
      stamp_snapshot = host_stamps;
    }
    CHECK_EQ(sample_snapshot.size(), 2U);
    if (sample_snapshot.size() == 2U) {
      CHECK_EQ(sample_snapshot[0].sequence, 0U);
      CHECK_EQ(sample_snapshot[0].celsius, 22.5F);
      CHECK_EQ(sample_snapshot[0].monotonic_ns, 102U);
      CHECK_EQ(sample_snapshot[1].sequence, 2U);
      CHECK_EQ(sample_snapshot[1].celsius, 23.75F);
      CHECK_EQ(sample_snapshot[1].monotonic_ns, 104U);
      CHECK_TRUE(stamp_snapshot[0] > 1'600'000'000'000'000'000LL);
      CHECK_TRUE(stamp_snapshot[1] > 1'600'000'000'000'000'000LL);
    }
    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.temperature_samples_received, 2U);
    CHECK_EQ(diagnostics.temperature_samples_delivered, 2U);
    CHECK_EQ(diagnostics.temperature_samples_dropped, 3U);
    CHECK_EQ(diagnostics.temperature_crc_errors, 1U);
    CHECK_EQ(diagnostics.temperature_sequence_gaps, 1U);
    CHECK_EQ(diagnostics.parse_errors, 1U);
    CHECK_EQ(diagnostics.crc_errors, 1U);
  }

  {
    // OCPU has its own UDP socket and lifecycle. A concurrently running OIMG
    // client awaiting START/data must not prevent finite CPU delivery.
    const std::uint16_t port = reserve_udp_port();
    CHECK_TRUE(port != 0U);
    TemperatureStreamConfig cpu_config;
    cpu_config.bind_address = "127.0.0.1";
    cpu_config.port = port;
    cpu_config.samples = 1U;
    Client cpu = make_temperature_stream_client(cpu_config);
    CallbackSet cpu_callbacks;
    std::atomic<std::uint32_t> callbacks{0U};
    cpu_callbacks.on_temperature = [&](const TemperatureSample&) { ++callbacks; };
    auto subscription = cpu.subscribe({}, std::move(cpu_callbacks));
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(cpu.open().ok());

    const int image_listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK_TRUE(image_listener >= 0);
    sockaddr_in image_address{};
    image_address.sin_family = AF_INET;
    image_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    image_address.sin_port = 0;
    CHECK_TRUE(bind(image_listener,
                    reinterpret_cast<const sockaddr*>(&image_address),
                    sizeof(image_address)) == 0);
    socklen_t address_size = sizeof(image_address);
    CHECK_TRUE(getsockname(image_listener,
                           reinterpret_cast<sockaddr*>(&image_address),
                           &address_size) == 0);
    CHECK_TRUE(listen(image_listener, 1) == 0);
    ImageStreamConfig image_config;
    image_config.base_port = ntohs(image_address.sin_port);
    image_config.host = "127.0.0.1";
    image_config.camera_mask = 0x01U;
    image_config.connect_timeout = std::chrono::seconds{2};
    Client image = make_image_stream_client(image_config);
    CHECK_TRUE(image.open().ok());
    CHECK_TRUE(image.start().ok());

    CHECK_TRUE(cpu.start().ok());
    CHECK_TRUE(send_packet(port, make_packet(31.0F, 1U, 100U)));
    CHECK_TRUE(subscription.value().wait_for_delivery(1U,
                                                      std::chrono::seconds{2}));
    CHECK_EQ(callbacks.load(), 1U);
    CHECK_TRUE(image.stop().ok());
    CHECK_TRUE(image.close().ok());
    close(image_listener);
    CHECK_TRUE(cpu.stop().ok());
    // Reuse the same bound UDP socket for another finite run.
    CHECK_TRUE(cpu.start().ok());
    CHECK_TRUE(send_packet(port, make_packet(32.0F, 2U, 200U)));
    CHECK_TRUE(subscription.value().wait_for_delivery(2U,
                                                      std::chrono::seconds{2}));
    CHECK_TRUE(cpu.close().ok());
    CHECK_EQ(cpu.diagnostics().temperature_samples_delivered, 2U);
    CHECK_EQ(cpu.diagnostics().temperature_samples_dropped, 0U);
    CHECK_EQ(cpu.diagnostics().parse_errors, 0U);
  }

  return orbvi_test::finish();
}
