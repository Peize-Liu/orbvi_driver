#include "orbvi_sdk/rtsp.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "orbvi_sdk/client.hpp"
#include "h264_frame_metadata.hpp"
#include "rtsp_frame_converter.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk::detail {
namespace {

std::int64_t host_timestamp_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Status ffmpeg_status(int error, const char* operation) {
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  if (av_strerror(error, text, sizeof(text)) < 0) {
    return Status{StatusCode::io_error,
                  std::string{operation} + ": unknown FFmpeg error"};
  }
  const StatusCode code =
      error == AVERROR(ETIMEDOUT) ? StatusCode::timeout : StatusCode::io_error;
  return Status{code, std::string{operation} + ": " + text};
}

bool valid_rtsp_url(const std::string& url) {
  const bool has_rtsp_scheme = url.rfind("rtsp://", 0U) == 0U;
  const bool has_rtsps_scheme = url.rfind("rtsps://", 0U) == 0U;
  const std::size_t prefix_size = has_rtsps_scheme ? 8U : 7U;
  return (has_rtsp_scheme || has_rtsps_scheme) && url.size() > prefix_size;
}

}  // namespace

Status validate_rtsp_config(const RtspConfig& config) {
  if (!valid_rtsp_url(config.urls[0])) {
    return Status{StatusCode::invalid_argument,
                  "rtsp.urls[0] must be a non-empty rtsp:// or rtsps:// URL"};
  }

  // Empty slots are disabled; every populated slot retains its camera index.
  for (std::size_t index = 1U; index < config.urls.size(); ++index) {
    if (!config.urls[index].empty() && !valid_rtsp_url(config.urls[index])) {
      return Status{StatusCode::invalid_argument,
                    "rtsp.urls[" + std::to_string(index) +
                        "] must be empty or a valid rtsp:// / rtsps:// URL"};
    }
  }

  if (config.connect_timeout.count() <= 0) {
    return Status{StatusCode::invalid_argument,
                  "rtsp.connect_timeout must be greater than zero"};
  }
  if (config.read_timeout.count() <= 0) {
    return Status{StatusCode::invalid_argument,
                  "rtsp.read_timeout must be greater than zero"};
  }
  if (config.reconnect_delay.count() < 0) {
    return Status{StatusCode::invalid_argument,
                  "rtsp.reconnect_delay must not be negative"};
  }
  return Status{};
}

class RtspStreamRuntime final : public Runtime {
 public:
  RtspStreamRuntime(RtspConfig config, std::size_t index, PixelFormat output_format)
      : config_(std::move(config)), camera_index_(index), output_format_(output_format) {}

  [[nodiscard]] std::size_t camera_index() const noexcept { return camera_index_; }

  ~RtspStreamRuntime() override {
    static_cast<void>(stop());
    static_cast<void>(close());
  }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    std::vector<DeviceInfo> devices;
    if (config_.urls[camera_index_].empty()) {
      return devices;
    }

