#pragma once
#include "orbvi_driver/driver.hpp"
namespace orbvi_driver::detail {
// Only an exact repeat of the last sensor identity is safe to suppress.
// Counter resets and changed timestamps must remain visible as discontinuities.
inline bool discard_duplicate(StreamStats& previous,uint64_t sequence,int64_t stamp) {
  if(previous.count && sequence==previous.last_sequence && stamp==previous.last_timestamp_ns) {
    ++previous.duplicates_filtered;
    return true;
  }
  return false;
}
}
