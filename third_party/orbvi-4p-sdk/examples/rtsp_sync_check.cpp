#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/rtsp.hpp"

namespace {
struct Counts {
  std::uint64_t received{0};
  std::uint64_t missing_metadata{0};
  std::uint64_t missing_frames{0};
  std::uint64_t discontinuities{0};
  std::uint32_t previous_id{0};
  std::int64_t previous_timestamp{0};
  std::int64_t first_capture_timestamp{0};
  std::int64_t first_host_timestamp{0};
  std::int64_t last_host_timestamp{0};
  bool seen{false};
};
}

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: orbvi_sdk_rtsp_sync_check <url0> <url1> <url2> <url3> [seconds]\n";
    return 2;
  }
  unsigned long seconds = 60;
  if (argc == 6) {
    char* end = nullptr;
    seconds = std::strtoul(argv[5], &end, 10);
    if (*argv[5] == '\0' || *end != '\0' || seconds < 1 || seconds > 86400) return 2;
  }
  orbvi_sdk::RtspConfig config;
  for (std::size_t index = 0; index < config.urls.size(); ++index)
    config.urls[index] = argv[index + 1];
  auto client = orbvi_sdk::make_rtsp_client(config, orbvi_sdk::PixelFormat::bgr8);
  std::array<Counts, 4> counts{};
  std::atomic<bool> measuring{false};
  std::mutex mutex;
  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_frame = [&](const orbvi_sdk::FrameView& frame) {
    if (frame.camera_index >= counts.size() || !measuring.load()) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!measuring.load()) return;
    auto& current = counts[frame.camera_index];
    ++current.received;
    if (current.first_host_timestamp == 0) current.first_host_timestamp = frame.timestamp_ns;
    current.last_host_timestamp = frame.timestamp_ns;
    if (frame.sensor_timestamp_ns <= 0) {
      ++current.missing_metadata;
      return;
    }
    const auto frame_id = static_cast<std::uint32_t>(frame.sequence);
    if (!current.seen) current.first_capture_timestamp = frame.sensor_timestamp_ns;
    if (current.seen) {
      const std::uint32_t advance = frame_id - current.previous_id;
      if (advance > 1 && advance < 0x80000000U) current.missing_frames += advance - 1;
      else if (advance != 1) ++current.discontinuities;
      if (frame.sensor_timestamp_ns <= current.previous_timestamp) ++current.discontinuities;
    }
    current.previous_id = frame_id;
    current.previous_timestamp = frame.sensor_timestamp_ns;
    current.seen = true;
  };
  auto subscription = client.subscribe(
      orbvi_sdk::SubscribeOptions{256U, orbvi_sdk::DropPolicy::drop_oldest}, std::move(callbacks));
  if (!subscription.ok()) {
    std::cerr << "subscribe: " << subscription.status().to_string() << '\n';
    return 3;
  }
  auto status = client.open();
  if (!status.ok()) {
    std::cerr << "open: " << status.to_string() << '\n';
    return 4;
  }
  status = client.start();
  if (!status.ok()) {
    std::cerr << "start: " << status.to_string() << '\n';
    static_cast<void>(client.close());
    return 5;
  }
  std::this_thread::sleep_for(std::chrono::seconds{5});
  measuring.store(true);
  std::this_thread::sleep_for(std::chrono::seconds{seconds});
  measuring.store(false);
  const auto stop_status = client.stop();
  const auto stats = subscription.value().stats();
  subscription.value().cancel();
  const auto close_status = client.close();
  std::array<Counts, 4> result_counts;
  {
    std::lock_guard<std::mutex> lock(mutex);
    result_counts = counts;
  }
  bool passed = stop_status.ok() && close_status.ok() && stats.dropped == 0 && stats.failed == 0;
  for (std::size_t index = 0; index < result_counts.size(); ++index) {
    const auto& current = result_counts[index];
    const auto elapsed = current.last_host_timestamp - current.first_host_timestamp;
    const double fps = elapsed > 0 ? (current.received - 1) * 1e9 / static_cast<double>(elapsed) : 0;
    const auto capture_elapsed = current.previous_timestamp - current.first_capture_timestamp;
    const double capture_fps = capture_elapsed > 0 ?
        (current.received - 1) * 1e9 / static_cast<double>(capture_elapsed) : 0;
    std::cout << "camera=" << index << " frames=" << current.received << " fps=" << fps
              << " capture_fps=" << capture_fps
              << " missing_metadata=" << current.missing_metadata << " missing_frames=" << current.missing_frames
              << " discontinuities=" << current.discontinuities << " last_id=" << current.previous_id
              << " last_capture_ns=" << current.previous_timestamp << '\n';
    passed = passed && current.received > 1 && current.missing_metadata == 0 &&
             current.missing_frames == 0 && current.discontinuities == 0 && fps >= 29 && fps <= 31;
  }
  std::cout << "subscription_dropped=" << stats.dropped << " callback_failures=" << stats.failed << '\n';
  if (!stop_status.ok()) std::cerr << "stop: " << stop_status.to_string() << '\n';
  if (!close_status.ok()) std::cerr << "close: " << close_status.to_string() << '\n';
  return passed ? 0 : 6;
}
