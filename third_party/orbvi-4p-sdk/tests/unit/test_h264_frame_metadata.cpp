#include <array>
#include <cstdint>
#include <cstring>

#include "h264_frame_metadata.hpp"
#include "test_support.hpp"

int main() {
  constexpr std::array<std::uint8_t, 30> payload{
      0x4f, 0x52, 0x42, 0x56, 0x49, 0x2d, 0x34, 0x50,
      0xa5, 0x68, 0x49, 0x26, 0xb2, 0x84, 0x7d, 0x91,
      1, 2, 0x12, 0x34, 0x56, 0x78,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  AVFrame* frame = av_frame_alloc();
  CHECK_TRUE(frame != nullptr);
  if (frame == nullptr) return orbvi_test::finish();
  orbvi_sdk::H264FrameMetadata metadata;
  CHECK_TRUE(!orbvi_sdk::h264_frame_metadata(frame, 2, metadata));
  AVFrameSideData* side = av_frame_new_side_data(frame, AV_FRAME_DATA_SEI_UNREGISTERED, payload.size());
  CHECK_TRUE(side != nullptr);
  if (side != nullptr) {
    std::memcpy(side->data, payload.data(), payload.size());
    CHECK_TRUE(orbvi_sdk::h264_frame_metadata(frame, 2, metadata));
    CHECK_EQ(metadata.frame_id, 0x12345678U);
    CHECK_EQ(metadata.timestamp_ns, INT64_C(0x0102030405060708));
    CHECK_TRUE(!orbvi_sdk::h264_frame_metadata(frame, 1, metadata));
    side->data[16] = 2;
    CHECK_TRUE(!orbvi_sdk::h264_frame_metadata(frame, 2, metadata));
    side->data[16] = 1;
    std::memset(side->data + 22, 0, 8);
    CHECK_TRUE(!orbvi_sdk::h264_frame_metadata(frame, 2, metadata));
    std::memcpy(side->data, payload.data(), payload.size());
    AVFrameSideData* duplicate = av_frame_new_side_data(frame, AV_FRAME_DATA_SEI_UNREGISTERED, payload.size());
    CHECK_TRUE(duplicate != nullptr);
    if (duplicate != nullptr) {
      std::memcpy(duplicate->data, payload.data(), payload.size());
      CHECK_TRUE(!orbvi_sdk::h264_frame_metadata(frame, 2, metadata));
    }
  }
  av_frame_free(&frame);
  return orbvi_test::finish();
}
