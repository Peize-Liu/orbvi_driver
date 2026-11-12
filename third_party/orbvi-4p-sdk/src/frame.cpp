#include "orbvi_sdk/frame.hpp"
#include "orbvi_sdk/compressed_frame.hpp"

#include <algorithm>

namespace orbvi_sdk {

OwnedFrame::OwnedFrame(const FrameView& view)
    : storage_(), metadata_(view) {
  if (view.data != nullptr && view.size > 0U) {
    storage_.assign(view.data, view.data + view.size);
  }
  metadata_.data = nullptr;
}

FrameView OwnedFrame::view() const noexcept {
  FrameView result = metadata_;
  result.data = storage_.empty() ? nullptr : storage_.data();
  result.size = storage_.size();
  return result;
}

OwnedCompressedFrame::OwnedCompressedFrame(const CompressedFrameView& view) : metadata_(view) {
  if (view.data && view.size) storage_.assign(view.data, view.data + view.size);
  metadata_.data = nullptr;
}
CompressedFrameView OwnedCompressedFrame::view() const noexcept {
  auto result = metadata_;
  result.data = storage_.empty() ? nullptr : storage_.data();
  result.size = storage_.size();
  return result;
}
}  // namespace orbvi_sdk