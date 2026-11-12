#include "orbvi_sdk/client.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "orbvi_sdk/fake.hpp"
#include "orbvi_sdk/image_stream.hpp"
#include "orbvi_sdk/replay.hpp"
#include "orbvi_sdk/rtsp.hpp"
#include "orbvi_sdk/udp_imu.hpp"
#include "runtime_internal.hpp"

namespace orbvi_sdk::detail {

struct ClientStateData;

namespace {

enum class EventKind {
  frame,
  compressed_frame,
  temperature,
  imu,
  sync,
  diagnostics,
};

struct DeliveryEvent {
  EventKind kind{EventKind::frame};
  std::shared_ptr<const OwnedFrame> frame;
  std::shared_ptr<const OwnedCompressedFrame> compressed_frame;
  std::shared_ptr<const TemperatureSample> temperature;
  std::uint32_t camera_index{0};
  std::shared_ptr<const ImuEvent> imu;
  std::shared_ptr<const FrameSync> sync;
  std::shared_ptr<const DiagnosticsSnapshot> diagnostics;
};

std::size_t normalized_capacity(std::size_t requested) {
  return requested == 0U ? std::size_t{64U} : requested;
}

thread_local ClientStateData* callback_owner = nullptr;

}  // namespace

struct SubscriptionState {
  enum class EnqueueResult {
    enqueued,
    dropped,
    dropped_oldest,
    cancelled,
    filtered,
    paused,
  };

  SubscriptionState(
      SubscribeOptions resolved_options, CallbackSet callback_set,
      ClientStateData* owner_state,
      std::function<void(EventKind, std::uint32_t)> drop_notifier,
      std::function<void(EventKind, std::uint32_t, bool, std::string)> delivery_notifier)
      : options(std::move(resolved_options)),
        callbacks(std::move(callback_set)),
        capacity(normalized_capacity(options.queue_capacity)),
        owner(owner_state),
        notify_drop(std::move(drop_notifier)),
        notify_delivery(std::move(delivery_notifier)) {}

  ~SubscriptionState() {
    request_cancel();
    join();
  }

  void start() {
    worker = std::thread([this]() { run(); });
  }

  void request_pause() noexcept {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      paused = true;
    }
    ready.notify_all();
  }

