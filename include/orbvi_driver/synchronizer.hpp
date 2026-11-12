#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
namespace orbvi_driver {
// Frame counters belong to individual cameras. Match the shared board clock,
// retaining original frame IDs separately; reject rather than invent partners.
class TimestampMatcher {
 public:
  struct Match {std::array<int64_t,4> timestamps;uint64_t skew_ns;};
  explicit TimestampMatcher(int64_t tolerance_ns=1000000,int64_t retention_ns=1000000000):tolerance_(tolerance_ns),retention_(retention_ns){}
  uint64_t unmatched=0;
  std::optional<Match> add(unsigned camera,int64_t stamp) {
    if(camera>=4||stamp<=0)return {};
    for(auto it=pending_.begin();it!=pending_.end();) {
      if(stamp-it->first<=retention_&&pending_.size()<256)break;
      unmatched+=it->second.size();it=pending_.erase(it);
    }
    auto best=pending_.end();int64_t distance=tolerance_+1;
    for(auto it=pending_.lower_bound(stamp-tolerance_);it!=pending_.end()&&it->first<=stamp+tolerance_;++it) {
      if(it->second.count(camera))continue;
      auto d=it->first>stamp?it->first-stamp:stamp-it->first;
      if(d<distance){best=it;distance=d;}
    }
    if(best==pending_.end())best=pending_.emplace(stamp,std::map<unsigned,int64_t>{}).first;
    best->second[camera]=stamp;
    if(best->second.size()!=4)return {};
    Match m{};for(auto p:best->second)m.timestamps[p.first]=p.second;
    const auto bounds=std::minmax_element(m.timestamps.begin(),m.timestamps.end());m.skew_ns=uint64_t(*bounds.second-*bounds.first);
    pending_.erase(best);return m;
  }
 private:
  int64_t tolerance_,retention_;
  std::map<int64_t,std::map<unsigned,int64_t>> pending_;
};
}
