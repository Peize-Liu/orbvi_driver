#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/rtsp.hpp"

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: orbvi_sdk_rtsp_four_demo <url0> <url1> <url2> <url3>\n";
    return 2;
  }

  orbvi_sdk::RtspConfig config;
  for (std::size_t index = 0; index < config.urls.size(); ++index)
    config.urls[index] = argv[index + 1U];

  auto client = orbvi_sdk::make_rtsp_client(config);
  std::mutex mutex;
  std::condition_variable ready;
  std::array<std::uint64_t, 4> counts{};
  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_frame = [&](const orbvi_sdk::FrameView& frame) {
    if (frame.camera_index >= counts.size()) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++counts[frame.camera_index];
    }
    ready.notify_all();
  };
  auto subscription = client.subscribe(
      orbvi_sdk::SubscribeOptions{32U, orbvi_sdk::DropPolicy::drop_oldest},
      std::move(callbacks));
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
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready.wait_for(lock, std::chrono::seconds{20}, [&] {
      for (auto count : counts) if (count < 10U) return false;
      return true;
    });
  }
  subscription.value().cancel();
  const auto stop_status = client.stop();
  const auto close_status = client.close();
  bool passed = stop_status.ok() && close_status.ok();
  for (std::size_t index = 0; index < counts.size(); ++index) {
    std::cout << "camera_index=" << index << " frames=" << counts[index] << '\n';
    passed = passed && counts[index] >= 10U;
  }
  return passed ? 0 : 6;
}
