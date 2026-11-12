#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavutil/log.h>
}

#include "rtsp_frame_converter.hpp"
#include "test_support.hpp"

namespace {
int warning_count = 0;

void count_warnings(void*, int level, const char*, va_list) {
  if (level <= AV_LOG_WARNING) ++warning_count;
}
}

int main() {
  using orbvi_sdk::PixelFormat;
  using orbvi_sdk::StatusCode;
  using orbvi_sdk::detail::RtspFrameConverter;

  AVFrame* source = av_frame_alloc();
  AVFrame* output = av_frame_alloc();
  RtspFrameConverter converter;
  CHECK_TRUE(source != nullptr);
  CHECK_TRUE(output != nullptr);
  if (source == nullptr || output == nullptr) {
    av_frame_free(&source);
    av_frame_free(&output);
    return orbvi_test::finish();
  }
  source->format = AV_PIX_FMT_RGB24;
  source->width = 13;
  source->height = 4;
  const int allocated = av_frame_get_buffer(source, 32);
  CHECK_EQ(allocated, 0);
  if (allocated < 0) {
    av_frame_free(&source);
    av_frame_free(&output);
    return orbvi_test::finish();
  }
  for (int row = 0; row < source->height; ++row) {
    for (int column = 0; column < source->width; ++column) {
      auto* pixel = source->data[0] + row * source->linesize[0] + column * 3;
      pixel[0] = 210;
      pixel[1] = static_cast<std::uint8_t>(30 + row);
      pixel[2] = static_cast<std::uint8_t>(10 + column);
    }
  }

  CHECK_TRUE(converter.convert(source, output, source->width,
                               source->height, PixelFormat::bgr8).ok());
  CHECK_EQ(output->format, AV_PIX_FMT_BGR24);
  CHECK_TRUE(output->linesize[0] > source->width * 3);
  for (int row = 0; row < source->height; ++row) {
    for (int column = 0; column < source->width; ++column) {
      const auto* pixel = output->data[0] + row * output->linesize[0] + column * 3;
      CHECK_EQ(pixel[0], static_cast<std::uint8_t>(10 + column));
      CHECK_EQ(pixel[1], static_cast<std::uint8_t>(30 + row));
      CHECK_EQ(pixel[2], static_cast<std::uint8_t>(210));
    }
  }

  CHECK_TRUE(converter.convert(source, output, source->width,
                               source->height, PixelFormat::gray8).ok());
  CHECK_EQ(output->format, AV_PIX_FMT_GRAY8);
  CHECK_TRUE(output->data[0][0] > 30U);
  CHECK_TRUE(output->data[0][0] < 210U);
  CHECK_EQ(converter.convert(source, output, source->width,
                             source->height, PixelFormat::nv12).code(),
           StatusCode::invalid_argument);
  CHECK_EQ(converter.convert(source, output, 0, source->height,
                             PixelFormat::bgr8).code(),
           StatusCode::invalid_argument);

  av_frame_unref(source);
  source->format = AV_PIX_FMT_YUVJ420P;
  source->width = 14;
  source->height = 4;
  const int yuv_allocated = av_frame_get_buffer(source, 32);
  CHECK_EQ(yuv_allocated, 0);
  if (yuv_allocated == 0) {
    std::memset(source->data[0], 80, source->linesize[0] * source->height);
    std::memset(source->data[1], 90, source->linesize[1] * (source->height / 2));
    std::memset(source->data[2], 180, source->linesize[2] * (source->height / 2));
    av_log_set_callback(count_warnings);
    CHECK_TRUE(converter.convert(source, output, source->width,
                                 source->height, PixelFormat::bgr8).ok());
    const int initial_warnings = warning_count;
    for (int index = 0; index < 5; ++index) {
      CHECK_TRUE(converter.convert(source, output, source->width,
                                   source->height, PixelFormat::bgr8).ok());
    }
    CHECK_EQ(warning_count, initial_warnings);
    CHECK_EQ(output->format, AV_PIX_FMT_BGR24);
    CHECK_TRUE(output->data[0][0] != output->data[0][2]);
    CHECK_TRUE(converter.convert(source, output, source->width,
                                 source->height, PixelFormat::gray8).ok());
    converter.reset();
    CHECK_TRUE(converter.convert(source, output, source->width,
                                 source->height, PixelFormat::gray8).ok());
    av_log_set_callback(av_log_default_callback);
  }

  // Exercise the ARM64 vector pack and scalar tail, padding, limited/full range,
  // resize/reset, and reuse while a consumer still holds the previous frame.
  for (const auto format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}) {
    for (int width : {14, 34, 1280}) {
      av_frame_unref(source);
      source->format = format;
      source->width = width;
      source->height = 4;
      CHECK_EQ(av_frame_get_buffer(source, 32), 0);
      AVFrame* reference = av_frame_alloc();
      reference->format = AV_PIX_FMT_BGR24;
      reference->width = width;
      reference->height = 4;
      CHECK_EQ(av_frame_get_buffer(reference, 32), 0);
      auto* context = sws_getContext(width, 4, format, width, 4, AV_PIX_FMT_BGR24,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
      CHECK_TRUE(context != nullptr);
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < width; ++x) source->data[0][y * source->linesize[0] + x] = 16 + (x + y * 50) % 220;
      }
      std::memset(source->data[1], 90, source->linesize[1] * 2);
      std::memset(source->data[2], 180, source->linesize[2] * 2);
      sws_scale(context, source->data, source->linesize, 0, 4, reference->data, reference->linesize);
      CHECK_TRUE(converter.convert(source, output, width, 4, PixelFormat::bgr8).ok());
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < width * 3; ++x) {
          CHECK_TRUE(std::abs(int(output->data[0][y * output->linesize[0] + x]) -
                              int(reference->data[0][y * reference->linesize[0] + x])) <= 3);
        }
      }
      AVFrame* held = av_frame_clone(output);
      CHECK_TRUE(held != nullptr);
      const auto first = held->data[0][2];
      std::memset(source->data[0], 235, source->linesize[0] * 4);
      CHECK_TRUE(converter.convert(source, output, width, 4, PixelFormat::bgr8).ok());
      CHECK_EQ(held->data[0][2], first);
      av_frame_free(&held);
      av_frame_free(&reference);
      sws_freeContext(context);
      converter.reset();
    }
  }

  av_frame_free(&source);
  av_frame_free(&output);
  return orbvi_test::finish();
}
