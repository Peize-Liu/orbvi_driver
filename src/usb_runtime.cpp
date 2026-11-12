#include "usb_runtime.hpp"
#include "usb_device.hpp"
#include "orbvi_driver/usb_protocol.hpp"
#include "handshake.hpp"
#include <orbvi_sdk/udp_imu.hpp>
#include "h264_frame_metadata.hpp"
#include "rtsp_frame_converter.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace orbvi_driver {
namespace {
class Decoder {
  AVCodecContext* codec_=nullptr;AVFrame* frame_=nullptr;AVFrame* pixels_=nullptr;AVPacket* packet_=nullptr;
  orbvi_sdk::detail::RtspFrameConverter converter_;
 public:
  Decoder(){
    const auto* c=avcodec_find_decoder(AV_CODEC_ID_H264);if(!c)throw std::runtime_error("H264 decoder unavailable");
    codec_=avcodec_alloc_context3(c);frame_=av_frame_alloc();pixels_=av_frame_alloc();packet_=av_packet_alloc();
    if(!codec_||!frame_||!pixels_||!packet_){clear();throw std::runtime_error("USB decoder allocation failed");}
    codec_->thread_count=1;
    if(avcodec_open2(codec_,c,nullptr)<0){clear();throw std::runtime_error("USB decoder open failed");}
  }
  void clear(){av_packet_free(&packet_);av_frame_free(&pixels_);av_frame_free(&frame_);avcodec_free_context(&codec_);}
  ~Decoder(){clear();}
  template<class F> void decode(const usb::Message& m,unsigned index,const Config& config,F emit){
    if(av_new_packet(packet_,int(m.payload.size()-4))<0)throw std::runtime_error("USB packet allocation failed");
    std::memcpy(packet_->data,m.payload.data()+4,m.payload.size()-4);
    packet_->pts=packet_->dts=m.timestamp;
    int status=avcodec_send_packet(codec_,packet_);av_packet_unref(packet_);
    if(status<0)throw std::runtime_error("USB H264 packet rejected");
    unsigned decoded=0;
    while((status=avcodec_receive_frame(codec_,frame_))>=0){
      ++decoded;orbvi_sdk::H264FrameMetadata meta;
      if(frame_->width!=config.width||frame_->height!=config.height||
         !orbvi_sdk::h264_frame_metadata(frame_,index,meta)||meta.timestamp_ns!=int64_t(m.timestamp)||meta.frame_id!=m.sequence)
        throw std::runtime_error("USB decoded frame metadata/dimensions mismatch");
      auto converted=converter_.convert(frame_,pixels_,frame_->width,frame_->height,orbvi_sdk::PixelFormat::bgr8);
      if(!converted.ok())throw std::runtime_error(converted.to_string());
      orbvi_sdk::FrameView view;view.data=pixels_->data[0];view.size=size_t(pixels_->linesize[0])*frame_->height;
      view.width=frame_->width;view.height=frame_->height;view.stride=pixels_->linesize[0];view.camera_index=index;
      view.sequence=meta.frame_id;view.sensor_timestamp_ns=meta.timestamp_ns;view.timestamp_ns=meta.timestamp_ns;
      view.pixel_format=orbvi_sdk::PixelFormat::bgr8;view.layout=orbvi_sdk::FrameLayout::single_plane;view.crc_valid=true;
      emit(view);av_frame_unref(frame_);
    }
    if(status!=AVERROR(EAGAIN)||decoded!=1)throw std::runtime_error("USB requires one decoded all-intra picture per access unit");
  }
};
int connect_bridge(const Config& c){
  addrinfo hints{},*list=nullptr;hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_INET;
  if(getaddrinfo(c.host.c_str(),std::to_string(c.usb_port).c_str(),&hints,&list))throw std::runtime_error("USB bridge host resolution failed");
  int fd=-1;
  for(auto* a=list;a;a=a->ai_next){
    fd=socket(a->ai_family,SOCK_STREAM,0);if(fd<0)continue;
    fcntl(fd,F_SETFL,O_NONBLOCK);int rc=connect(fd,a->ai_addr,a->ai_addrlen);
    if(rc<0&&errno==EINPROGRESS){pollfd p{fd,POLLOUT,0};int error=0;socklen_t n=sizeof(error);
      rc=(poll(&p,1,3000)>0&&!getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)&&!error)?0:-1;}
    if(!rc)break;close(fd);fd=-1;
  }
  freeaddrinfo(list);if(fd<0)throw std::runtime_error("USB bridge unavailable; start orbvi_usb_bridge on the USB host");return fd;
}
}
struct UsbRuntime::Impl {
  Config config;std::function<void(const orbvi_sdk::FrameView&)> image;std::function<void(const orbvi_sdk::ImuEvent&)> imu;
  mutable std::mutex mutex;std::condition_variable ready;std::atomic<bool> done{false};bool verified=false;
  std::array<std::deque<usb::Message>,5> queues;std::array<std::thread,5> workers;std::thread reader;UsbStats counters;
  Impl(Config c,decltype(image) f,decltype(imu) i):config(std::move(c)),image(std::move(f)),imu(std::move(i)){}
  void fail(const std::string& e){std::lock_guard<std::mutex> lock(mutex);if(counters.error.empty())counters.error=e;done=true;ready.notify_all();}
  void enqueue(usb::Message m){
    if(m.type==usb::config){
      auto root=YAML::Load(std::string(m.payload.begin(),m.payload.end()));verify_configuration(config,root);
      if(root["usb_protocol"].as<int>()!=1)throw std::runtime_error("USB driver protocol mismatch");
      std::lock_guard<std::mutex> lock(mutex);counters.mux_drops=root["usb_mux_dropped"].as<uint64_t>();
      counters.mux_invalid=root["usb_mux_invalid"].as<uint64_t>();verified=true;ready.notify_all();return;
    }
    unsigned index=4;
    if(m.type==usb::camera){if(m.payload.size()<5||(index=unsigned(usb::le(m.payload.data(),4)))>=4)throw std::runtime_error("invalid USB camera payload");}
    else if(m.type!=usb::imu)throw std::runtime_error("unsupported USB driver message type");
    std::lock_guard<std::mutex> lock(mutex);if(!verified)return;
    auto& q=queues[index];size_t limit=index==4?config.imu_queue:config.image_queue;
    if(q.size()>=limit){q.pop_front();if(index==4)++counters.imu_drops;else ++counters.image_drops;}
    q.push_back(std::move(m));ready.notify_all();
  }
  void receive(){
    int fd=-1;
    try{
      std::unique_ptr<usb::Device> device;
      if(config.usb_backend=="direct")device=std::make_unique<usb::Device>(config.usb_serial);else fd=connect_bridge(config);
      usb::Parser parser{bool(device)};std::vector<uint8_t> bytes(1024*1024);
      auto last=std::chrono::steady_clock::now();
      while(!done){int n=0;
        if(device)n=device->read(bytes.data(),bytes.size());
        else {pollfd p{fd,POLLIN,0};int r=poll(&p,1,200);if(r<0&&errno!=EINTR)throw std::runtime_error("USB bridge poll failed");
          if(r>0){n=int(recv(fd,bytes.data(),bytes.size(),0));if(n==0)throw std::runtime_error("USB bridge disconnected");
            if(n<0&&errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("USB bridge receive failed");}}
        if(n>0){last=std::chrono::steady_clock::now();parser.feed(bytes.data(),n,[&](usb::Message m){enqueue(std::move(m));});}
        else if(std::chrono::steady_clock::now()-last>std::chrono::seconds(5))throw std::runtime_error("USB data timed out");
      }
    }catch(const std::exception& e){if(!done)fail(e.what());}
    if(fd>=0)close(fd);
  }
  void consume(unsigned index){
    try{
      std::unique_ptr<Decoder> decoder;if(index<4)decoder=std::make_unique<Decoder>();
      while(!done){usb::Message m;
        {std::unique_lock<std::mutex> lock(mutex);ready.wait(lock,[&]{return done||!queues[index].empty();});if(done)break;
          m=std::move(queues[index].front());queues[index].pop_front();}
        if(decoder)decoder->decode(m,index,config,image);
        else {auto result=orbvi_sdk::parse_oimu_packet(m.payload.data(),m.payload.size(),int64_t(m.timestamp));
          if(!result.ok())throw std::runtime_error(result.status().to_string());orbvi_sdk::ImuEvent event;event.sample=std::move(result).value();
          if(uint32_t(event.sample.sequence)!=m.sequence||uint64_t(event.sample.sensor_timestamp_ns)!=m.timestamp)throw std::runtime_error("USB IMU envelope mismatch");imu(event);}
      }
    }catch(const std::exception& e){fail(e.what());}catch(...){fail("USB callback failed");}
  }
};
UsbRuntime::UsbRuntime(const Config& c,std::function<void(const orbvi_sdk::FrameView&)> f,std::function<void(const orbvi_sdk::ImuEvent&)> i):p_(new Impl(c,std::move(f),std::move(i))){}
UsbRuntime::~UsbRuntime(){stop();}
void UsbRuntime::start(){auto& p=*p_;try{
  for(unsigned i=0;i<5;++i)p.workers[i]=std::thread([&p,i]{p.consume(i);});p.reader=std::thread([&p]{p.receive();});
  std::unique_lock<std::mutex> lock(p.mutex);
  if(!p.ready.wait_for(lock,std::chrono::seconds(8),[&]{return p.verified||p.done;}))throw std::runtime_error("USB configuration handshake timed out");
  if(p.done)throw std::runtime_error(p.counters.error);
}catch(...){stop();throw;}}
void UsbRuntime::stop()noexcept{auto& p=*p_;p.done=true;p.ready.notify_all();if(p.reader.joinable())p.reader.join();for(auto& t:p.workers)if(t.joinable())t.join();}
UsbStats UsbRuntime::stats()const{std::lock_guard<std::mutex> lock(p_->mutex);return p_->counters;}
}
