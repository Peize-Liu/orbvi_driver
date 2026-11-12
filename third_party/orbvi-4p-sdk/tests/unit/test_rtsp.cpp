#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/rtsp.hpp"
#include "orbvi_sdk/transport.hpp"
#include "test_support.hpp"

namespace {

orbvi_sdk::RtspConfig make_single_camera_config() {
  orbvi_sdk::RtspConfig config;
  config.urls[0] = "rtsp://127.0.0.1:8554/camera0";
  return config;
}

void expect_invalid_argument(const orbvi_sdk::RtspConfig& config,
                             const char* expected_message) {
  orbvi_sdk::Client client = orbvi_sdk::make_rtsp_client(config);
  const orbvi_sdk::Status status = client.open();
  CHECK_EQ(status.code(), orbvi_sdk::StatusCode::invalid_argument);
  CHECK_TRUE(status.message().find(expected_message) != std::string::npos);
  CHECK_EQ(client.state(), orbvi_sdk::ClientState::closed);
}

}  // namespace

int main() {
  using namespace orbvi_sdk;

  RtspConfig config = make_single_camera_config();
  CHECK_EQ(config.urls.size(), 4U);
  CHECK_TRUE(config.urls[1].empty());
  CHECK_TRUE(config.urls[2].empty());
  CHECK_TRUE(config.urls[3].empty());
  CHECK_EQ(config.connect_timeout.count(), 5000);
  CHECK_EQ(config.read_timeout.count(), 5000);
  CHECK_EQ(config.reconnect_delay.count(), 1000);
  CHECK_EQ(config.max_reconnect_attempts, 5U);
  CHECK_TRUE(std::string{to_string(TransportMode::rtsp)} == "rtsp");
  CHECK_EQ(static_cast<std::uint32_t>(PixelFormat::raw16), 5U);
  CHECK_EQ(static_cast<std::uint32_t>(PixelFormat::bgr8), 6U);
  CHECK_TRUE(std::string{to_string(PixelFormat::bgr8)} == "bgr8");

  Client client = make_rtsp_client(config);
  CHECK_EQ(client.options().runtime_mode, RuntimeMode::rtsp);
  CHECK_EQ(client.options().transport_mode, TransportMode::rtsp);
  CHECK_EQ(client.state(), ClientState::closed);
  const auto stored_config =
      std::dynamic_pointer_cast<const RtspConfig>(client.options().runtime_config);
  CHECK_TRUE(stored_config != nullptr);
  if (stored_config) {
    CHECK_EQ(stored_config->urls[0], config.urls[0]);
    CHECK_TRUE(stored_config->urls[1].empty());
    CHECK_TRUE(stored_config->urls[2].empty());
    CHECK_TRUE(stored_config->urls[3].empty());
  }

  auto devices = client.enumerate();
  CHECK_TRUE(devices.ok());
  CHECK_EQ(devices.value().size(), 1U);
  if (devices.ok() && !devices.value().empty()) {
    CHECK_EQ(devices.value()[0].id, std::string{"rtsp-camera-0"});
    CHECK_TRUE(devices.value()[0].serial.empty());
  }

  {
    Client color = make_rtsp_client(config, PixelFormat::bgr8);
    const auto color_devices = color.enumerate();
    CHECK_TRUE(color_devices.ok());
    CHECK_EQ(color_devices.value().size(), 1U);
    CHECK_EQ(color_devices.value()[0].id, std::string{"rtsp-camera-0"});
  }
  {
    Client gray = make_rtsp_client(config, PixelFormat::gray8);
    const auto gray_devices = gray.enumerate();
    CHECK_TRUE(gray_devices.ok());
    CHECK_EQ(gray_devices.value().size(), 1U);
  }
  {
    Client invalid_format = make_rtsp_client(config, PixelFormat::nv12);
    const Status status = invalid_format.open();
    CHECK_EQ(status.code(), StatusCode::invalid_argument);
    CHECK_TRUE(status.message().find("output_format") != std::string::npos);
  }

  {
    RtspConfig four_urls = config;
    four_urls.urls[1] = "rtsp://127.0.0.1:8554/camera1";
    four_urls.urls[2] = "rtsp://127.0.0.1:8554/camera2";
    four_urls.urls[3] = "rtsp://127.0.0.1:8554/camera3";
    Client multi = make_rtsp_client(four_urls);
    const auto stored =
        std::dynamic_pointer_cast<const RtspConfig>(multi.options().runtime_config);
    CHECK_TRUE(stored != nullptr);
    if (stored) {
      for (std::size_t index = 0U; index < stored->urls.size(); ++index) {
        CHECK_EQ(stored->urls[index], four_urls.urls[index]);
      }
    }

    auto four_devices = multi.enumerate();
    CHECK_TRUE(four_devices.ok());
    CHECK_EQ(four_devices.value().size(), 4U);
    for (std::size_t index = 0; index < 4U; ++index) {
      CHECK_EQ(four_devices.value()[index].id,
               std::string{"rtsp-camera-"} + std::to_string(index));
    }
    const Status status = multi.open();
    CHECK_TRUE(status.code() == StatusCode::io_error ||
               status.code() == StatusCode::timeout);
    CHECK_TRUE(status.message().find("camera_index=0") != std::string::npos);
    CHECK_EQ(multi.state(), ClientState::failed);
    CHECK_TRUE(multi.close().ok());
    CHECK_EQ(multi.state(), ClientState::closed);
  }

  {
    RtspConfig invalid = config;
    invalid.urls[0] = "http://127.0.0.1/camera0";
    expect_invalid_argument(invalid, "rtsp.urls[0]");
  }
  {
    RtspConfig invalid = config;
    invalid.urls[1] = "http://127.0.0.1/camera1";
    expect_invalid_argument(invalid, "rtsp.urls[1]");
  }
  {
    RtspConfig invalid = config;
    invalid.connect_timeout = std::chrono::milliseconds{0};
    expect_invalid_argument(invalid, "rtsp.connect_timeout");
  }
  {
    RtspConfig invalid = config;
    invalid.read_timeout = std::chrono::milliseconds{0};
    expect_invalid_argument(invalid, "rtsp.read_timeout");
  }
  {
    RtspConfig invalid = config;
    invalid.reconnect_delay = std::chrono::milliseconds{-1};
    expect_invalid_argument(invalid, "rtsp.reconnect_delay");
  }
  {
    ClientOptions options;
    options.runtime_mode = RuntimeMode::rtsp;
    options.transport_mode = TransportMode::rtsp;
    Client missing_config_client{std::move(options)};
    const Status missing_config_status = missing_config_client.open();
    CHECK_EQ(missing_config_status.code(), StatusCode::invalid_argument);
    CHECK_TRUE(missing_config_status.message().find("RtspConfig") !=
               std::string::npos);
    CHECK_EQ(missing_config_client.state(), ClientState::closed);
  }

  {
    RtspConfig unreachable = config;
    unreachable.urls[0] = "rtsp://127.0.0.1:1/camera0";
    unreachable.connect_timeout = std::chrono::milliseconds{200};
    unreachable.read_timeout = std::chrono::milliseconds{200};

    Client failed_client = make_rtsp_client(unreachable);
    std::atomic<int> callbacks{0};
    CallbackSet callback_set;
    callback_set.on_frame = [&callbacks](const FrameView&) { ++callbacks; };
    auto subscription = failed_client.subscribe(
        SubscribeOptions{4U, DropPolicy::drop_oldest}, std::move(callback_set));
    CHECK_TRUE(subscription.ok());

    const Status open_status = failed_client.open();
    CHECK_TRUE(open_status.code() == StatusCode::io_error ||
               open_status.code() == StatusCode::timeout);
    CHECK_TRUE(open_status.message().find("cannot open RTSP URL") !=
               std::string::npos);
    CHECK_EQ(failed_client.state(), ClientState::failed);
    CHECK_TRUE(failed_client.diagnostics().last_error.find(
                   "cannot open RTSP URL") != std::string::npos);
    CHECK_EQ(callbacks.load(), 0);

    CHECK_TRUE(failed_client.close().ok());
    CHECK_EQ(failed_client.state(), ClientState::closed);
    CHECK_TRUE(failed_client.diagnostics().last_error.empty());
    CHECK_EQ(callbacks.load(), 0);
    if (subscription.ok()) {
      CHECK_EQ(subscription.value().stats().delivered, 0U);
    }
  }

  return orbvi_test::finish();
}
