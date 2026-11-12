#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/temperature_stream.hpp"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_stop_signal(int) {
  stop_requested = 1;
}

struct Options {
  orbvi_sdk::TemperatureStreamConfig config;
  std::chrono::milliseconds timeout{15000};
  bool timeout_set{false};
};

void print_usage() {
  std::cerr
      << "usage: orbvi_sdk_temperature_stream_demo [options]\n"
      << "options:\n"
      << "  --bind-address IPv4          default 0.0.0.0\n"
      << "  --port P                     default 5620\n"
      << "  --samples N                  0=until SIGINT/SIGTERM, positive=finite\n"
      << "  --timeout-ms N               overall wait; 0 disables\n"
      << "                               default: 0 continuous / 15000 finite\n"
      << "note: monotonic_ns is board metadata; host_unix_ns is the ROS stamp source\n";
}

bool parse_unsigned(const char* text, std::uint64_t& value,
                    bool allow_zero) {
  if (text == nullptr) {
    return false;
  }
  try {
    const std::string input{text};
    std::size_t used = 0U;
    const unsigned long long parsed = std::stoull(input, &used, 0);
    if (used != input.size() || (!allow_zero && parsed == 0ULL)) {
      return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    const char* value = index + 1 < argc ? argv[++index] : nullptr;
    std::uint64_t parsed = 0U;
    if (argument == "--help") {
      print_usage();
      std::exit(0);
    }
    if (argument == "--bind-address") {
      if (value == nullptr) return false;
      options.config.bind_address = value;
    } else if (argument == "--port") {
      if (!parse_unsigned(value, parsed, false) || parsed > 65535ULL) {
        return false;
      }
      options.config.port = static_cast<std::uint16_t>(parsed);
    } else if (argument == "--samples") {
      if (!parse_unsigned(value, parsed, true) || parsed > 100000ULL) {
        return false;
      }
      options.config.samples = static_cast<std::uint32_t>(parsed);
    } else if (argument == "--timeout-ms") {
      if (!parse_unsigned(value, parsed, true) || parsed > 600000ULL) {
        return false;
      }
      options.timeout = std::chrono::milliseconds{parsed};
      options.timeout_set = true;
    } else {
      return false;
    }
  }
  if (!options.timeout_set && options.config.samples == 0U) {
    options.timeout = std::chrono::milliseconds{0};
  }
  return !options.config.bind_address.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }

  std::signal(SIGINT, handle_stop_signal);
  std::signal(SIGTERM, handle_stop_signal);

  orbvi_sdk::Client client =
      orbvi_sdk::make_temperature_stream_client(options.config);
  orbvi_sdk::CallbackSet callbacks;
  callbacks.on_temperature = [&](const orbvi_sdk::TemperatureSample& sample) {
    const std::int64_t host_unix_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    std::cout << "OCPU sequence=" << sample.sequence
              << " celsius=" << std::fixed << std::setprecision(3)
              << sample.celsius << " board_monotonic_ns=" << sample.monotonic_ns
              << " host_unix_ns=" << host_unix_ns << '\n';
  };

  auto subscription = client.subscribe({}, std::move(callbacks));
  if (!subscription.ok()) {
    std::cerr << "subscribe failed: " << subscription.status().to_string() << '\n';
    return 3;
  }
  if (const orbvi_sdk::Status status = client.open(); !status.ok()) {
    std::cerr << "open failed: " << status.to_string() << '\n';
    return 4;
  }
  if (const orbvi_sdk::Status status = client.start(); !status.ok()) {
    std::cerr << "start failed: " << status.to_string() << '\n';
    static_cast<void>(client.close());
    return 5;
  }

  const auto deadline = options.timeout.count() == 0
                            ? std::chrono::steady_clock::time_point::max()
                            : std::chrono::steady_clock::now() + options.timeout;
  while (!stop_requested && std::chrono::steady_clock::now() < deadline) {
    if (options.config.samples != 0U &&
        subscription.value().wait_for_delivery(
            options.config.samples, std::chrono::milliseconds{50})) {
      break;
    }
    const orbvi_sdk::ClientState state = client.state();
    if (state == orbvi_sdk::ClientState::failed) {
      break;
    }
    if (state == orbvi_sdk::ClientState::stopped) {
      // The runtime may finish before the subscription has drained its queue.
      const auto stats = subscription.value().stats();
      const auto received = client.diagnostics().temperature_samples_received;
      if (stats.delivered + stats.failed + stats.dropped >= received) {
        break;
      }
    }
    if (options.config.samples == 0U) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }

  const orbvi_sdk::Status stop_status = client.stop();
  const orbvi_sdk::DiagnosticsSnapshot diagnostics = client.diagnostics();
  const orbvi_sdk::Status close_status = client.close();

  std::cout << "TEMPERATURE_RESULT received="
            << diagnostics.temperature_samples_received
            << " delivered=" << diagnostics.temperature_samples_delivered
            << " dropped=" << diagnostics.temperature_samples_dropped
            << " crc_errors=" << diagnostics.temperature_crc_errors
            << " sequence_gaps=" << diagnostics.temperature_sequence_gaps
            << " parse_errors=" << diagnostics.parse_errors << '\n';

  const bool complete =
      options.config.samples == 0U ||
      diagnostics.temperature_samples_delivered == options.config.samples;
  return complete && stop_status.ok() && close_status.ok() &&
                 diagnostics.temperature_crc_errors == 0U &&
                 diagnostics.parse_errors == 0U &&
                 diagnostics.callback_failures == 0U
             ? 0
             : 1;
}