  void resume() noexcept {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      paused = false;
    }
    ready.notify_all();
  }

  void request_cancel() noexcept {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      cancelled = true;
      paused = false;
      queue.clear();
    }
    ready.notify_all();
  }

  void join() noexcept {
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      try {
        worker.join();
      } catch (...) {
      }
    }
  }

  [[nodiscard]] EnqueueResult enqueue(DeliveryEvent event) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!wants(event.kind)) {
      return EnqueueResult::filtered;
    }
    if (cancelled) {
      return EnqueueResult::cancelled;
    }
    if (paused) {
      return EnqueueResult::paused;
    }

    bool dropped_oldest = false;
    EventKind evicted_kind = EventKind::frame;
    std::uint32_t evicted_camera = 0U;
    if (queue.size() >= capacity) {
      if (options.drop_policy == DropPolicy::drop_newest) {
        ++dropped;
        return EnqueueResult::dropped;
      }
      if (options.drop_policy == DropPolicy::block) {
        ready.wait(lock, [this]() {
          return cancelled || paused || queue.size() < capacity;
        });
        if (cancelled) {
          return EnqueueResult::cancelled;
        }
        if (paused) {
          return EnqueueResult::paused;
        }
      } else {
        evicted_kind = queue.front().kind;
        evicted_camera = queue.front().camera_index;
        queue.pop_front();
        ++dropped;
        dropped_oldest = true;
      }
    }

    queue.push_back(std::move(event));
    lock.unlock();
    if (dropped_oldest && notify_drop) {
      notify_drop(evicted_kind, evicted_camera);
    }
    ready.notify_all();
    return dropped_oldest ? EnqueueResult::dropped_oldest
                          : EnqueueResult::enqueued;
  }

  [[nodiscard]] SubscriptionStats snapshot() const {
    const std::lock_guard<std::mutex> lock(mutex);
    SubscriptionStats result;
    result.delivered = delivered;
    result.dropped = dropped;
    result.failed = failed;
    result.queued = queue.size();
    result.queue_capacity = capacity;
    result.cancelled = cancelled;
    return result;
  }

  [[nodiscard]] bool wait_for_delivery(
      std::uint64_t target_count,
      std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex);
    return delivered_cv.wait_for(lock, timeout, [this, target_count]() {
             return delivered >= target_count || cancelled;
           }) &&
           delivered >= target_count;
  }

 private:
  [[nodiscard]] bool wants(EventKind kind) const noexcept {
    switch (kind) {
      case EventKind::frame:
        return static_cast<bool>(callbacks.on_frame);
      case EventKind::compressed_frame:
        return static_cast<bool>(callbacks.on_compressed_frame);
      case EventKind::temperature:
        return static_cast<bool>(callbacks.on_temperature);
      case EventKind::imu:
        return static_cast<bool>(callbacks.on_imu);
      case EventKind::sync:
        return static_cast<bool>(callbacks.on_sync);
      case EventKind::diagnostics:
        return static_cast<bool>(callbacks.on_diagnostics);
    }
    return false;
  }

  void dispatch_event(const DeliveryEvent& event) {
    switch (event.kind) {
      case EventKind::frame:
        callbacks.on_frame(event.frame->view());
        break;
      case EventKind::compressed_frame:
        callbacks.on_compressed_frame(event.compressed_frame->view());
        break;
      case EventKind::temperature:
        callbacks.on_temperature(*event.temperature);
        break;
      case EventKind::imu:
        callbacks.on_imu(*event.imu);
        break;
      case EventKind::sync:
        callbacks.on_sync(*event.sync);
        break;
      case EventKind::diagnostics:
        callbacks.on_diagnostics(*event.diagnostics);
        break;
    }
  }

  void run() noexcept {
    while (true) {
      DeliveryEvent event;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this]() {
          return cancelled || (!paused && !queue.empty());
        });
        if (cancelled) {
          return;
        }
        if (paused) {
          continue;
        }
        event = std::move(queue.front());
        queue.pop_front();
      }

      // A blocked producer may now have space. Notify after releasing the
      // queue lock so it cannot contend with the callback dispatch below.
      ready.notify_all();

      bool success = false;
      std::string error;
      ClientStateData* previous_owner = callback_owner;
      callback_owner = owner;
      try {
        dispatch_event(event);
        success = true;
      } catch (const std::exception& exception) {
        error = exception.what();
      } catch (...) {
        error = "unknown callback exception";
      }
      callback_owner = previous_owner;

      try {
        if (notify_delivery) {
          notify_delivery(event.kind, event.camera_index, success, std::move(error));
        }
      } catch (...) {
      }

      {
        const std::lock_guard<std::mutex> lock(mutex);
        if (success) {
          ++delivered;
        } else {
          ++failed;
        }
      }
      if (success) {
        delivered_cv.notify_all();
      }
    }
  }

  SubscribeOptions options;
  CallbackSet callbacks;
  const std::size_t capacity;
  ClientStateData* owner{nullptr};
  std::function<void(EventKind, std::uint32_t)> notify_drop;
  std::function<void(EventKind, std::uint32_t, bool, std::string)> notify_delivery;
  mutable std::mutex mutex;
  mutable std::condition_variable ready;
  mutable std::condition_variable delivered_cv;
  std::deque<DeliveryEvent> queue;
  std::thread worker;
  std::uint64_t delivered{0U};
  std::uint64_t dropped{0U};
  std::uint64_t failed{0U};
  bool cancelled{false};
  bool paused{false};
};

