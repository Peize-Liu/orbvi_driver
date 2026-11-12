#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/replay.hpp"
#include "test_support.hpp"

namespace {

std::filesystem::path unique_replay_path(const std::string& prefix) {
  static std::atomic<unsigned> counter{0U};
  const unsigned value = counter.fetch_add(1U);
  return std::filesystem::temp_directory_path() /
         (prefix + std::to_string(value) + ".replay");
}

class TempReplay {
 public:
  explicit TempReplay(const std::string& contents)
      : path_(unique_replay_path("orbvi_sdk_replay_error_")) {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output << contents;
  }

  ~TempReplay() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

orbvi_sdk::StatusCode open_replay_status(const std::string& contents) {
  TempReplay fixture(contents);
  orbvi_sdk::ReplayConfig config;
  config.path = fixture.path().string();
  orbvi_sdk::Client client = orbvi_sdk::make_replay_client(config);
  return client.open().code();
}

}  // namespace

int main() {
  using namespace orbvi_sdk;

  const std::string header = "orbvi-replay-v1\n";
  const std::string contract =
      "product=4p-x5 layout=grid_2x2_1280x1088_ports_v1 protocol=0.1\n";

  {
    ReplayConfig config;
    Client client = make_replay_client(config);
    CHECK_EQ(client.open().code(), StatusCode::invalid_argument);
  }

  {
    ReplayConfig config;
    config.path = unique_replay_path("orbvi_sdk_missing_replay_").string();
    Client client = make_replay_client(config);
    auto devices = client.enumerate();
    CHECK_TRUE(devices.ok());
    CHECK_EQ(devices.value().size(), 1U);
    CHECK_EQ(client.open().code(), StatusCode::io_error);
  }

  CHECK_EQ(open_replay_status(""), StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status("not-a-replay-header\n"),
           StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(header), StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(header + "frame camera=0\n"),
           StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(
               header +
               "product=4p-x5 layout=grid_2x2_1280x1088_ports_v1 protocol=9.9\n"),
           StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(header + contract), StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(
               header + contract +
               "frame camera=0 sequence=abc group_id=1 timestamp_ns=1 "
               "sensor_timestamp_ns=1 width=8 height=4 stride=8 format=gray8 "
               "crc=1 data=0102\n"),
           StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(
               header + contract +
               "frame camera=0 sequence=1 group_id=1 timestamp_ns=1 "
               "sensor_timestamp_ns=1 width=8 height=4 stride=8 format=gray8 "
               "crc=1 data=0g\n"),
           StatusCode::corrupt_data);
  CHECK_EQ(open_replay_status(
               header + contract +
               "imu sequence=1 timestamp_ns=1 sensor_timestamp_ns=1 "
               "generation=0 accel_x=not_a_double\n"),
           StatusCode::corrupt_data);

  return orbvi_test::finish();
}