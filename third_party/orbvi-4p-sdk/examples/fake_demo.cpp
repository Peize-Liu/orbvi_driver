// Minimal ROS-free example: fake runtime, one device, three frames.
//
// Shows the intended usage pattern for a host consumer (for example
// orbvi_ros_bridge):
//   * create a client from a runtime config,
//   * subscribe before start(),
//   * open() then start(),
//   * wait for a known number of frames,
//   * cancel()/stop()/close() in order.
//
// Threading: callbacks run on the subscription worker thread, never on the
// caller thread. FrameView::data is only valid until the callback returns; copy
// into an orbvi_sdk::OwnedFrame before handing the pixels to another thread.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <utility>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"

int main() {
  constexpr std::size_t kExpectedFrames = 3U;

  orbvi_sdk::FakeConfig config = orbvi_sdk::make_default_fake_config(1U);
  config.frames_per_camera = kExpectedFrames;

  std::mutex mutex;
  std::condition_variable ready;
  std::size_t frame_count = 0U;

  orbvi_sdk::Client client = orbvi_sdk::make_fake_client(config);

  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_frame = [&](const orbvi_sdk::FrameView& view) {
    std::cout << "camera=" << view.camera_index << " sequence=" << view.sequence
              << " bytes=" << view.size << '\n';
    const std::lock_guard<std::mutex> lock(mutex);
    ++frame_count;
    ready.notify_all();
  };

  auto subscription = client.subscribe(
      orbvi_sdk::SubscribeOptions{16U, orbvi_sdk::DropPolicy::drop_oldest},
      std::move(callbacks));
  if (!subscription.ok()) {
    std::cerr << "subscribe failed: " << subscription.status().to_string()
              << '\n';
    return 1;
  }

  if (const orbvi_sdk::Status status = client.open(); !status.ok()) {
    std::cerr << "open failed: " << status.to_string() << '\n';
    return 2;
  }
  if (const orbvi_sdk::Status status = client.start(); !status.ok()) {
    std::cerr << "start failed: " << status.to_string() << '\n';
    static_cast<void>(client.close());
    return 3;
  }

  bool complete = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    complete = ready.wait_for(lock, std::chrono::seconds{5}, [&]() {
      return frame_count >= kExpectedFrames;
    });
  }

  // Shutdown order: cancel the subscription before stopping the client so the
  // callback can no longer run while the caller tears down its own state.
  subscription.value().cancel();
  static_cast<void>(client.stop());
  static_cast<void>(client.close());

  const orbvi_sdk::SubscriptionStats stats = subscription.value().stats();
  std::cout << "frames=" << frame_count << " delivered=" << stats.delivered
            << " dropped=" << stats.dropped << " failed=" << stats.failed << '\n';
  return complete && frame_count == kExpectedFrames ? 0 : 4;
}