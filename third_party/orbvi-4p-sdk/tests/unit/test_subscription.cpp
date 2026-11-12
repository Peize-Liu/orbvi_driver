#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
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

orbvi_sdk::FakeConfig make_fast_config(std::uint64_t frames_per_camera) {
  orbvi_sdk::FakeConfig config = orbvi_sdk::make_default_fake_config(1U);
  config.frames_per_camera = frames_per_camera;
  config.emit_interval_us = 0U;
  return config;
}

}  // namespace

int main() {
  using namespace orbvi_sdk;
  using namespace std::chrono_literals;

  {
    FakeConfig config = make_default_fake_config(1U);
    config.frames_per_camera = 1000U;
    config.emit_interval_us = 1000U;
    Client client = make_fake_client(config);

    std::atomic<int> frames{0};
    CallbackSet callbacks;
    callbacks.on_frame = [&frames](const FrameView&) { ++frames; };
    auto subscription = client.subscribe(
        SubscribeOptions{4U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until([&frames]() { return frames.load() >= 1; }, 2s));

    subscription.value().cancel();
    subscription.value().cancel();
    CHECK_FALSE(subscription.value().valid());
    CHECK_TRUE(subscription.value().stats().cancelled);
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
  }

  {
    Client client = make_fake_client(make_fast_config(500U));
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
        SubscribeOptions{1U, DropPolicy::drop_newest}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&subscription]() { return subscription.value().stats().dropped > 0U; },
        3s));
    release.set_value();
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    const SubscriptionStats stats = subscription.value().stats();
    CHECK_TRUE(stats.dropped > 0U);
    CHECK_TRUE(stats.delivered >= 1U);
  }

  {
    Client client = make_fake_client(make_fast_config(500U));
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
        SubscribeOptions{1U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&subscription]() { return subscription.value().stats().dropped > 0U; },
        3s));
    release.set_value();
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    const SubscriptionStats stats = subscription.value().stats();
    CHECK_TRUE(stats.dropped > 0U);
    CHECK_TRUE(stats.delivered >= 1U);
  }

  {
    FakeConfig config = make_fast_config(500U);
    ClientOptions options;
    options.runtime_mode = RuntimeMode::fake;
    options.transport_mode = TransportMode::fake;
    options.selector.select_any = true;
    options.runtime_config = std::make_shared<FakeConfig>(config);
    options.callback_queue_capacity = 1U;
    options.default_drop_policy = DropPolicy::drop_newest;
    Client client{options};

    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first_called{false};
    CallbackSet callbacks;
    callbacks.on_frame = [&first_called, gate](const FrameView&) {
      if (!first_called.exchange(true)) {
        gate.wait();
      }
    };
    auto subscription = client.subscribe(SubscribeOptions{}, callbacks);
    CHECK_TRUE(subscription.ok());
    CHECK_EQ(subscription.value().stats().queue_capacity, 1U);
    CHECK_TRUE(client.open().ok());
    CHECK_TRUE(client.start().ok());
    CHECK_TRUE(wait_until(
        [&subscription]() { return subscription.value().stats().dropped > 0U; },
        3s));
    release.set_value();
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    CHECK_TRUE(subscription.value().stats().dropped > 0U);
  }

  {
    Client client = make_fake_client(make_fast_config(1U));
    CallbackSet callbacks;
    callbacks.on_frame = [](const FrameView&) {};
    auto default_subscription = client.subscribe(SubscribeOptions{}, callbacks);
    CHECK_TRUE(default_subscription.ok());
    CHECK_EQ(default_subscription.value().stats().queue_capacity, 64U);
    default_subscription.value().cancel();

    auto override_subscription =
        client.subscribe(SubscribeOptions{3U, DropPolicy::drop_oldest}, callbacks);
    CHECK_TRUE(override_subscription.ok());
    CHECK_EQ(override_subscription.value().stats().queue_capacity, 3U);
    override_subscription.value().cancel();
    CHECK_TRUE(client.close().ok());
  }

  {
    Client client = make_fake_client(make_fast_config(3U));
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
        [&subscription]() { return subscription.value().stats().failed >= 3U; },
        3s));
    CHECK_TRUE(client.stop().ok());
    CHECK_TRUE(client.close().ok());
    const SubscriptionStats stats = subscription.value().stats();
    CHECK_EQ(stats.failed, 3U);
    CHECK_EQ(stats.delivered, 0U);
  }

  return orbvi_test::finish();
}