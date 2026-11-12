#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

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

orbvi_sdk::FakeConfig make_fast_config(std::uint64_t frames) {
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
    FakeConfig callback_config = make_fast_config(100000U);
    callback_config.emit_interval_us = 1000U;
    Client client = make_fake_client(callback_config);
    auto handle = std::make_shared<SubscriptionHandle>();
    std::promise<void> release_callback;
    std::shared_future<void> callback_gate = release_callback.get_future().share();
    std::atomic<bool> callback_entered{false};
    std::atomic<bool> diagnostics_ok{false};
    std::atomic<bool> open_busy{false};
    std::atomic<bool> start_busy{false};
    std::atomic<bool> stop_busy{false};
    std::atomic<bool> close_busy{false};
    std::atomic<bool> subscribe_busy{false};

    CallbackSet callbacks;
    callbacks.on_frame = [&](const FrameView&) {
      const DiagnosticsSnapshot snapshot = client.diagnostics();
      diagnostics_ok = snapshot.state == ClientState::open ||
                       snapshot.state == ClientState::started;
      open_busy = client.open().code() == StatusCode::busy;
      start_busy = client.start().code() == StatusCode::busy;
      stop_busy = client.stop().code() == StatusCode::busy;
      close_busy = client.close().code() == StatusCode::busy;
      subscribe_busy = client.subscribe(SubscribeOptions{}, CallbackSet{}).status().code() ==
                       StatusCode::busy;
      handle->cancel();
      callback_entered = true;
      callback_gate.wait();
    };

    auto subscription = client.subscribe(
        SubscribeOptions{4U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());
    *handle = std::move(subscription.value());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&callback_entered]() { return callback_entered.load(); }, 2s));
    CHECK_TRUE(client.stop().ok());
    release_callback.set_value();
    CHECK_TRUE(client.close().ok());

    CHECK_TRUE(diagnostics_ok.load());
    CHECK_TRUE(open_busy.load());
    CHECK_TRUE(start_busy.load());
    CHECK_TRUE(stop_busy.load());
    CHECK_TRUE(close_busy.load());
    CHECK_TRUE(subscribe_busy.load());
    CHECK_FALSE(handle->valid());
  }

  {
    Client client = make_fake_client(make_fast_config(100000U));
    std::atomic<std::uint64_t> callbacks{0U};
    CallbackSet callback_set;
    callback_set.on_frame = [&callbacks](const FrameView&) {
      callbacks.fetch_add(1U);
    };
    auto subscription = client.subscribe(
        SubscribeOptions{64U, DropPolicy::drop_oldest}, callback_set);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until([&callbacks]() { return callbacks.load() > 0U; }, 2s));

    std::atomic<bool> start_workers{false};
    std::atomic<StatusCode> stop_code{StatusCode::internal_error};
    std::atomic<StatusCode> close_code{StatusCode::internal_error};
    std::vector<std::thread> workers;
    for (int index = 0; index < 4; ++index) {
      workers.emplace_back([&client, &start_workers]() {
        while (!start_workers.load()) {
          std::this_thread::yield();
        }
        for (int iteration = 0; iteration < 1000; ++iteration) {
          const DiagnosticsSnapshot snapshot = client.diagnostics();
          (void)snapshot;
        }
      });
    }
    workers.emplace_back([&client, &start_workers, &stop_code]() {
      while (!start_workers.load()) {
        std::this_thread::yield();
      }
      stop_code = client.stop().code();
    });
    workers.emplace_back([&client, &start_workers, &close_code]() {
      while (!start_workers.load()) {
        std::this_thread::yield();
      }
      close_code = client.close().code();
    });

    start_workers = true;
    for (auto& worker : workers) {
      worker.join();
    }

    CHECK_EQ(stop_code.load(), StatusCode::ok);
    CHECK_EQ(close_code.load(), StatusCode::ok);
    CHECK_EQ(client.state(), ClientState::closed);
    CHECK_TRUE(callbacks.load() > 0U);
  }

  {
    Client client = make_fake_client(make_fast_config(10000U));
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first_called{false};
    CallbackSet callbacks;
    callbacks.on_frame = [&first_called, gate](const FrameView&) {
      if (!first_called.exchange(true)) {
        gate.wait();
      }
    };
    auto subscription = client.subscribe(
        SubscribeOptions{1U, DropPolicy::block}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&client]() { return client.diagnostics().frames_received >= 3U; },
        3s));

    auto stop_future = std::async(std::launch::async,
                                  [&client]() { return client.stop(); });
    const bool stop_ready =
        stop_future.wait_for(2s) == std::future_status::ready;
    release.set_value();
    const Status stop_status = stop_future.get();
    CHECK_TRUE(stop_ready);
    CHECK_TRUE(stop_status.ok());
    CHECK_TRUE(client.close().ok());
  }

  {
    Client client = make_fake_client(make_fast_config(10000U));
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first_called{false};
    CallbackSet callbacks;
    callbacks.on_frame = [&first_called, gate](const FrameView&) {
      if (!first_called.exchange(true)) {
        gate.wait();
      }
    };
    auto subscription = client.subscribe(
        SubscribeOptions{1U, DropPolicy::block}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&client]() { return client.diagnostics().frames_received >= 3U; },
        3s));

    subscription.value().cancel();
    release.set_value();
    CHECK_TRUE(wait_until(
        [&subscription]() { return subscription.value().stats().cancelled; },
        2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
  }

  {
    Client client = make_fake_client(make_fast_config(1U));
    std::atomic<int> callbacks{0};
    CallbackSet callback_set;
    callback_set.on_frame = [&callbacks](const FrameView&) { ++callbacks; };
    auto subscription = client.subscribe(
        SubscribeOptions{4U, DropPolicy::drop_oldest}, callback_set);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until([&callbacks]() { return callbacks.load() >= 1; }, 2s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    const int callbacks_after_close = callbacks.load();
    std::this_thread::sleep_for(20ms);
    CHECK_EQ(callbacks.load(), callbacks_after_close);
  }

  return orbvi_test::finish();
}
