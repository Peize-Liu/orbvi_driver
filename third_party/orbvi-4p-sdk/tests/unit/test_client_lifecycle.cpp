#include <utility>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "test_support.hpp"

int main() {
  using namespace orbvi_sdk;

  Client unsupported;
  CHECK_EQ(unsupported.state(), ClientState::closed);
  CHECK_TRUE(unsupported.stop().ok());
  CHECK_EQ(unsupported.state(), ClientState::closed);
  CHECK_EQ(unsupported.open().code(), StatusCode::not_supported);
  CHECK_EQ(unsupported.state(), ClientState::failed);

  auto devices = unsupported.enumerate();
  CHECK_TRUE(devices.ok());
  CHECK_TRUE(devices.value().empty());

  CallbackSet empty_callbacks;
  auto empty_subscription =
      unsupported.subscribe(SubscribeOptions{}, empty_callbacks);
  CHECK_FALSE(empty_subscription.ok());
  CHECK_EQ(empty_subscription.status().code(), StatusCode::invalid_argument);

  CallbackSet frame_callbacks;
  frame_callbacks.on_frame = [](const FrameView&) {};
  auto failed_subscription =
      unsupported.subscribe(SubscribeOptions{}, frame_callbacks);
  CHECK_FALSE(failed_subscription.ok());
  CHECK_EQ(failed_subscription.status().code(), StatusCode::not_open);
  CHECK_TRUE(unsupported.close().ok());
  CHECK_EQ(unsupported.state(), ClientState::closed);

  FakeConfig config = make_default_fake_config(1U);
  config.frames_per_camera = 2U;
  config.imu_samples = 0U;
  config.emit_frame_sync = false;
  config.emit_interval_us = 1000U;

  Client client = make_fake_client(config);
  CHECK_EQ(client.state(), ClientState::closed);
  CHECK_TRUE(client.open().ok());
  CHECK_EQ(client.state(), ClientState::open);
  CHECK_EQ(client.open().code(), StatusCode::already_open);
  CHECK_EQ(client.state(), ClientState::open);
  CHECK_TRUE(client.start().ok());
  CHECK_EQ(client.state(), ClientState::started);
  CHECK_TRUE(client.start().ok());
  CHECK_EQ(client.state(), ClientState::started);
  CHECK_TRUE(client.stop().ok());
  CHECK_EQ(client.state(), ClientState::stopped);
  CHECK_TRUE(client.stop().ok());
  CHECK_EQ(client.state(), ClientState::stopped);
  CHECK_TRUE(client.start().ok());
  CHECK_EQ(client.state(), ClientState::started);
  CHECK_TRUE(client.close().ok());
  CHECK_EQ(client.state(), ClientState::closed);
  CHECK_TRUE(client.close().ok());
  CHECK_EQ(client.state(), ClientState::closed);
  CHECK_TRUE(client.open().ok());
  CHECK_TRUE(client.close().ok());

  Client moved_to = std::move(client);
  CHECK_EQ(client.state(), ClientState::closed);
  CHECK_EQ(client.stop().code(), StatusCode::internal_error);
  CHECK_EQ(client.open().code(), StatusCode::internal_error);
  CHECK_EQ(client.enumerate().status().code(), StatusCode::internal_error);
  auto moved_subscription = client.subscribe(SubscribeOptions{}, frame_callbacks);
  CHECK_FALSE(moved_subscription.ok());
  CHECK_EQ(moved_subscription.status().code(), StatusCode::internal_error);
  CHECK_TRUE(client.close().ok());

  CHECK_TRUE(moved_to.open().ok());
  CHECK_EQ(moved_to.state(), ClientState::open);
  CHECK_TRUE(moved_to.close().ok());

  Client assigned_to;
  Client assigned_from = make_fake_client(config);
  assigned_to = std::move(assigned_from);
  CHECK_EQ(assigned_from.state(), ClientState::closed);
  CHECK_TRUE(assigned_to.open().ok());
  CHECK_TRUE(assigned_to.close().ok());

  {
    Client raii = make_fake_client(config);
    CHECK_TRUE(raii.open().ok());
    CHECK_TRUE(raii.start().ok());
  }

  return orbvi_test::finish();
}
