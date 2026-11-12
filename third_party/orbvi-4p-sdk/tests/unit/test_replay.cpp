#include <atomic>
#include <chrono>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/replay.hpp"
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

}  // namespace

int main() {
  using namespace orbvi_sdk;
  using namespace std::chrono_literals;

  ReplayConfig config;
  config.path = ORBVI_REPLAY_FIXTURE;
  Client client = make_replay_client(config);
  CHECK_EQ(client.enumerate().value().size(), 1U);

  std::atomic<int> frames{0};
  std::atomic<int> imu{0};
  std::atomic<int> syncs{0};
  CallbackSet callbacks;
  callbacks.on_frame = [&frames](const FrameView& view) {
    CHECK_TRUE(view.valid());
    ++frames;
  };
  callbacks.on_imu = [&imu](const ImuEvent&) { ++imu; };
  callbacks.on_sync = [&syncs](const FrameSync&) { ++syncs; };
  auto subscription = client.subscribe(
      SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
  CHECK_TRUE(subscription.ok());
  CHECK_TRUE(client.open().ok());
  CHECK_TRUE(client.start().ok());
  CHECK_TRUE(subscription.value().wait_for_delivery(4U, 2s));
  CHECK_TRUE(client.stop().ok());
  CHECK_TRUE(client.close().ok());

  const SubscriptionStats stats = subscription.value().stats();
  CHECK_EQ(stats.delivered, 4U);
  CHECK_EQ(stats.failed, 0U);
  CHECK_TRUE(frames >= 2);
  CHECK_TRUE(imu >= 1);
  CHECK_TRUE(syncs >= 1);
  {
    ReplayConfig eof_config;
    eof_config.path = ORBVI_REPLAY_FIXTURE;
    eof_config.stop_at_end = true;
    Client eof_client = make_replay_client(eof_config);

    std::atomic<int> frames{0};
    std::atomic<int> imu{0};
    std::atomic<int> syncs{0};
    CallbackSet callbacks;
    callbacks.on_frame = [&frames](const FrameView&) { ++frames; };
    callbacks.on_imu = [&imu](const ImuEvent&) { ++imu; };
    callbacks.on_sync = [&syncs](const FrameSync&) { ++syncs; };
    auto subscription = eof_client.subscribe(
        SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());

    CHECK_TRUE(eof_client.open().ok());
    CHECK_TRUE(eof_client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(4U, 2s));
    CHECK_TRUE(wait_until(
        [&eof_client]() { return eof_client.state() == ClientState::stopped; },
        2s));
    CHECK_EQ(eof_client.state(), ClientState::stopped);

    CHECK_TRUE(eof_client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(8U, 2s));
    CHECK_TRUE(wait_until(
        [&eof_client]() { return eof_client.state() == ClientState::stopped; },
        2s));
    CHECK_TRUE(eof_client.stop().ok());
    CHECK_TRUE(eof_client.close().ok());

    CHECK_EQ(frames.load(), 4);
    CHECK_EQ(imu.load(), 2);
    CHECK_EQ(syncs.load(), 2);
    const int callbacks_after_close = frames.load() + imu.load() + syncs.load();
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(frames.load() + imu.load() + syncs.load(), callbacks_after_close);
  }

  {
    ReplayConfig manual_stop_config;
    manual_stop_config.path = ORBVI_REPLAY_FIXTURE;
    manual_stop_config.stop_at_end = false;
    Client manual_stop_client = make_replay_client(manual_stop_config);

    std::atomic<int> frames{0};
    std::atomic<int> imu{0};
    std::atomic<int> syncs{0};
    CallbackSet callbacks;
    callbacks.on_frame = [&frames](const FrameView&) { ++frames; };
    callbacks.on_imu = [&imu](const ImuEvent&) { ++imu; };
    callbacks.on_sync = [&syncs](const FrameSync&) { ++syncs; };
    auto subscription = manual_stop_client.subscribe(
        SubscribeOptions{16U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());

    CHECK_TRUE(manual_stop_client.open().ok());
    CHECK_TRUE(manual_stop_client.start().ok());
    CHECK_TRUE(subscription.value().wait_for_delivery(4U, 2s));
    CHECK_TRUE(wait_until(
        [&frames, &imu, &syncs]() {
          return frames.load() == 2 && imu.load() == 1 && syncs.load() == 1;
        },
        2s));
    CHECK_EQ(manual_stop_client.state(), ClientState::started);
    CHECK_TRUE(manual_stop_client.stop().ok());
    CHECK_EQ(manual_stop_client.state(), ClientState::stopped);
    CHECK_TRUE(manual_stop_client.close().ok());

    const int callbacks_after_close = frames.load() + imu.load() + syncs.load();
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(frames.load() + imu.load() + syncs.load(), callbacks_after_close);
  }

  return orbvi_test::finish();
}