    DeviceInfo device;
    device.id = "rtsp-camera-" + std::to_string(camera_index_);
    // Do not expose URL credentials as a device serial.
    device.serial.clear();
    device.product_id = kProductId4pX5;
    device.firmware_version = "rtsp-h264";
    device.fake = false;
    devices.push_back(std::move(device));
    return devices;
  }

  Status open(const DeviceSelector&) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
      return Status{};
    }

    stop_requested_.store(false);
    const Status status = open_stream();
    if (!status.ok()) {
      close_ffmpeg();
      return status;
    }
    opened_ = true;
    return Status{};
  }

  Status start() override {
    std::thread finished;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!opened_) {
        return Status{StatusCode::not_open,
                      "open() must succeed before start()"};
      }
      if (running_) {
        return Status{};
      }
      if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        finished = std::move(worker_);
      }
      stop_requested_.store(false);
      last_error_ = Status{};
      running_ = true;
    }

    if (finished.joinable()) {
      finished.join();
    }

    try {
      worker_ = std::thread([this]() { decode_loop(); });
    } catch (...) {
      const std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      return Status{StatusCode::internal_error,
                    "cannot start RTSP decode thread"};
    }
    return Status{};
  }

  Status stop() override {
    std::thread worker;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_.store(true);
      running_ = false;
      if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker = std::move(worker_);
      }
    }

    if (worker.joinable()) {
      worker.join();
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    Status result = last_error_;
    last_error_ = Status{};
    return result;
  }

  Status close() noexcept override {
    Status status;
    try {
      status = stop();
    } catch (...) {
      status = Status{StatusCode::internal_error,
                      "RTSP runtime stop threw during close"};
    }

    try {
      const std::lock_guard<std::mutex> lock(mutex_);
      close_ffmpeg();
      opened_ = false;
      stop_requested_.store(false);
    } catch (...) {
      if (status.ok()) {
        status = Status{StatusCode::internal_error,
                        "RTSP runtime cleanup failed"};
      }
    }
    return status;
  }

  void set_callbacks(RuntimeCallbacks callbacks) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    callbacks_ = std::move(callbacks);
  }

  [[nodiscard]] bool started() const noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

 private:
  static int interrupt_callback(void* opaque) {
    const auto* runtime = static_cast<const RtspStreamRuntime*>(opaque);
    return runtime != nullptr && runtime->stop_requested_.load() ? 1 : 0;
  }

  Status open_stream() {
    if (!valid_rtsp_url(config_.urls[camera_index_])) {
      return Status{StatusCode::invalid_argument,
                    "rtsp.urls[" + std::to_string(camera_index_) +
                         "] must be a valid RTSP URL"};
    }

    close_ffmpeg();

    format_ctx_ = avformat_alloc_context();
    if (format_ctx_ == nullptr) {
      return Status{StatusCode::internal_error,
                    "cannot allocate RTSP format context"};
    }
    format_ctx_->interrupt_callback.callback = &RtspStreamRuntime::interrupt_callback;
    format_ctx_->interrupt_callback.opaque = this;

    AVDictionary* options = nullptr;
    const std::string connect_timeout_us = std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            config_.connect_timeout)
            .count());
    const auto read_timeout = std::max(config_.connect_timeout,
                                       config_.read_timeout);
    const std::string read_timeout_us = std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(read_timeout)
            .count());
    static_cast<void>(
        av_dict_set(&options, "rtsp_transport", "tcp", 0));
    static_cast<void>(
        av_dict_set(&options, "stimeout", connect_timeout_us.c_str(), 0));
    static_cast<void>(
        av_dict_set(&options, "rw_timeout", read_timeout_us.c_str(), 0));

    const std::string& url = config_.urls[camera_index_];
    const int open_result =
        avformat_open_input(&format_ctx_, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (open_result < 0) {
      close_ffmpeg();
      return ffmpeg_status(open_result, "cannot open RTSP URL");
    }

    const int stream_info_result = avformat_find_stream_info(format_ctx_, nullptr);
    if (stream_info_result < 0) {
      const Status status =
          ffmpeg_status(stream_info_result, "cannot read RTSP stream info");
      close_ffmpeg();
      return status;
    }

    video_stream_index_ =
        av_find_best_stream(format_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
      close_ffmpeg();
      return Status{StatusCode::not_found,
                    "RTSP stream has no video track"};
    }

    AVStream* stream = format_ctx_->streams[video_stream_index_];
    if (stream == nullptr || stream->codecpar == nullptr ||
        stream->codecpar->codec_id != AV_CODEC_ID_H264) {
      close_ffmpeg();
      return Status{StatusCode::not_supported,
                    "RTSP step 3 supports H.264 video only"};
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
      close_ffmpeg();
      return Status{StatusCode::not_supported,
                    "FFmpeg H.264 decoder is unavailable"};
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (codec_ctx_ == nullptr) {
      close_ffmpeg();
      return Status{StatusCode::internal_error,
                    "cannot allocate H.264 decoder context"};
    }

    const int parameters_result =
        avcodec_parameters_to_context(codec_ctx_, stream->codecpar);
    if (parameters_result < 0) {
      const Status status =
          ffmpeg_status(parameters_result, "cannot copy H.264 parameters");
      close_ffmpeg();
      return status;
    }

    codec_ctx_->pkt_timebase = stream->time_base;
    const int codec_result = avcodec_open2(codec_ctx_, codec, nullptr);
    if (codec_result < 0) {
      const Status status =
          ffmpeg_status(codec_result, "cannot open H.264 decoder");
      close_ffmpeg();
      return status;
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    output_frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr || output_frame_ == nullptr) {
      close_ffmpeg();
      return Status{StatusCode::internal_error,
                    "cannot allocate RTSP decode buffers"};
    }

    return Status{};
  }

  RuntimeCallbacks callbacks_snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return callbacks_;
  }

  Status dispatch_decoded_frame(AVFrame* frame) {
    const int width = frame->width > 0 ? frame->width : codec_ctx_->width;
    const int height = frame->height > 0 ? frame->height : codec_ctx_->height;
    if (width <= 0 || height <= 0) {
      return Status{StatusCode::corrupt_data,
                    "decoded RTSP frame has invalid dimensions"};
    }

    H264FrameMetadata metadata;
    const bool has_metadata = h264_frame_metadata(
        frame, static_cast<std::uint32_t>(camera_index_), metadata);
    const Status converted = converter_.convert(
        frame, output_frame_, width, height, output_format_);
    if (!converted.ok()) return converted;

    FrameView view;
    view.data = output_frame_->data[0];
    view.size = static_cast<std::size_t>(output_frame_->linesize[0]) *
                static_cast<std::size_t>(height);
    view.camera_index = static_cast<std::uint32_t>(camera_index_);
    ++sequence_;
    view.sequence = has_metadata ? metadata.frame_id : sequence_;
    view.group_id = 0U;
    view.timestamp_ns = host_timestamp_ns();
    view.sensor_timestamp_ns = has_metadata ? metadata.timestamp_ns : 0;
    view.generation = 0U;
    view.width = static_cast<std::uint32_t>(width);
    view.height = static_cast<std::uint32_t>(height);
    view.stride = static_cast<std::uint32_t>(output_frame_->linesize[0]);
    view.pixel_format = output_format_;
    view.layout = FrameLayout::single_plane;
    view.contract = ContractFingerprint{};
    view.crc_valid = true;

    const RuntimeCallbacks callbacks = callbacks_snapshot();
    if (callbacks.on_frame) {
      try {
        callbacks.on_frame(view);
      } catch (const std::exception& exception) {
        return Status{StatusCode::internal_error,
                      std::string{"RTSP frame callback failed: "} +
                          exception.what()};
      } catch (...) {
        return Status{StatusCode::internal_error,
                      "RTSP frame callback failed"};
      }
    }
    return Status{};
  }

  Status receive_decoded_frames() {
    while (!stop_requested_.load()) {
      const int receive_result = avcodec_receive_frame(codec_ctx_, frame_);
      if (receive_result == AVERROR(EAGAIN) ||
          receive_result == AVERROR_EOF) {
        return Status{};
      }
      if (receive_result < 0) {
        return ffmpeg_status(receive_result, "cannot decode H.264 frame");
      }

      const Status dispatch_status = dispatch_decoded_frame(frame_);
      av_frame_unref(frame_);
      if (!dispatch_status.ok()) {
        return dispatch_status;
      }
    }
    return Status{};
  }

  Status decode_loop_impl() {
    while (!stop_requested_.load()) {
      const int read_result = av_read_frame(format_ctx_, packet_);
      if (read_result < 0) {
        if (read_result == AVERROR_EXIT || stop_requested_.load()) {
          return Status{};
        }
        if (read_result == AVERROR_EOF) {
          const int flush_result = avcodec_send_packet(codec_ctx_, nullptr);
          if (flush_result < 0 && flush_result != AVERROR_EOF) {
            return ffmpeg_status(flush_result, "cannot flush H.264 decoder");
          }
          return receive_decoded_frames();
        }
        return ffmpeg_status(read_result, "cannot read RTSP packet");
      }

      if (packet_->stream_index == video_stream_index_) {
        int send_result = avcodec_send_packet(codec_ctx_, packet_);
        if (send_result == AVERROR(EAGAIN)) {
          // The decoder still has buffered output. Drain it before retrying
          // the same compressed packet; unrefing here would silently drop it.
          const Status receive_status = receive_decoded_frames();
          if (!receive_status.ok()) {
            av_packet_unref(packet_);
            return receive_status;
          }
          if (stop_requested_.load()) {
            av_packet_unref(packet_);
            return Status{};
          }
          send_result = avcodec_send_packet(codec_ctx_, packet_);
        }
        if (send_result < 0) {
          av_packet_unref(packet_);
          return ffmpeg_status(send_result, "cannot submit H.264 packet");
        }
        av_packet_unref(packet_);
        const Status receive_status = receive_decoded_frames();
        if (!receive_status.ok()) {
          return receive_status;
        }
      } else {
        av_packet_unref(packet_);
      }
    }
    return Status{};
  }

  void decode_loop() noexcept {
    Status result;
    try {
      result = decode_loop_impl();
    } catch (const std::exception& exception) {
      result = Status{StatusCode::internal_error,
                      std::string{"RTSP decode loop failed: "} +
                          exception.what()};
    } catch (...) {
      result = Status{StatusCode::internal_error, "RTSP decode loop failed"};
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    if (!stop_requested_.load()) {
      last_error_ = result;
    }
  }

  void close_ffmpeg() noexcept {
    try {
      converter_.reset();
      if (output_frame_ != nullptr) {
        av_frame_free(&output_frame_);
      }
      if (frame_ != nullptr) {
        av_frame_free(&frame_);
      }
      if (packet_ != nullptr) {
        av_packet_free(&packet_);
      }
      if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
      }
      if (format_ctx_ != nullptr) {
        avformat_close_input(&format_ctx_);
      }
      video_stream_index_ = -1;
    } catch (...) {
      // close() is noexcept; FFmpeg cleanup functions are not expected to throw.
    }
  }

  RtspConfig config_;
  std::size_t camera_index_;
  PixelFormat output_format_;
  mutable std::mutex mutex_;
  RuntimeCallbacks callbacks_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  bool opened_{false};
  bool running_{false};
  Status last_error_;
  AVFormatContext* format_ctx_{nullptr};
  AVCodecContext* codec_ctx_{nullptr};
  AVPacket* packet_{nullptr};
  AVFrame* frame_{nullptr};
  AVFrame* output_frame_{nullptr};
  RtspFrameConverter converter_;
  int video_stream_index_{-1};
  std::uint64_t sequence_{0U};
};