namespace {

std::unique_ptr<Runtime> make_runtime(const ClientOptions& options,
                                      std::string& error) {
  switch (options.runtime_mode) {
    case RuntimeMode::unsupported:
      return make_unsupported_runtime();
    case RuntimeMode::fake: {
      const auto config =
          std::dynamic_pointer_cast<const FakeConfig>(options.runtime_config);
      if (!config) {
        error = "runtime_mode=fake requires a FakeConfig";
        return make_unsupported_runtime();
      }
      return make_fake_runtime(*config);
    }
    case RuntimeMode::replay: {
      const auto config =
          std::dynamic_pointer_cast<const ReplayConfig>(options.runtime_config);
      if (!config) {
        error = "runtime_mode=replay requires a ReplayConfig";
        return make_unsupported_runtime();
      }
      return make_replay_runtime(*config);
    }
    case RuntimeMode::udp_imu: {
      const auto config =
          std::dynamic_pointer_cast<const UdpImuConfig>(options.runtime_config);
      if (!config) {
        error = "runtime_mode=udp_imu requires a UdpImuConfig";
        return make_unsupported_runtime();
      }
      const Status validation = validate_udp_imu_config(*config);
      if (!validation.ok()) {
        error = validation.message();
        return make_unsupported_runtime();
      }
      return make_udp_imu_runtime(*config);
    }
    case RuntimeMode::image_stream: {
      const auto config = std::dynamic_pointer_cast<const ImageStreamConfig>(options.runtime_config);
      if (!config) { error = "runtime_mode=image_stream requires an ImageStreamConfig"; return make_unsupported_runtime(); }
      const auto validation = validate_image_stream_config(*config);
      if (!validation.ok()) { error = validation.message(); return make_unsupported_runtime(); }
      return make_image_stream_runtime(*config);
    }
    case RuntimeMode::temperature_stream: {
      const auto config =
          std::dynamic_pointer_cast<const TemperatureStreamConfig>(
              options.runtime_config);
      if (!config) {
        error = "runtime_mode=temperature_stream requires a TemperatureStreamConfig";
        return make_unsupported_runtime();
      }
      const Status validation = validate_temperature_stream_config(*config);
      if (!validation.ok()) {
        error = validation.message();
        return make_unsupported_runtime();
      }
      return make_temperature_stream_runtime(*config);
    }
    case RuntimeMode::rtsp: {
      const auto config =
          std::dynamic_pointer_cast<const RtspConfig>(options.runtime_config);
      const auto output_config =
          std::dynamic_pointer_cast<const RtspOutputConfig>(options.runtime_config);
      if (!config && !output_config) {
        error = "runtime_mode=rtsp requires an RtspConfig";
        return make_unsupported_runtime();
      }
      const RtspConfig& source = config ? *config : output_config->source;
      const PixelFormat output_format =
          config ? PixelFormat::gray8 : output_config->output_format;
      if (output_format != PixelFormat::gray8 && output_format != PixelFormat::bgr8) {
        error = "rtsp output_format must be gray8 or bgr8";
        return make_unsupported_runtime();
      }
      const Status validation = validate_rtsp_config(source);
      if (!validation.ok()) {
        error = validation.message();
        return make_unsupported_runtime();
      }
      return make_rtsp_runtime(source, output_format);
    }
  }
  error = "unknown runtime mode";
  return make_unsupported_runtime();
}

}  // namespace

struct ClientStateData {
  enum class Transition {
    none,
    opening,
    starting,
    stopping,
    closing,
  };

