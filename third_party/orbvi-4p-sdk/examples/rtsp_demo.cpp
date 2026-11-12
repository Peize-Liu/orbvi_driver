// Minimal single-camera RTSP demo.
//
// This example intentionally supports only camera_index=0 and H.264 input.
// FrameView::data remains borrowed for the duration of the callback only.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/contract.hpp"
#include "orbvi_sdk/rtsp.hpp"

namespace {

bool parse_positive(const char* text, std::uint64_t& value) {
  try {
    std::size_t consumed = 0U;
    const std::string input{text};
    const unsigned long long parsed = std::stoull(input, &consumed, 10);
    if (consumed != input.size() || parsed == 0ULL) {
      return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void print_usage() {
  std::cerr << "usage: orbvi_sdk_rtsp_demo <rtsp-url> [max_frames] "
               "[timeout_seconds] [gray8|bgr8]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 5) {
    print_usage();
    return 2;
  }

  std::uint64_t max_frames = 10U;
  if (argc >= 3 && !parse_positive(argv[2], max_frames)) {
    print_usage();
    return 2;
  }

  std::uint64_t timeout_seconds = 10U;
  if (argc >= 4 && !parse_positive(argv[3], timeout_seconds)) {
    print_usage();
    return 2;
  }

  orbvi_sdk::PixelFormat output_format = orbvi_sdk::PixelFormat::gray8;
  if (argc >= 5) {
    const std::string requested{argv[4]};
    if (requested == "bgr8") {
      output_format = orbvi_sdk::PixelFormat::bgr8;
    } else if (requested != "gray8") {
      print_usage();
      return 2;
    }
  }

  orbvi_sdk::RtspConfig config;
  config.urls[0] = argv[1];
  // The four-slot mapping remains intact: only urls[0] is populated for this
  // single-camera implementation; urls[1..3] stay empty.

  orbvi_sdk::Client client = orbvi_sdk::make_rtsp_client(config, output_format);

  struct Stats {
    std::uint64_t frames{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t stride{0U};
    orbvi_sdk::PixelFormat format{orbvi_sdk::PixelFormat::unknown};
  };

  std::mutex mutex;
  std::condition_variable ready;
  Stats stats;
  bool complete = false;

  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_frame = [&](const orbvi_sdk::FrameView& view) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++stats.frames;
      stats.width = view.width;
      stats.height = view.height;
      stats.stride = view.stride;
      stats.format = view.pixel_format;
      complete = stats.frames >= max_frames;
    }
    ready.notify_all();
  };

  auto subscription = client.subscribe(
      orbvi_sdk::SubscribeOptions{16U, orbvi_sdk::DropPolicy::drop_oldest},
      std::move(callbacks));
  if (!subscription.ok()) {
    std::cerr << "subscribe failed: " << subscription.status().to_string()
              << '\n';
    return 3;
  }

  if (const orbvi_sdk::Status status = client.open(); !status.ok()) {
    std::cerr << "open failed: " << status.to_string() << '\n';
    return 4;
  }
  if (const orbvi_sdk::Status status = client.start(); !status.ok()) {
    std::cerr << "start failed: " << status.to_string() << '\n';
    static_cast<void>(client.close());
    return 5;
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    complete = ready.wait_for(lock, std::chrono::seconds{timeout_seconds},
                              [&]() { return complete; });
  }

  // Cancel callbacks before stopping the runtime so the borrowed FrameView
  // cannot outlive the callback and no new frame is delivered during teardown.
  subscription.value().cancel();
  static_cast<void>(client.stop());
  static_cast<void>(client.close());

  std::cout << "camera_index=0 frames=" << stats.frames << " width=" << stats.width
            << " height=" << stats.height << " stride=" << stats.stride
            << " format=" << orbvi_sdk::to_string(stats.format) << '\n';
  return complete ? 0 : 6;
}
