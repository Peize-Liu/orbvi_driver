#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

extern "C" {
#include <libavutil/frame.h>
}

namespace orbvi_sdk {

struct H264FrameMetadata {
  std::uint32_t frame_id{0U};
  std::int64_t timestamp_ns{0};
};

inline constexpr std::array<std::uint8_t, 16> kCaptureUuid{
    0x4f, 0x52, 0x42, 0x56, 0x49, 0x2d, 0x34, 0x50,
    0xa5, 0x68, 0x49, 0x26, 0xb2, 0x84, 0x7d, 0x91};

inline bool capture_uuid_matches(const std::uint8_t* data, std::size_t size) {
  if (size < kCaptureUuid.size()) return false;
  for (std::size_t byte = 0; byte < kCaptureUuid.size(); ++byte)
    if (data[byte] != kCaptureUuid[byte]) return false;
  return true;
}

inline bool parse_capture_payload(const std::uint8_t* data, std::size_t size,
                                  std::uint32_t camera_index, H264FrameMetadata& metadata) {
  if (size != 30U || !capture_uuid_matches(data, size) || data[16] != 1U ||
      data[17] != camera_index) return false;
  std::uint32_t frame_id = 0U;
  for (std::size_t byte = 18; byte < 22; ++byte)
    frame_id = (frame_id << 8U) | data[byte];
  std::uint64_t timestamp = 0U;
  for (std::size_t byte = 22; byte < 30; ++byte)
    timestamp = (timestamp << 8U) | data[byte];
  if (timestamp == 0U || timestamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  metadata = {frame_id, static_cast<std::int64_t>(timestamp)};
  return true;
}

inline bool h264_frame_metadata(const AVFrame* frame, std::uint32_t camera_index,
                                H264FrameMetadata& metadata) {
  bool found = false;
  for (int index = 0; index < frame->nb_side_data; ++index) {
    const AVFrameSideData* side = frame->side_data[index];
    if (side == nullptr || side->type != AV_FRAME_DATA_SEI_UNREGISTERED ||
        side->size < 16) continue;
    const auto size = static_cast<std::size_t>(side->size);
    if (!capture_uuid_matches(side->data, size)) continue;
    if (found || !parse_capture_payload(side->data, size, camera_index, metadata)) return false;
    found = true;
  }
  return found;
}

}
