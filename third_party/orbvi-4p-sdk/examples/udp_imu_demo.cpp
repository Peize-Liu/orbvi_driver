#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/udp_imu.hpp"

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    std::cerr << "usage: orbvi_sdk_udp_imu_demo <bind-ipv4> <port> [count=100] [timeout-seconds=15]\n";
    return 2;
  }
  orbvi_sdk::UdpImuConfig config;
  config.bind_address = argv[1];
  std::uint64_t count = 100U, seconds = 15U;
  try {
    std::size_t used = 0;
    const auto port = std::stoul(argv[2], &used);
    if (used != std::string(argv[2]).size() || port == 0U || port > 65535U) throw 1;
    config.port = static_cast<std::uint16_t>(port);
    if (argc > 3) { count = std::stoull(argv[3]); if (count == 0U) throw 1; }
    if (argc > 4) { seconds = std::stoull(argv[4]); if (seconds == 0U) throw 1; }
  } catch (...) { std::cerr << "invalid numeric argument\n"; return 2; }
  auto client = orbvi_sdk::make_udp_imu_client(config);
  std::mutex mutex;
  std::condition_variable cv;
  std::uint64_t received = 0U;
  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_imu = [&](const orbvi_sdk::ImuEvent& event) {
    const auto& s = event.sample;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
      std::cout << "OIMU seq=" << s.sequence << " host_ns=" << s.timestamp_ns
                << " accel_iio_ns=" << s.accel_timestamp_ns
                << " gyro_iio_ns=" << s.gyro_timestamp_ns
                << " board_send_ns=" << s.board_send_timestamp_ns << '\n';
    }
    cv.notify_one();
  };
  auto sub = client.subscribe({}, std::move(callbacks));
  if (!sub.ok()) { std::cerr << sub.status().to_string() << '\n'; return 1; }
  auto status = client.open();
  if (status.ok()) status = client.start();
  if (!status.ok()) { std::cerr << status.to_string() << '\n'; static_cast<void>(client.close()); return 1; }
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return received >= count; });
  }
  static_cast<void>(client.stop());
  const auto stats = client.diagnostics();
  static_cast<void>(client.close());
  std::cout << "OIMU_RESULT samples=" << received << " crc_errors=" << stats.crc_errors
            << " parse_errors=" << stats.parse_errors << " sequence_gaps="
            << stats.imu_sequence_gaps << '\n';
  return received >= count && stats.crc_errors == 0U && stats.parse_errors == 0U ? 0 : 1;
}
