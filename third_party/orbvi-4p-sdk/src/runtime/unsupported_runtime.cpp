#include <mutex>
#include <string>
#include <utility>

#include "runtime_internal.hpp"

namespace orbvi_sdk {
RuntimeConfig::~RuntimeConfig() = default;
}  // namespace orbvi_sdk

namespace orbvi_sdk::detail {
namespace {

class UnsupportedRuntime final : public Runtime {
 public:
  explicit UnsupportedRuntime(std::string open_message)
      : open_message_(std::move(open_message)) {}

  Result<std::vector<DeviceInfo>> enumerate() const override {
    return std::vector<DeviceInfo>{};
  }

  Status open(const DeviceSelector&) override {
    return Status{StatusCode::not_supported, open_message_};
  }

  Status start() override {
    return Status{StatusCode::not_open, "client is not open"};
  }

  Status stop() override { return Status{}; }

  Status close() noexcept override { return Status{}; }

  void set_callbacks(RuntimeCallbacks callbacks) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    callbacks_ = std::move(callbacks);
  }

  [[nodiscard]] bool started() const noexcept override { return false; }

 private:
  std::string open_message_;
  mutable std::mutex mutex_;
  RuntimeCallbacks callbacks_;
};

}  // namespace

std::unique_ptr<Runtime> make_unsupported_runtime() {
  return std::make_unique<UnsupportedRuntime>(
      "real device backend is not enabled in this build");
}
}  // namespace orbvi_sdk::detail
