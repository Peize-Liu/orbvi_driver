#include <atomic>
#include <chrono>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "test_support.hpp"

int main() {
  using namespace orbvi_sdk;
  using namespace std::chrono_literals;

  FakeConfig config = make_default_fake_config(1U);
  config.frames_per_camera = 4U;
  config.emit_interval_us = 50U;
  Client client = make_fake_client(config);

  auto devices = client.enumerate();
  CHECK_TRUE(devices.ok());
  CHECK_EQ(devices.value().size(), 1U);
  CHECK_TRUE(devices.value().front().fake);
  CHECK_TRUE(has_capability(devices.value().front(), DeviceCapability::imu));

  std::atomic<int> frames{0};
  std::atomic<int> imu{0};
  std::atomic<int> syncs{0};
  std::atomic<bool> valid_frames{true};
  CallbackSet callbacks;
  callbacks.on_frame = [&frames, &valid_frames](const FrameView& view) {
    if (!view.valid()) {
      valid_frames = false;
    }
    ++frames;
  };
  callbacks.on_imu = [&imu](const ImuEvent&) { ++imu; };
  callbacks.on_sync = [&syncs](const FrameSync&) { ++syncs; };
  auto subscription = client.subscribe(
      SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
  CHECK_TRUE(subscription.ok());

  CHECK_TRUE(client.open().ok());
  CHECK_TRUE(client.start().ok());
  CHECK_EQ(client.state(), ClientState::started);
  CHECK_TRUE(subscription.value().wait_for_delivery(12U, 2s));
  CHECK_TRUE(client.stop().ok());
  CHECK_EQ(client.state(), ClientState::stopped);
  CHECK_EQ(client.close().code(), StatusCode::ok);
  CHECK_EQ(client.state(), ClientState::closed);

  const SubscriptionStats stats = subscription.value().stats();
  CHECK_TRUE(stats.delivered >= 12U);
  CHECK_EQ(stats.failed, 0U);
  CHECK_TRUE(valid_frames);
  CHECK_TRUE(frames >= 4);
  CHECK_TRUE(imu >= 4);
  CHECK_TRUE(syncs >= 4);
  CHECK_EQ(client.diagnostics().state, ClientState::closed);
  {
    FakeConfig config = make_default_fake_config(1U);
    config.auto_emit = false;
    config.frames_per_camera = 5U;
    config.imu_samples = 5U;
    Client client = make_fake_client(config);

    std::atomic<int> callbacks{0};
    CallbackSet callback_set;
    callback_set.on_frame = [&callbacks](const FrameView&) { ++callbacks; };
    callback_set.on_imu = [&callbacks](const ImuEvent&) { ++callbacks; };
    callback_set.on_sync = [&callbacks](const FrameSync&) { ++callbacks; };
    auto subscription = client.subscribe(
        SubscribeOptions{8U, DropPolicy::drop_oldest}, callback_set);
    CHECK_TRUE(subscription.ok());

    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_EQ(client.state(), ClientState::started);
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(callbacks.load(), 0);
    CHECK_EQ(client.diagnostics().frames_received, 0U);
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(callbacks.load(), 0);
  }

  {
    FakeConfig config = make_default_fake_config(1U);
    config.frames_per_camera = 2U;
    config.imu_samples = 3U;
    config.emit_frame_sync = true;
    config.emit_timing_health = true;
    config.emit_interval_us = 0U;
    Client client = make_fake_client(config);

    std::atomic<int> frames{0};
    std::atomic<int> imu{0};
    std::atomic<int> syncs{0};
    std::atomic<int> imu_with_timing{0};
    std::atomic<int> imu_without_timing{0};
    CallbackSet callbacks;
    callbacks.on_frame = [&frames](const FrameView&) { ++frames; };
    callbacks.on_imu = [&imu, &imu_with_timing,
                        &imu_without_timing](const ImuEvent& event) {
      ++imu;
      if (event.timing.sample_count > 0U) {
        ++imu_with_timing;
      } else {
        ++imu_without_timing;
      }
    };
    callbacks.on_sync = [&syncs](const FrameSync&) { ++syncs; };
    auto subscription = client.subscribe(
        SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());

    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(7U, 2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    const DiagnosticsSnapshot diagnostics = client.diagnostics();
    CHECK_EQ(frames.load(), 2);
    CHECK_EQ(imu.load(), 3);
    CHECK_EQ(syncs.load(), 2);
    CHECK_EQ(imu_with_timing.load(), 3);
    CHECK_EQ(imu_without_timing.load(), 0);
    CHECK_EQ(diagnostics.frames_received, 2U);
    CHECK_EQ(diagnostics.imu_samples_received, 3U);
    CHECK_EQ(diagnostics.sync_messages_received, 2U);
  }

  {
    FakeConfig config = make_default_fake_config(1U);
    config.frames_per_camera = 0U;
    config.imu_samples = 3U;
    config.emit_frame_sync = false;
    config.emit_timing_health = false;
    config.emit_interval_us = 0U;
    Client client = make_fake_client(config);

    std::atomic<int> imu{0};
    std::atomic<int> imu_with_timing{0};
    std::atomic<int> imu_without_timing{0};
    CallbackSet callbacks;
    callbacks.on_imu = [&imu, &imu_with_timing,
                        &imu_without_timing](const ImuEvent& event) {
      ++imu;
      if (event.timing.sample_count > 0U) {
        ++imu_with_timing;
      } else {
        ++imu_without_timing;
      }
    };
    auto subscription = client.subscribe(
        SubscribeOptions{8U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());

    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(3U, 2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());

    CHECK_EQ(imu.load(), 3);
    CHECK_EQ(imu_with_timing.load(), 0);
    CHECK_EQ(imu_without_timing.load(), 3);
    CHECK_EQ(client.diagnostics().imu_samples_received, 3U);
  }

  return orbvi_test::finish();
}