  explicit ClientStateData(ClientOptions client_options)
      : options(std::move(client_options)),
        runtime(make_runtime(options, configuration_error)) {
    if (options.callback_queue_capacity == 0U) {
      configuration_error = "callback_queue_capacity must be greater than zero";
    }
    if (options.default_drop_policy == DropPolicy::inherit) {
      configuration_error =
          "ClientOptions.default_drop_policy cannot be inherit";
    }

    RuntimeCallbacks callbacks;
    callbacks.on_frame = [this](const FrameView& frame) { dispatch_frame(frame); };
    callbacks.on_compressed_frame = [this](const CompressedFrameView& frame) { dispatch_compressed(frame); };
    callbacks.on_temperature = [this](const TemperatureSample& sample) { dispatch_temperature(sample); };
    callbacks.on_image_error = [this](std::uint32_t camera, bool crc, std::string message) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.image_dropped[camera]; ++diagnostics.frames_dropped;
      if (crc) { ++diagnostics.crc_errors; ++diagnostics.image_crc_errors[camera]; }
      else ++diagnostics.parse_errors;
      diagnostics.last_error = std::move(message);
    };
    callbacks.on_image_sequence_gap = [this](std::uint32_t camera, std::uint64_t gap) {
      const std::lock_guard<std::mutex> lock(mutex);
      diagnostics.image_dropped[camera] += gap; diagnostics.frames_dropped += gap;
    };
    callbacks.on_temperature_error = [this](bool crc, std::string message) {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.temperature_samples_dropped;
      if (crc) {
        ++diagnostics.crc_errors;
        ++diagnostics.temperature_crc_errors;
      } else {
        ++diagnostics.parse_errors;
      }
      diagnostics.last_error = std::move(message);
    };
    callbacks.on_temperature_sequence_gap = [this](std::uint64_t gap) {
      const std::lock_guard<std::mutex> lock(mutex);
      diagnostics.temperature_samples_dropped += gap;
      diagnostics.temperature_sequence_gaps += gap;
    };
    callbacks.on_imu = [this](const ImuEvent& sample) { dispatch_imu(sample); };
    callbacks.on_sync = [this](const FrameSync& value) { dispatch_sync(value); };
    callbacks.on_diagnostics =
        [this](const DiagnosticsSnapshot& value) { dispatch_diagnostics(value); };
    callbacks.on_imu_packet_error = [this](bool crc) {
      const std::lock_guard<std::mutex> lock(mutex);
      if (crc) {
        ++diagnostics.crc_errors;
      } else {
        ++diagnostics.parse_errors;
      }
    };
    callbacks.on_imu_sequence_gap = [this](std::uint64_t gap) {
      const std::lock_guard<std::mutex> lock(mutex);
      diagnostics.imu_sequence_gaps += gap;
    };
    runtime->set_callbacks(std::move(callbacks));
  }

  ~ClientStateData() {
    cancel_subscriptions();
    if (runtime) {
      try {
        static_cast<void>(runtime->stop());
        static_cast<void>(runtime->close());
      } catch (...) {
      }
    }
  }

  [[nodiscard]] bool is_callback_thread() const noexcept {
    return callback_owner == this;
  }

  void record_drop(EventKind kind, std::uint32_t camera = 0) noexcept {
    const std::lock_guard<std::mutex> lock(mutex);
    if (kind == EventKind::frame || kind == EventKind::compressed_frame) {
      ++diagnostics.frames_dropped;
      if (options.runtime_mode == RuntimeMode::image_stream) {
        ++diagnostics.image_dropped[camera];
      }
    } else if (kind == EventKind::temperature) {
      ++diagnostics.temperature_samples_dropped;
    }
  }

  void record_delivery(EventKind kind, std::uint32_t camera, bool success, std::string error) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (success) {
      if (kind == EventKind::frame || kind == EventKind::compressed_frame) {
        ++diagnostics.frames_delivered;
        if (options.runtime_mode == RuntimeMode::image_stream) {
          ++diagnostics.image_delivered[camera];
        }
      } else if (kind == EventKind::temperature) {
        ++diagnostics.temperature_samples_delivered;
      }
      return;
    }

    if (options.runtime_mode == RuntimeMode::image_stream &&
        (kind == EventKind::frame || kind == EventKind::compressed_frame)) {
      ++diagnostics.image_dropped[camera];
      ++diagnostics.frames_dropped;
    } else if (kind == EventKind::temperature) {
      ++diagnostics.temperature_samples_dropped;
    }
    ++diagnostics.callback_failures;
    if (error.empty()) {
      diagnostics.last_error = "callback failed";
    } else {
      diagnostics.last_error = "callback failed: " + error;
    }
  }

  void dispatch_frame(const FrameView& view) {
    auto owned = std::make_shared<OwnedFrame>(view);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      targets = subscriptions;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.frames_received;
      diagnostics.last_frame_timestamp_ns = view.timestamp_ns;
      if (!view.crc_valid) {
        ++diagnostics.crc_errors;
      }
    }

    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::frame;
      event.frame = owned;
      event.camera_index = view.camera_index;
      const SubscriptionState::EnqueueResult result =
          target->enqueue(std::move(event));
      if (result == SubscriptionState::EnqueueResult::dropped) {
        record_drop(EventKind::frame, view.camera_index);
      }
    }
  }

  void dispatch_compressed(const CompressedFrameView& view) {
    auto owned = std::make_shared<OwnedCompressedFrame>(view);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    { const std::lock_guard<std::mutex> lock(subscriptions_mutex); targets = subscriptions; }
    { const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.frames_received;
      diagnostics.last_frame_timestamp_ns = view.timestamp_ns;
    }
    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::compressed_frame;
      event.compressed_frame = owned;
      event.camera_index = view.camera_index;
      const auto result = target->enqueue(std::move(event));
      if (result == SubscriptionState::EnqueueResult::dropped)
        record_drop(EventKind::compressed_frame, view.camera_index);
    }
  }

  void dispatch_temperature(const TemperatureSample& sample) {
    auto owned = std::make_shared<TemperatureSample>(sample);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      targets = subscriptions;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.temperature_samples_received;
    }

    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::temperature;
      event.temperature = owned;
      const auto result = target->enqueue(std::move(event));
      if (result == SubscriptionState::EnqueueResult::dropped) {
        record_drop(EventKind::temperature);
      }
    }
  }

  void dispatch_imu(const ImuEvent& sample) {
    auto owned = std::make_shared<ImuEvent>(sample);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      targets = subscriptions;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.imu_samples_received;
    }

    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::imu;
      event.imu = owned;
      static_cast<void>(target->enqueue(std::move(event)));
    }
  }

  void dispatch_sync(const FrameSync& value) {
    auto owned = std::make_shared<FrameSync>(value);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      targets = subscriptions;
    }
    {
      const std::lock_guard<std::mutex> lock(mutex);
      ++diagnostics.sync_messages_received;
    }

    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::sync;
      event.sync = owned;
      static_cast<void>(target->enqueue(std::move(event)));
    }
  }

  void dispatch_diagnostics(const DiagnosticsSnapshot& value) {
    auto owned = std::make_shared<DiagnosticsSnapshot>(value);
    std::vector<std::shared_ptr<SubscriptionState>> targets;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      targets = subscriptions;
    }

    for (const auto& target : targets) {
      DeliveryEvent event;
      event.kind = EventKind::diagnostics;
      event.diagnostics = owned;
      static_cast<void>(target->enqueue(std::move(event)));
    }
  }

  void pause_subscriptions() noexcept {
    std::vector<std::shared_ptr<SubscriptionState>> active;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      active = subscriptions;
    }
    for (const auto& subscription : active) {
      subscription->request_pause();
    }
  }

  void resume_subscriptions() noexcept {
    std::vector<std::shared_ptr<SubscriptionState>> active;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      active = subscriptions;
    }
    for (const auto& subscription : active) {
      subscription->resume();
    }
  }

  void cancel_subscriptions() noexcept {
    std::vector<std::shared_ptr<SubscriptionState>> active;
    {
      const std::lock_guard<std::mutex> lock(subscriptions_mutex);
      active.swap(subscriptions);
    }

    for (const auto& subscription : active) {
      subscription->request_cancel();
    }
    for (const auto& subscription : active) {
      subscription->join();
    }
  }

  [[nodiscard]] bool has_subscriptions() const noexcept {
    const std::lock_guard<std::mutex> lock(subscriptions_mutex);
    return !subscriptions.empty();
  }

  void set_last_error(const Status& status) {
    const std::lock_guard<std::mutex> lock(mutex);
    diagnostics.last_error = status.to_string();
  }

  void clear_last_error() {
    const std::lock_guard<std::mutex> lock(mutex);
    diagnostics.last_error.clear();
  }

  ClientOptions options;
  std::string configuration_error;
  std::unique_ptr<Runtime> runtime;
  mutable std::mutex mutex;
  mutable std::mutex subscriptions_mutex;
  mutable std::mutex lifecycle_mutex;
  mutable std::condition_variable lifecycle_cv;
  ClientState state{ClientState::closed};
  Transition transition{Transition::none};
  DiagnosticsSnapshot diagnostics;
  std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
};

}  // namespace orbvi_sdk::detail