class RtspRuntime final : public Runtime {
 public:
  RtspRuntime(const RtspConfig& config, PixelFormat output_format) {
    for (std::size_t index = 0; index < config.urls.size(); ++index) {
      if (!config.urls[index].empty()) {
        streams_.push_back(
            std::make_unique<RtspStreamRuntime>(config, index, output_format));
      }
    }
  }

  Result<std::vector<DeviceInfo>> enumerate() const override {
    std::vector<DeviceInfo> devices;
    for (const auto& stream : streams_) {
      auto result = stream->enumerate();
      if (!result.ok()) return result.status();
      for (auto& device : result.value()) devices.push_back(std::move(device));
    }
    return devices;
  }

  Status open(const DeviceSelector& selector) override {
    for (std::size_t index = 0; index < streams_.size(); ++index) {
      const Status status = streams_[index]->open(selector);
      if (!status.ok()) {
        for (std::size_t previous = 0; previous < index; ++previous)
          static_cast<void>(streams_[previous]->close());
        return Status{status.code(), "RTSP open failed at camera_index=" +
                                      std::to_string(streams_[index]->camera_index()) +
                                      ": " + status.message()};
      }
    }
    return Status{};
  }

  Status start() override {
    for (std::size_t index = 0; index < streams_.size(); ++index) {
      const Status status = streams_[index]->start();
      if (!status.ok()) {
        for (std::size_t previous = 0; previous < index; ++previous)
          static_cast<void>(streams_[previous]->stop());
        return status;
      }
    }
    return Status{};
  }

