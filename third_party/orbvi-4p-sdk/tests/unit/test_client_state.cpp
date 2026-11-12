#include <atomic>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "test_support.hpp"

int main() {
  using namespace orbvi_sdk;

  Client client;
  CHECK_EQ(client.state(), ClientState::closed);
  CHECK_TRUE(client.stop().ok());
  CHECK_EQ(client.state(), ClientState::closed);

  CHECK_EQ(client.open().code(), StatusCode::not_supported);
  CHECK_EQ(client.state(), ClientState::failed);

  auto devices = client.enumerate();
  CHECK_TRUE(devices.ok());
  CHECK_TRUE(devices.value().empty());

  std::atomic<int> callbacks{0};
  CallbackSet callback_set;
  callback_set.on_frame = [&callbacks](const FrameView&) { ++callbacks; };
  auto subscription = client.subscribe(SubscribeOptions{}, callback_set);
  CHECK_FALSE(subscription.ok());
  CHECK_EQ(subscription.status().code(), StatusCode::not_open);

  CHECK_TRUE(client.close().ok());
  CHECK_EQ(client.state(), ClientState::closed);

  Client fake_client = make_fake_client(make_default_fake_config(1U));
  auto fake_subscription =
      fake_client.subscribe(SubscribeOptions{}, callback_set);
  CHECK_TRUE(fake_subscription.ok());
  CHECK_TRUE(fake_subscription.value().valid());
  fake_subscription.value().cancel();
  CHECK_TRUE(fake_subscription.value().stats().cancelled);
  CHECK_TRUE(fake_client.close().ok());
  CHECK_EQ(fake_client.state(), ClientState::closed);
  return orbvi_test::finish();
}