namespace orbvi_sdk {

SubscriptionHandle::SubscriptionHandle(
    std::shared_ptr<detail::SubscriptionState> state)
    : state_(std::move(state)) {}

bool SubscriptionHandle::valid() const noexcept {
  return state_ != nullptr && !state_->snapshot().cancelled;
}

void SubscriptionHandle::cancel() noexcept {
  if (state_) {
    state_->request_cancel();
  }
}

SubscriptionStats SubscriptionHandle::stats() const {
  return state_ ? state_->snapshot() : SubscriptionStats{};
}

bool SubscriptionHandle::wait_for_delivery(
    std::uint64_t target_count, std::chrono::milliseconds timeout) const {
  return state_ != nullptr && state_->wait_for_delivery(target_count, timeout);
}

Client::Client(ClientOptions options)
    : state_(std::make_unique<detail::ClientStateData>(std::move(options))) {}

Client::Client(Client&& other) noexcept = default;
Client& Client::operator=(Client&& other) noexcept = default;

Client::~Client() {
  if (state_) {
    static_cast<void>(close());
  }
}

const ClientOptions& Client::options() const noexcept {
  static const ClientOptions fallback{};
  return state_ ? state_->options : fallback;
}

ClientState Client::state() const noexcept {
  if (!state_) {
    return ClientState::closed;
  }

  ClientState current = ClientState::closed;
  {
    const std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    if (state_->transition != detail::ClientStateData::Transition::none) {
      return state_->state;
    }
    current = state_->state;
  }

  if (current == ClientState::started && state_->runtime &&
      !state_->runtime->started()) {
    const std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    if (state_->state == ClientState::started &&
        state_->transition == detail::ClientStateData::Transition::none &&
        !state_->runtime->started()) {
      state_->state = ClientState::stopped;
    }
    return state_->state;
  }

  return current;
}

DiagnosticsSnapshot Client::diagnostics() const {
  if (!state_) {
    return DiagnosticsSnapshot{};
  }

  DiagnosticsSnapshot result;
  {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    result = state_->diagnostics;
  }
  result.state = state();
  return result;
}

Result<std::vector<DeviceInfo>> Client::enumerate() const {
  if (!state_) {
    return Status{StatusCode::internal_error, "client is moved-from"};
  }
  return state_->runtime->enumerate();
}

Status Client::open() {
  if (!state_) {
    return Status{StatusCode::internal_error, "client is moved-from"};
  }
  if (state_->is_callback_thread()) {
    return Status{StatusCode::busy, "open() from callback is not supported"};
  }
  if (!state_->configuration_error.empty()) {
    return Status{StatusCode::invalid_argument, state_->configuration_error};
  }

  std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
  state_->lifecycle_cv.wait(lock, [this]() {
    return state_->transition == detail::ClientStateData::Transition::none;
  });

  if (state_->state == ClientState::open ||
      state_->state == ClientState::started ||
      state_->state == ClientState::stopped) {
    return Status{StatusCode::already_open, "client is already open"};
  }
  if (state_->state == ClientState::failed) {
    return Status{StatusCode::internal_error, "client is in failed state"};
  }

  state_->transition = detail::ClientStateData::Transition::opening;
  lock.unlock();

  Status status;
  try {
    status = state_->runtime->open(state_->options.selector);
  } catch (...) {
    status = Status{StatusCode::internal_error, "runtime open threw an exception"};
  }

  // Do not hold lifecycle_mutex while taking the diagnostics mutex: state()
  // and diagnostics() may otherwise form a lock-order cycle.
  if (status.ok()) {
    state_->clear_last_error();
  } else {
    state_->set_last_error(status);
  }

  lock.lock();
  state_->state = status.ok() ? ClientState::open : ClientState::failed;
  state_->transition = detail::ClientStateData::Transition::none;
  state_->lifecycle_cv.notify_all();
  return status;
}

Status Client::start() {
  if (!state_) {
    return Status{StatusCode::internal_error, "client is moved-from"};
  }
  if (state_->is_callback_thread()) {
    return Status{StatusCode::busy, "start() from callback is not supported"};
  }

  std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
  state_->lifecycle_cv.wait(lock, [this]() {
    return state_->transition == detail::ClientStateData::Transition::none;
  });

  if (state_->state == ClientState::started &&
      !state_->runtime->started()) {
    // A finite runtime (notably Replay at EOF) can end without an explicit
    // stop(). Normalize that state before deciding whether start is idempotent.
    state_->state = ClientState::stopped;
  }
  if (state_->state == ClientState::started) {
    return Status{};
  }
  if (state_->state == ClientState::failed) {
    return Status{StatusCode::internal_error, "client is in failed state"};
  }
  if (state_->state != ClientState::open &&
      state_->state != ClientState::stopped) {
    return Status{StatusCode::not_open, "open() must succeed before start()"};
  }

  state_->transition = detail::ClientStateData::Transition::starting;
  lock.unlock();

  Status status;
  try {
    status = state_->runtime->start();
  } catch (...) {
    status = Status{StatusCode::internal_error, "runtime start threw an exception"};
  }

  if (status.ok()) {
    state_->clear_last_error();
  } else {
    state_->set_last_error(status);
  }

  lock.lock();
  state_->state = status.ok() ? ClientState::started : ClientState::failed;
  state_->transition = detail::ClientStateData::Transition::none;
  state_->lifecycle_cv.notify_all();
  lock.unlock();

  // A stop() pauses subscriptions without destroying them. Resume only after
  // the runtime has successfully started and the public state is committed.
  if (status.ok()) {
    state_->resume_subscriptions();
  }
  return status;
}

Status Client::stop() {
  if (!state_) {
    return Status{StatusCode::internal_error, "client is moved-from"};
  }
  if (state_->is_callback_thread()) {
    return Status{StatusCode::busy, "stop() from callback is not supported"};
  }

  std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
  state_->lifecycle_cv.wait(lock, [this]() {
    return state_->transition == detail::ClientStateData::Transition::none;
  });

  if (state_->state == ClientState::open ||
      state_->state == ClientState::stopped ||
      state_->state == ClientState::closed) {
    return Status{};
  }
  if (state_->state == ClientState::failed) {
    return Status{StatusCode::internal_error, "client is in failed state"};
  }

  state_->transition = detail::ClientStateData::Transition::stopping;
  lock.unlock();

  // Pause, rather than cancel, so a subsequent start() can resume the same
  // subscriptions. Pausing also wakes producers blocked in DropPolicy::block.
  state_->pause_subscriptions();

  Status status;
  try {
    status = state_->runtime->stop();
  } catch (...) {
    status = Status{StatusCode::internal_error, "runtime stop threw an exception"};
  }

  if (status.ok()) {
    state_->clear_last_error();
  } else {
    state_->set_last_error(status);
  }

  lock.lock();
  state_->state = status.ok() ? ClientState::stopped : ClientState::failed;
  state_->transition = detail::ClientStateData::Transition::none;
  state_->lifecycle_cv.notify_all();
  return status;
}

Status Client::close() noexcept {
  if (!state_) {
    return Status{};
  }
  if (state_->is_callback_thread()) {
    return Status{StatusCode::busy, "close() from callback is not supported"};
  }

  try {
    ClientState prior_state = ClientState::closed;
    {
      std::unique_lock<std::mutex> lock(state_->lifecycle_mutex);
      state_->lifecycle_cv.wait(lock, [this]() {
        return state_->transition == detail::ClientStateData::Transition::none;
      });
      prior_state = state_->state;
      state_->transition = detail::ClientStateData::Transition::closing;
    }

    // Wake blocked producers before stopping the runtime. The subscription
    // workers are joined only after the runtime has stopped producing events.
    state_->pause_subscriptions();

    Status first_error;
    if (prior_state == ClientState::started ||
        prior_state == ClientState::failed) {
      Status stop_status;
      try {
        stop_status = state_->runtime->stop();
      } catch (...) {
        stop_status =
            Status{StatusCode::internal_error, "runtime stop threw an exception"};
      }
      if (!stop_status.ok()) {
        first_error = stop_status;
      }
    }

    Status close_status;
    try {
      close_status = state_->runtime->close();
    } catch (...) {
      close_status =
          Status{StatusCode::internal_error, "runtime close threw an exception"};
    }
    if (first_error.ok() && !close_status.ok()) {
      first_error = close_status;
    }

    state_->cancel_subscriptions();

    // Commit the lifecycle state before touching diagnostics, so the
    // lifecycle lock is never held while acquiring the diagnostics mutex.
    {
      std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
      state_->state = first_error.ok() ? ClientState::closed : ClientState::failed;
      state_->transition = detail::ClientStateData::Transition::none;
      state_->lifecycle_cv.notify_all();
    }

    if (first_error.ok()) {
      state_->clear_last_error();
    } else {
      state_->set_last_error(first_error);
    }
    return first_error;
  } catch (...) {
    const Status status{StatusCode::internal_error, "close failed"};
    try {
      {
        const std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
        state_->state = ClientState::failed;
        state_->transition = detail::ClientStateData::Transition::none;
        state_->lifecycle_cv.notify_all();
      }
      state_->set_last_error(status);
    } catch (...) {
    }
    return status;
  }
}

Result<SubscriptionHandle> Client::subscribe(SubscribeOptions options,
                                              CallbackSet callbacks) {
  if (!state_) {
    return Status{StatusCode::internal_error, "client is moved-from"};
  }
  if (state_->is_callback_thread()) {
    return Status{StatusCode::busy, "subscribe() from callback is not supported"};
  }
  if (!callbacks.on_frame && !callbacks.on_compressed_frame &&
      !callbacks.on_temperature && !callbacks.on_imu && !callbacks.on_sync &&
      !callbacks.on_diagnostics) {
    return Status{StatusCode::invalid_argument, "at least one callback is required"};
  }
  if (!state_->configuration_error.empty()) {
    return Status{StatusCode::invalid_argument, state_->configuration_error};
  }

  std::unique_lock<std::mutex> lifecycle_lock(state_->lifecycle_mutex);
  if (state_->transition != detail::ClientStateData::Transition::none) {
    return Status{StatusCode::busy, "client lifecycle transition is in progress"};
  }
  if (state_->state == ClientState::failed) {
    return Status{StatusCode::not_open, "client is in failed state"};
  }

  const std::size_t effective_capacity =
      options.queue_capacity == 0U ? state_->options.callback_queue_capacity
                                   : options.queue_capacity;
  const DropPolicy effective_policy =
      options.drop_policy == DropPolicy::inherit
          ? state_->options.default_drop_policy
          : options.drop_policy;
  if (effective_capacity == 0U || effective_policy == DropPolicy::inherit) {
    return Status{StatusCode::invalid_argument,
                  "subscription queue capacity and drop policy must resolve"};
  }

  SubscribeOptions resolved;
  resolved.queue_capacity = effective_capacity;
  resolved.drop_policy = effective_policy;

  std::shared_ptr<detail::SubscriptionState> subscription;
  try {
    subscription = std::make_shared<detail::SubscriptionState>(
        resolved, std::move(callbacks), state_.get(),
        [this](detail::EventKind kind, std::uint32_t camera) { state_->record_drop(kind, camera); },
        [this](detail::EventKind kind, std::uint32_t camera, bool success, std::string error) {
          state_->record_delivery(kind, camera, success, std::move(error));
        });
    subscription->start();
  } catch (...) {
    return Status{StatusCode::internal_error, "cannot start subscription worker"};
  }

  {
    const std::lock_guard<std::mutex> subscriptions_lock(
        state_->subscriptions_mutex);
    state_->subscriptions.push_back(subscription);
  }
  return SubscriptionHandle{std::move(subscription)};
}

}  // namespace orbvi_sdk
