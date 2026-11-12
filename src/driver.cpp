#include "orbvi_driver/driver.hpp"
#include "handshake.hpp"
#include "duplicate_filter.hpp"
#include "orbvi_driver/synchronizer.hpp"
#include <orbvi_sdk/client.hpp>
#include <orbvi_sdk/rtsp.hpp>
#include <orbvi_sdk/udp_imu.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace orbvi_driver {
static void require(const orbvi_sdk::Status& s) { if(!s.ok()) throw std::runtime_error(s.to_string()); }
static void observe(StreamStats& s,uint64_t sequence,int64_t stamp,bool camera) {
  if(s.count) {
    if(stamp<=s.last_timestamp_ns) ++s.discontinuities;
    if(camera) { auto delta=uint32_t(sequence-s.last_sequence); if(delta==0||delta>=0x80000000u) ++s.discontinuities; else s.gaps+=delta-1; }
    else if(sequence<=s.last_sequence) ++s.discontinuities;
    else s.gaps+=sequence-s.last_sequence-1;
  }
  ++s.count; s.last_sequence=sequence;s.last_timestamp_ns=stamp;
}
struct Driver::Impl {
  Config config;
  orbvi_sdk::Client images, imu;
  orbvi_sdk::SubscriptionHandle image_subscription,imu_subscription;
  Callbacks callbacks;
  mutable std::mutex mutex;
  Stats counters;
  TimestampMatcher matcher;
  bool active=false;
  explicit Impl(Config c):config(std::move(c)) { validate(config); }
  void frame(const orbvi_sdk::FrameView& f) {
    if(f.camera_index>=4) return;
    const bool valid=f.valid()&&f.crc_valid&&f.sensor_timestamp_ns>0&&
      f.pixel_format==orbvi_sdk::PixelFormat::bgr8&&f.width==unsigned(config.width)&&f.height==unsigned(config.height)&&
      f.stride>=f.width*3 && f.size>=size_t(f.stride)*(f.height-1)+f.width*3;
    if(!valid) { std::lock_guard<std::mutex> lock(mutex); ++counters.cameras[f.camera_index].invalid; return; }
    {
      std::lock_guard<std::mutex> lock(mutex);
      if(detail::discard_duplicate(counters.cameras[f.camera_index],f.sequence,f.sensor_timestamp_ns))return;
    }
    Image out;out.camera=f.camera_index;out.sequence=f.sequence;out.timestamp_ns=f.sensor_timestamp_ns;
    out.width=f.width;out.height=f.height;out.rgb.resize(size_t(f.width)*f.height*3);
    for(unsigned y=0;y<f.height;++y) {
      const auto* src=f.data+size_t(y)*f.stride;auto* dst=out.rgb.data()+size_t(y)*f.width*3;unsigned x=0;
#if defined(__aarch64__)
      for(;x+16<=f.width;x+=16) { auto p=vld3q_u8(src+size_t(x)*3);std::swap(p.val[0],p.val[2]);vst3q_u8(dst+size_t(x)*3,p); }
#endif
      for(;x<f.width;++x) { dst[x*3]=src[x*3+2];dst[x*3+1]=src[x*3+1];dst[x*3+2]=src[x*3]; }
    }
    {
      std::lock_guard<std::mutex> lock(mutex);observe(counters.cameras[f.camera_index],f.sequence,f.sensor_timestamp_ns,true);
      auto group=matcher.add(f.camera_index,f.sensor_timestamp_ns);
      if(group) {
        ++counters.sync_groups;counters.last_camera_skew_ns=group->skew_ns;
        if(group->skew_ns>=1000000)++counters.sync_skew_errors;
        counters.max_camera_skew_ns=std::max(counters.max_camera_skew_ns,group->skew_ns);
      }
      counters.sync_unmatched_frames=matcher.unmatched;
    }
    if(callbacks.image) callbacks.image(std::move(out));
  }
  void sample(const orbvi_sdk::ImuEvent& event) {
    const auto& s=event.sample;Imu out;
    out.sequence=s.sequence;out.accel_timestamp_ns=s.accel_timestamp_ns;out.gyro_timestamp_ns=s.gyro_timestamp_ns;
    out.timestamp_ns=std::min(s.accel_timestamp_ns,s.gyro_timestamp_ns);
    out.acceleration={s.accel_x_mps2,s.accel_y_mps2,s.accel_z_mps2};out.angular_velocity={s.gyro_x_radps,s.gyro_y_radps,s.gyro_z_radps};
    bool valid=s.crc_valid&&out.accel_timestamp_ns>0&&out.gyro_timestamp_ns>0;
    for(double x:out.acceleration)valid=valid&&std::isfinite(x);for(double x:out.angular_velocity)valid=valid&&std::isfinite(x);
    {std::lock_guard<std::mutex> lock(mutex);
      if(!valid) {++counters.imu.invalid;return;}
      observe(counters.imu,out.sequence,out.timestamp_ns,false);
      if(std::abs(double(out.accel_timestamp_ns-out.gyro_timestamp_ns))>1.5e9/config.frequency_hz)++counters.imu_pairing_errors;
    }
    if(callbacks.imu)callbacks.imu(std::move(out));
  }
};
Driver::Driver(Config c):impl_(std::make_unique<Impl>(std::move(c))) {}
Driver::~Driver() {stop();}
void Driver::start(Callbacks callbacks) {
  auto& p=*impl_;if(p.active)throw std::runtime_error("driver already started");
  handshake(p.config);
  {std::lock_guard<std::mutex> lock(p.mutex);p.counters={};p.matcher=TimestampMatcher{};}
  p.callbacks=std::move(callbacks);
  orbvi_sdk::RtspConfig r;for(unsigned i=0;i<4;++i)r.urls[i]="rtsp://"+p.config.host+":"+std::to_string(p.config.rtsp_base_port+i)+p.config.rtsp_path;
  p.images=orbvi_sdk::make_rtsp_client(r,orbvi_sdk::PixelFormat::bgr8);
  orbvi_sdk::UdpImuConfig u;u.bind_address=p.config.imu_bind_address;u.port=uint16_t(p.config.imu_port);
  p.imu=orbvi_sdk::make_udp_imu_client(u);
  try {
    orbvi_sdk::CallbackSet image_cb,imu_cb;image_cb.on_frame=[&p](const auto& f){p.frame(f);};imu_cb.on_imu=[&p](const auto& s){p.sample(s);};
    auto a=p.images.subscribe({size_t(p.config.image_queue),orbvi_sdk::DropPolicy::drop_oldest},std::move(image_cb));
    if(!a.ok())throw std::runtime_error(a.status().to_string());p.image_subscription=std::move(a).value();
    auto b=p.imu.subscribe({size_t(p.config.imu_queue),orbvi_sdk::DropPolicy::drop_oldest},std::move(imu_cb));
    if(!b.ok())throw std::runtime_error(b.status().to_string());p.imu_subscription=std::move(b).value();
    require(p.imu.open());require(p.imu.start());require(p.images.open());require(p.images.start());p.active=true;
  } catch(...) {stop();throw;}
}
void Driver::stop() noexcept {
  auto& p=*impl_;p.image_subscription.cancel();p.imu_subscription.cancel();
  static_cast<void>(p.images.stop());static_cast<void>(p.images.close());
  static_cast<void>(p.imu.stop());static_cast<void>(p.imu.close());p.active=false;
}
Stats Driver::stats() const {
  const auto& p=*impl_;Stats s;{std::lock_guard<std::mutex> lock(p.mutex);s=p.counters;}
  auto a=p.image_subscription.stats(),b=p.imu_subscription.stats();
  s.image_callback_drops=a.dropped;s.imu_callback_drops=b.dropped;s.callback_failures=a.failed+b.failed;
  auto x=p.images.diagnostics(),y=p.imu.diagnostics();
  if(!x.last_error.empty())s.error=x.last_error;if(!y.last_error.empty())s.error+=y.last_error;
  return s;
}
bool capture_report(const Config& c,const Stats& before,const Stats& after,double seconds,std::string& json) {
  bool pass=seconds>0;std::ostringstream s;s<<"{\"wall_seconds\":"<<seconds<<",\"camera\":[";
  auto report=[&](const StreamStats& a,const StreamStats& b,double expected,double tolerance) {
    auto n=b.count-a.count,gaps=b.gaps-a.gaps,invalid=b.invalid-a.invalid,disc=b.discontinuities-a.discontinuities;
    double hz=b.last_timestamp_ns>a.last_timestamp_ns?double(n)*1e9/(b.last_timestamp_ns-a.last_timestamp_ns):0;
    bool ok=n>0&&a.count>0&&std::abs(hz-expected)<=expected*tolerance&&!gaps&&!invalid&&!disc&&double(n)/seconds>=expected*(1-tolerance);
    pass=pass&&ok;
    s<<"{\"count\":"<<n<<",\"rate_hz\":"<<hz<<",\"gaps\":"<<gaps<<",\"invalid\":"<<invalid<<",\"discontinuities\":"<<disc<<",\"duplicates_filtered\":"<<(b.duplicates_filtered-a.duplicates_filtered)<<",\"pass\":"<<(ok?"true":"false")<<'}';
  };
  for(unsigned i=0;i<4;++i){if(i)s<<',';report(before.cameras[i],after.cameras[i],c.fps,.02);}
  s<<"],\"imu\":";report(before.imu,after.imu,c.frequency_hz,.05);
  auto drops=after.image_callback_drops-before.image_callback_drops+after.imu_callback_drops-before.imu_callback_drops;
  auto failures=after.callback_failures-before.callback_failures;
  auto pair_errors=after.imu_pairing_errors-before.imu_pairing_errors;
  auto skew_errors=after.sync_skew_errors-before.sync_skew_errors;
  auto unmatched=after.sync_unmatched_frames-before.sync_unmatched_frames;
  auto groups=after.sync_groups-before.sync_groups;
  pass=pass&&!drops&&!failures&&!pair_errors&&!skew_errors&&!unmatched&&double(groups)>=c.fps*seconds*.97&&after.error.empty();
  s<<",\"callback_drops\":"<<drops<<",\"callback_failures\":"<<failures<<",\"imu_pairing_errors\":"<<pair_errors
   <<",\"sync_groups\":"<<groups<<",\"sync_unmatched_frames\":"<<unmatched<<",\"sync_skew_errors\":"<<skew_errors<<",\"last_camera_skew_ns\":"<<after.last_camera_skew_ns<<",\"max_camera_skew_ns_since_start\":"<<after.max_camera_skew_ns
   <<",\"error\":"<<json_quote(after.error)<<",\"pass\":"<<(pass?"true":"false")<<'}';json=s.str();return pass;
}
}
