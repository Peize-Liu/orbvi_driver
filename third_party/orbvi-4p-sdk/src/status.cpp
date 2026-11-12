#include "orbvi_sdk/status.hpp"

#include "orbvi_sdk/contract.hpp"
#include "orbvi_sdk/device.hpp"
#include "orbvi_sdk/diagnostics.hpp"
#include "orbvi_sdk/export.hpp"
#include "orbvi_sdk/transport.hpp"
#include "orbvi_sdk/version.hpp"

namespace orbvi_sdk {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {
  if (message_.empty() && code_ != StatusCode::ok) {
    message_ = status_code_name(code_);
  }
}

std::string Status::to_string() const {
  if (message_.empty()) {
    return status_code_name(code_);
  }
  return std::string{status_code_name(code_)} + ": " + message_;
}

const char* status_code_name(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::ok:
      return "ok";
    case StatusCode::invalid_argument:
      return "invalid_argument";
    case StatusCode::not_found:
      return "not_found";
    case StatusCode::unavailable:
      return "unavailable";
    case StatusCode::not_supported:
      return "not_supported";
    case StatusCode::already_open:
      return "already_open";
    case StatusCode::not_open:
      return "not_open";
    case StatusCode::busy:
      return "busy";
    case StatusCode::timeout:
      return "timeout";
    case StatusCode::corrupt_data:
      return "corrupt_data";
    case StatusCode::io_error:
      return "io_error";
    case StatusCode::cancelled:
      return "cancelled";
    case StatusCode::internal_error:
      return "internal_error";
  }
  return "unknown";
}

const char* to_string(StatusCode code) noexcept { return status_code_name(code); }

const char* to_string(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::unknown:
      return "unknown";
    case PixelFormat::mjpeg:
      return "mjpeg";
    case PixelFormat::yuyv:
      return "yuyv";
    case PixelFormat::nv12:
      return "nv12";
    case PixelFormat::gray8:
      return "gray8";
    case PixelFormat::raw16:
      return "raw16";
    case PixelFormat::bgr8:
      return "bgr8";
  }
  return "unknown";
}

const char* to_string(FrameLayout layout) noexcept {
  switch (layout) {
    case FrameLayout::unknown:
      return "unknown";
    case FrameLayout::single_plane:
      return "single_plane";
  }
  return "unknown";
}

const char* to_string(DeviceCapability capability) noexcept {
  switch (capability) {
    case DeviceCapability::imu:
      return "imu";
    case DeviceCapability::frame_sync:
      return "frame_sync";
    case DeviceCapability::timing_diagnostics:
      return "timing_diagnostics";
    case DeviceCapability::control:
      return "control";
  }
  return "unknown";
}

bool has_capability(const DeviceInfo& device,
                    DeviceCapability capability) noexcept {
  for (const DeviceCapability value : device.capabilities) {
    if (value == capability) {
      return true;
    }
  }
  return false;
}

const char* to_string(TransportMode mode) noexcept {
  switch (mode) {
    case TransportMode::automatic:
      return "automatic";
    case TransportMode::fake:
      return "fake";
    case TransportMode::replay:
      return "replay";
    case TransportMode::rtsp:
      return "rtsp";
  }
  return "unknown";
}

const char* to_string(ClientState state) noexcept {
  switch (state) {
    case ClientState::closed:
      return "closed";
    case ClientState::open:
      return "open";
    case ClientState::started:
      return "started";
    case ClientState::stopped:
      return "stopped";
    case ClientState::failed:
      return "failed";
  }
  return "unknown";
}

ORBVI_SDK_API const char* sdk_version() noexcept { return kSdkVersion; }

}  // namespace orbvi_sdk
