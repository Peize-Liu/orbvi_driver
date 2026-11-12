#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/fake.hpp"
#include "test_support.hpp"

namespace {

struct Observation {
  std::atomic<int> frames{0};
  std::atomic<int> crc_invalid{0};
  std::atomic<int> truncated{0};
  std::atomic<int> time_rollback{0};
  std::atomic<int> generation_change{0};
  std::atomic<int> out_of_order{0};
  std::atomic<int> reconnects{0};
  std::atomic<std::uint64_t> last_sequence{0U};
  std::atomic<std::uint64_t> frames_received{0U};
  std::atomic<std::uint64_t> crc_errors{0U};
  std::atomic<std::int64_t> last_frame_timestamp_ns{0};
};

std::shared_ptr<Observation> run_fault(orbvi_sdk::FakeFault fault,
                                       std::uint64_t expected_events) {
  using namespace orbvi_sdk;
  using namespace std::chrono_literals;

  auto observation = std::make_shared<Observation>();
  FakeConfig config = make_default_fake_config(1U);
  config.frames_per_camera = 5U;
  config.emit_interval_us = 200U;
  config.fault_after_frames = 2U;
  config.fault = fault;

  Client client = make_fake_client(config);
  CallbackSet callbacks;
  callbacks.on_frame = [observation](const FrameView& view) {
    if (!view.crc_valid) {
      ++observation->crc_invalid;
    }
    if (view.size < static_cast<std::size_t>(view.width) * view.height) {
      ++observation->truncated;
    }
    if (view.timestamp_ns < 0) {
      ++observation->time_rollback;
    }
    if (view.generation != 0U) {
      ++observation->generation_change;
    }
    const std::uint64_t previous = observation->last_sequence.load();
    if (previous != 0U && view.sequence <= previous) {
      ++observation->out_of_order;
    }
    observation->last_sequence.store(view.sequence);
    ++observation->frames;
  };
  callbacks.on_sync = [](const FrameSync&) {};
  callbacks.on_imu = [](const ImuEvent&) {};
  callbacks.on_diagnostics = [observation](const DiagnosticsSnapshot& value) {
    observation->reconnects += static_cast<int>(value.reconnects);
  };

  auto subscription = client.subscribe(
      SubscribeOptions{64U, DropPolicy::drop_oldest}, callbacks);
  CHECK_TRUE(subscription.ok());
  CHECK_TRUE(client.open().ok());
  CHECK_TRUE(client.start().ok());
  CHECK_TRUE(subscription.value().wait_for_delivery(expected_events, 3s));

  const DiagnosticsSnapshot diagnostics = client.diagnostics();
  observation->frames_received = diagnostics.frames_received;
  observation->crc_errors = diagnostics.crc_errors;
  observation->last_frame_timestamp_ns = diagnostics.last_frame_timestamp_ns;

  CHECK_TRUE(client.stop().ok());
  CHECK_TRUE(client.close().ok());
  CHECK_EQ(subscription.value().stats().failed, 0U);
  return observation;
}

}  // namespace

int main() {
  using namespace orbvi_sdk;

  {
    const auto observation = run_fault(FakeFault::none, 15U);
    CHECK_EQ(observation->frames.load(), 5);
    CHECK_EQ(observation->frames_received.load(), 5U);
    CHECK_EQ(observation->crc_invalid.load(), 0);
    CHECK_EQ(observation->truncated.load(), 0);
    CHECK_EQ(observation->time_rollback.load(), 0);
    CHECK_EQ(observation->generation_change.load(), 0);
    CHECK_EQ(observation->out_of_order.load(), 0);
    CHECK_EQ(observation->crc_errors.load(), 0U);
    CHECK_EQ(observation->last_frame_timestamp_ns.load(), 5000000);
  }

  {
    const auto observation = run_fault(FakeFault::missing_sequence, 11U);
    CHECK_EQ(observation->frames.load(), 1);
    CHECK_EQ(observation->frames_received.load(), 1U);
    CHECK_EQ(observation->out_of_order.load(), 0);
  }

  {
    const auto observation = run_fault(FakeFault::out_of_order, 15U);
    CHECK_EQ(observation->frames.load(), 5);
    CHECK_TRUE(observation->out_of_order.load() >= 1);
  }

  {
    const auto observation = run_fault(FakeFault::crc_error, 15U);
    CHECK_TRUE(observation->crc_invalid.load() >= 1);
    CHECK_TRUE(observation->crc_errors.load() >= 1U);
  }

  {
    const auto observation = run_fault(FakeFault::truncated_jpeg, 15U);
    CHECK_TRUE(observation->truncated.load() >= 1);
  }

  {
    const auto observation = run_fault(FakeFault::time_rollback, 15U);
    CHECK_TRUE(observation->time_rollback.load() >= 1);
    CHECK_EQ(observation->last_frame_timestamp_ns.load(), -1);
  }

  {
    const auto observation = run_fault(FakeFault::generation_change, 15U);
    CHECK_TRUE(observation->generation_change.load() >= 1);
  }

  {
    const auto observation = run_fault(FakeFault::disconnect_reconnect, 19U);
    CHECK_TRUE(observation->generation_change.load() >= 1);
    CHECK_TRUE(observation->reconnects.load() >= 1);
  }

  return orbvi_test::finish();
}