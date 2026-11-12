#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/image_stream.hpp"

namespace {

struct Options {
  orbvi_sdk::ImageStreamConfig config;
  std::string output_dir{"."};
  std::chrono::milliseconds timeout{120000};
};

void print_usage() {
  std::cerr
      << "usage: orbvi_sdk_image_stream_demo --host <ipv4> [options]\n"
      << "options:\n"
      << "  --kind nv12|jpeg             payload kind (default nv12)\n"
      << "  --camera-mask M              hex/decimal 4-bit mask (default 0xf)\n"
      << "  --frames-per-camera N        finite frame count (default 10)\n"
      << "  --base-port P                camera i uses P+i (default 5610)\n"
      << "  --connect-timeout-ms N       default 10000\n"
      << "  --read-timeout-ms N          default 12000\n"
      << "  --output-dir DIR             write received files (default .)\n"
      << "  --timeout-ms N               overall wait, 0 disables (default 120000)\n";
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
  try {
    std::size_t used = 0U;
    const unsigned long long parsed = std::stoull(text, &used, 0);
    if (used != text.size() || parsed == 0ULL) {
      return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool parse_timeout(const std::string& text, std::chrono::milliseconds& value) {
  std::uint64_t parsed = 0U;
  try {
    std::size_t used = 0U;
    const unsigned long long number = std::stoull(text, &used, 0);
    if (used != text.size()) {
      return false;
    }
    parsed = static_cast<std::uint64_t>(number);
  } catch (const std::exception&) {
    return false;
  }
  if (parsed > 600000ULL) {
    return false;
  }
  value = std::chrono::milliseconds{parsed};
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    auto next = [&]() -> const char* {
      if (index + 1 >= argc) {
        return nullptr;
      }
      return argv[++index];
    };
    if (argument == "--help") {
      print_usage();
      std::exit(0);
    }
    if (argument == "--host") {
      const char* value = next();
      if (value == nullptr) return false;
      options.config.host = value;
    } else if (argument == "--kind") {
      const char* value = next();
      if (value == nullptr) return false;
      const std::string kind{value};
      if (kind == "nv12") {
        options.config.format = orbvi_sdk::ImageStreamFormat::nv12;
      } else if (kind == "jpeg") {
        options.config.format = orbvi_sdk::ImageStreamFormat::jpeg;
      } else {
        return false;
      }
    } else if (argument == "--camera-mask") {
      const char* value = next();
      std::uint64_t mask = 0U;
      if (value == nullptr || !parse_u64(value, mask) || mask > 0x0fU) {
        return false;
      }
      options.config.camera_mask = static_cast<std::uint8_t>(mask);
    } else if (argument == "--frames-per-camera") {
      const char* value = next();
      std::uint64_t frames = 0U;
      if (value == nullptr || !parse_u64(value, frames) || frames > 100000ULL) {
        return false;
      }
      options.config.frames_per_camera = static_cast<std::uint32_t>(frames);
    } else if (argument == "--base-port") {
      const char* value = next();
      std::uint64_t port = 0U;
      if (value == nullptr || !parse_u64(value, port) || port > 65535ULL) {
        return false;
      }
      options.config.base_port = static_cast<std::uint16_t>(port);
    } else if (argument == "--connect-timeout-ms") {
      const char* value = next();
      std::uint64_t timeout = 0U;
      if (value == nullptr || !parse_u64(value, timeout) || timeout > 600000ULL) {
        return false;
      }
      options.config.connect_timeout =
          std::chrono::milliseconds{static_cast<std::int64_t>(timeout)};
    } else if (argument == "--read-timeout-ms") {
      const char* value = next();
      std::uint64_t timeout = 0U;
      if (value == nullptr || !parse_u64(value, timeout) || timeout > 600000ULL) {
        return false;
      }
      options.config.read_timeout =
          std::chrono::milliseconds{static_cast<std::int64_t>(timeout)};
    } else if (argument == "--output-dir") {
      const char* value = next();
      if (value == nullptr || *value == '\0') return false;
      options.output_dir = value;
    } else if (argument == "--timeout-ms") {
      const char* value = next();
      if (value == nullptr || !parse_timeout(value, options.timeout)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return !options.config.host.empty() && options.config.camera_mask != 0U;
}

std::string frame_path(const Options& options, std::uint32_t camera,
                       std::uint64_t sequence) {
  std::ostringstream path;
  path << options.output_dir << "/camera" << camera << "_seq" << sequence;
  path << (options.config.format == orbvi_sdk::ImageStreamFormat::jpeg
               ? ".jpg"
               : ".nv12");
  return path.str();
}

bool write_file(const std::string& path, const std::uint8_t* data,
                std::size_t size) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(size));
  return output.good();
}

std::size_t selected_camera_count(std::uint8_t mask) {
  std::size_t count = 0U;
  for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
    if ((mask & (1U << camera)) != 0U) {
      ++count;
    }
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }

  std::error_code directory_error;
  if (!std::filesystem::create_directories(options.output_dir,
                                           directory_error) &&
      directory_error) {
    std::cerr << "cannot create output directory: "
              << directory_error.message() << '\n';
    return 2;
  }

  orbvi_sdk::Client client = orbvi_sdk::make_image_stream_client(options.config);
  std::atomic<std::uint64_t> written{0U};
  std::atomic<std::uint64_t> write_failures{0U};

  orbvi_sdk::CallbackSet callbacks;
  if (options.config.format == orbvi_sdk::ImageStreamFormat::nv12) {
    callbacks.on_frame = [&](const orbvi_sdk::FrameView& view) {
      if (write_file(frame_path(options, view.camera_index, view.sequence),
                     view.data, view.size)) {
        ++written;
      } else {
        ++write_failures;
      }
    };
  } else {
    callbacks.on_compressed_frame =
        [&](const orbvi_sdk::CompressedFrameView& view) {
          if (write_file(frame_path(options, view.camera_index, view.sequence),
                         view.data, view.size)) {
            ++written;
          } else {
            ++write_failures;
          }
        };
  }

  const std::uint64_t expected =
      static_cast<std::uint64_t>(selected_camera_count(options.config.camera_mask)) *
      options.config.frames_per_camera;
  const std::size_t queue_capacity = static_cast<std::size_t>(
      std::max<std::uint64_t>(64U, expected * 2U));
  auto subscription = client.subscribe(
      orbvi_sdk::SubscribeOptions{queue_capacity, orbvi_sdk::DropPolicy::drop_oldest},
      std::move(callbacks));
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
  while (std::chrono::steady_clock::now() < deadline) {
    if (subscription.value().wait_for_delivery(
            expected, std::chrono::milliseconds{50})) {
      break;
    }
    const orbvi_sdk::ClientState state = client.state();
    if (state == orbvi_sdk::ClientState::failed) {
      break;
    }
    if (state == orbvi_sdk::ClientState::stopped) {
      // The runtime may finish before the subscription has drained its queue.
      const auto stats = subscription.value().stats();
      const auto received = client.diagnostics().frames_received;
      if (stats.delivered + stats.failed + stats.dropped >= received) {
        break;
      }
    }
  }

  const orbvi_sdk::Status stop_status = client.stop();
  const orbvi_sdk::DiagnosticsSnapshot diagnostics = client.diagnostics();
  const orbvi_sdk::Status close_status = client.close();

  bool complete = written.load() == expected && write_failures.load() == 0U;
  for (std::uint32_t camera = 0U; camera < 4U; ++camera) {
    if ((options.config.camera_mask & (1U << camera)) == 0U) {
      continue;
    }
    std::cout << "camera=" << camera
              << " delivered=" << diagnostics.image_delivered[camera]
              << " dropped=" << diagnostics.image_dropped[camera]
              << " crc_errors=" << diagnostics.image_crc_errors[camera] << '\n';
    complete = complete &&
               diagnostics.image_delivered[camera] ==
                   options.config.frames_per_camera &&
               diagnostics.image_dropped[camera] == 0U &&
               diagnostics.image_crc_errors[camera] == 0U;
  }
  std::cout << "IMAGE_RESULT expected=" << expected << " written=" << written.load()
            << " write_failures=" << write_failures.load()
            << " parse_errors=" << diagnostics.parse_errors
            << " callback_failures=" << diagnostics.callback_failures << '\n';
  return complete && stop_status.ok() && close_status.ok() &&
                 diagnostics.parse_errors == 0U &&
                 diagnostics.callback_failures == 0U
             ? 0
             : 1;
}
