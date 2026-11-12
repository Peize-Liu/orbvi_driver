#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "test_support.hpp"

namespace {

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

orbvi_sdk::FakeConfig make_frame_config(std::uint64_t frames) {
  orbvi_sdk::FakeConfig config = orbvi_sdk::make_default_fake_config(1U);
  config.frames_per_camera = frames;
  config.imu_samples = 0U;
  config.emit_frame_sync = false;
  config.emit_interval_us = 0U;
  return config;
}

}  // namespace

int main() {
  using namespace orbvi_sdk;
  using namespace std::chrono_literals;

  {
    Client client = make_fake_client(make_frame_config(4U));
    std::atomic<int> first_frames{0};
    std::atomic<int> second_frames{0};
    CallbackSet first_callbacks;
    first_callbacks.on_frame = [&first_frames](const FrameView&) {
      ++first_frames;
    };
    CallbackSet second_callbacks;
    second_callbacks.on_frame = [&second_frames](const FrameView&) {
      ++second_frames;
    };

    auto first = client.subscribe(
        SubscribeOptions{8U, DropPolicy::drop_oldest}, first_callbacks);
    auto second = client.subscribe(
        SubscribeOptions{8U, DropPolicy::drop_oldest}, second_callbacks);
    CHECK_TRUE(first.ok());
    CHECK_TRUE(second.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&first, &second]() {
          return first.value().stats().delivered >= 4U &&
                 second.value().stats().delivered >= 4U;
        },
        2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.frames_received, 4U);
    CHECK_EQ(diagnostics.frames_delivered, 8U);
    CHECK_EQ(diagnostics.frames_dropped, 0U);
    CHECK_EQ(diagnostics.callback_failures, 0U);
  }

  for (const DropPolicy policy : {DropPolicy::drop_oldest, DropPolicy::drop_newest}) {
    Client client = make_fake_client(make_frame_config(500U));
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first_called{false};
    CallbackSet callbacks;
    callbacks.on_frame = [&first_called, gate](const FrameView&) {
      if (!first_called.exchange(true)) {
        gate.wait();
      }
    };

    auto subscription =
        client.subscribe(SubscribeOptions{1U, policy}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&first_called]() { return first_called.load(); }, 3s));
    CHECK_TRUE(wait_until(
        [&client]() { return client.diagnostics().frames_dropped > 0U; },
        3s));
    release.set_value();
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    const SubscriptionStats stats = subscription.value().stats();
    CHECK_TRUE(diagnostics.frames_dropped > 0U);
    CHECK_EQ(stats.dropped, diagnostics.frames_dropped);
    CHECK_TRUE(stats.delivered >= 1U);
  }

  {
    Client client = make_fake_client(make_frame_config(3U));
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {
      throw std::runtime_error("callback failure");
    };
    auto subscription = client.subscribe(
        SubscribeOptions{32U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&client]() { return client.diagnostics().callback_failures >= 3U; },
        3s));
    const DiagnosticsSnapshot diagnostics_before_stop = client.diagnostics();
    CHECK_EQ(diagnostics_before_stop.callback_failures, 3U);
    CHECK_TRUE(diagnostics_before_stop.last_error.find("callback failure") !=
               std::string::npos);
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.callback_failures, 3U);
    CHECK_EQ(subscription.value().stats().failed, 3U);
    CHECK_EQ(subscription.value().stats().delivered, 0U);
  }

  {
    FakeConfig config = make_default_fake_config(1U);
    config.frames_per_camera = 2U;
    config.imu_samples = 5U;
    config.emit_frame_sync = true;
    config.emit_interval_us = 0U;
    Client client = make_fake_client(config);

    std::atomic<int> imu_samples{0};
    CallbackSet callbacks;
    callbacks.on_imu = [&imu_samples](const ImuEvent&) { ++imu_samples; };
    auto subscription = client.subscribe(
        SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&client, &imu_samples]() {
          const DiagnosticsSnapshot snapshot = client.diagnostics();
          return imu_samples.load() >= 5 &&
                 snapshot.frames_received >= 2U &&
                 snapshot.imu_samples_received >= 5U &&
                 snapshot.sync_messages_received >= 2U;
        },
        2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(diagnostics.frames_received, 2U);
    CHECK_EQ(diagnostics.frames_delivered, 0U);
    CHECK_EQ(diagnostics.imu_samples_received, 5U);
    CHECK_EQ(diagnostics.sync_messages_received, 2U);
    CHECK_EQ(subscription.value().stats().delivered, 5U);
    CHECK_EQ(subscription.value().stats().queued, 0U);
  }

  return orbvi_test::finish();
}