  Status stop() override {
    Status first_error;
    for (auto& stream : streams_) {
      const Status status = stream->stop();
      if (first_error.ok() && !status.ok()) first_error = status;
    }
    return first_error;
  }

  Status close() noexcept override {
    Status first_error;
    for (auto& stream : streams_) {
      const Status status = stream->close();
      if (first_error.ok() && !status.ok()) first_error = status;
    }
    return first_error;
  }

  void set_callbacks(RuntimeCallbacks callbacks) override {
    for (auto& stream : streams_) stream->set_callbacks(callbacks);
  }

  [[nodiscard]] bool started() const noexcept override {
    return !streams_.empty() &&
           std::all_of(streams_.begin(), streams_.end(),
                       [](const auto& stream) { return stream->started(); });
  }

 private:
  std::vector<std::unique_ptr<RtspStreamRuntime>> streams_;
};

std::unique_ptr<Runtime> make_rtsp_runtime(const RtspConfig& config,
                                           PixelFormat output_format) {
  return std::make_unique<RtspRuntime>(config, output_format);
}

}  // namespace orbvi_sdk::detail

namespace orbvi_sdk {

Client make_rtsp_client(const RtspConfig& config) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::rtsp;
  options.transport_mode = TransportMode::rtsp;
  options.selector.select_any = true;
  options.runtime_config = std::make_shared<RtspConfig>(config);
  return Client{std::move(options)};
}

Client make_rtsp_client(const RtspConfig& config, PixelFormat output_format) {
  ClientOptions options;
  options.runtime_mode = RuntimeMode::rtsp;
  options.transport_mode = TransportMode::rtsp;
  options.selector.select_any = true;
  auto output_config = std::make_shared<detail::RtspOutputConfig>();
  output_config->source = config;
  output_config->output_format = output_format;
  options.runtime_config = std::move(output_config);
  return Client{std::move(options)};
}

}  // namespace orbvi_sdk
