#pragma once

#include <string>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "orbvi_sdk/contract.hpp"
#include "orbvi_sdk/status.hpp"

namespace orbvi_sdk::detail {

class RtspFrameConverter {
 public:
  RtspFrameConverter() = default;
  RtspFrameConverter(const RtspFrameConverter&) = delete;
  RtspFrameConverter& operator=(const RtspFrameConverter&) = delete;
  ~RtspFrameConverter() { reset(); }

  void reset() noexcept {
    sws_freeContext(context_);
    context_ = nullptr;
    av_frame_free(&intermediate_);
  }

  Status convert(const AVFrame* source, AVFrame* output, int width, int height,
                 PixelFormat output_format) {
    if (source == nullptr || output == nullptr || width <= 0 || height <= 0 ||
        (output_format != PixelFormat::gray8 && output_format != PixelFormat::bgr8)) {
      return Status{StatusCode::invalid_argument, "invalid RTSP frame conversion"};
    }
    const AVPixelFormat destination = output_format == PixelFormat::bgr8
                                          ? AV_PIX_FMT_BGR24
                                          : AV_PIX_FMT_GRAY8;
    const AVPixelFormat source_format = static_cast<AVPixelFormat>(source->format);
    // FFmpeg 4.4 on ARM64 accelerates YUV420 -> BGRA, but not BGR24.
    // Pack away alpha afterwards while preserving the public BGR24 contract.
    bool packed_bgra = false;
#if defined(__aarch64__)
    packed_bgra = destination == AV_PIX_FMT_BGR24 &&
        (source_format == AV_PIX_FMT_YUV420P || source_format == AV_PIX_FMT_YUVJ420P);
#endif
    const auto scaled_destination = packed_bgra ? AV_PIX_FMT_BGRA : destination;
    if (context_ == nullptr || width_ != width || height_ != height ||
        source_format_ != source_format || destination_ != destination) {
      context_ = sws_getCachedContext(
          context_, width, height, source_format, width, height, scaled_destination,
          SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (context_ == nullptr) {
        return Status{StatusCode::internal_error, "cannot create RTSP conversion context"};
      }
      width_ = width;
      height_ = height;
      source_format_ = source_format;
      destination_ = destination;
    }

    if (output->width != width || output->height != height ||
        output->format != destination) {
      av_frame_unref(output);
      output->format = destination;
      output->width = width;
      output->height = height;
      const int result = av_frame_get_buffer(output, 32);
      if (result < 0) {
        return Status{StatusCode::internal_error,
                      "cannot allocate RTSP output buffer: " + std::to_string(result)};
      }
    }
    const int writable = av_frame_make_writable(output);
    if (writable < 0) {
      return Status{StatusCode::internal_error,
                    "cannot make RTSP output writable: " + std::to_string(writable)};
    }
    AVFrame* scaled = output;
    if (packed_bgra) {
      if (intermediate_ == nullptr) intermediate_ = av_frame_alloc();
      if (intermediate_ == nullptr) {
        return Status{StatusCode::internal_error, "cannot allocate RTSP intermediate frame"};
      }
      if (intermediate_->width != width || intermediate_->height != height) {
        av_frame_unref(intermediate_);
        intermediate_->format = AV_PIX_FMT_BGRA;
        intermediate_->width = width;
        intermediate_->height = height;
      }
      if (intermediate_->buf[0] == nullptr && av_frame_get_buffer(intermediate_, 32) < 0) {
        return Status{StatusCode::internal_error, "cannot allocate RTSP intermediate buffer"};
      }
      scaled = intermediate_;
    }
    const int rows = sws_scale(context_, source->data, source->linesize, 0, height,
                               scaled->data, scaled->linesize);
    if (rows != height || output->linesize[0] <= 0) {
      return Status{StatusCode::internal_error, "cannot convert decoded RTSP frame"};
    }
#if defined(__aarch64__)
    if (packed_bgra) {
      for (int y = 0; y < height; ++y) {
        const auto* src = scaled->data[0] + y * scaled->linesize[0];
        auto* dst = output->data[0] + y * output->linesize[0];
        int x = 0;
        for (; x + 16 <= width; x += 16) {
          const auto bgra = vld4q_u8(src + x * 4);
          const uint8x16x3_t bgr = {{bgra.val[0], bgra.val[1], bgra.val[2]}};
          vst3q_u8(dst + x * 3, bgr);
        }
        for (; x < width; ++x) {
          dst[x * 3] = src[x * 4];
          dst[x * 3 + 1] = src[x * 4 + 1];
          dst[x * 3 + 2] = src[x * 4 + 2];
        }
      }
    }
#endif
    return Status{};
  }

 private:
  SwsContext* context_{nullptr};
  AVFrame* intermediate_{nullptr};
  int width_{0};
  int height_{0};
  AVPixelFormat source_format_{AV_PIX_FMT_NONE};
  AVPixelFormat destination_{AV_PIX_FMT_NONE};
};

}
