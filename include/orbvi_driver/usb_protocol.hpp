#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace orbvi_driver::usb {
// X5US v1 envelope, with ORBVI driver extension message types.
constexpr uint8_t camera=0x10, imu=0x11, config=0x12;
constexpr size_t header_size=24, max_payload=4*1024*1024;
inline uint64_t le(const uint8_t* p,unsigned n){uint64_t v=0;for(unsigned i=0;i<n;++i)v|=uint64_t(p[i])<<(i*8);return v;}
struct Message {uint8_t type=0;uint32_t sequence=0;uint64_t timestamp=0;std::vector<uint8_t> payload;};
class Parser {
  std::vector<uint8_t> bytes_;
  bool seeking_;
 public:
  explicit Parser(bool seek_initial_header=false):seeking_(seek_initial_header){}
  template<class F> void feed(const uint8_t* data,size_t size,F emit) {
    if(size>max_payload+header_size)throw std::runtime_error("USB input chunk exceeds bound");
    bytes_.insert(bytes_.end(),data,data+size);size_t offset=0;
    while(bytes_.size()-offset>=header_size) {
      const auto* h=bytes_.data()+offset;
      if(seeking_){
        if(std::memcmp(h,"X5US",4)||h[4]!=1||h[6]||h[7]||
           (h[5]!=camera&&h[5]!=imu&&h[5]!=config)||!le(h+8,4)||le(h+8,4)>max_payload||!le(h+16,8)) {++offset;continue;}
        seeking_=false;
      }
      if(std::memcmp(h,"X5US",4)||h[4]!=1||h[6]||h[7])throw std::runtime_error("invalid X5US header");
      auto length=le(h+8,4);if(length==0||length>max_payload)throw std::runtime_error("invalid USB payload length");
      if(bytes_.size()-offset<header_size+length)break;
      Message m;m.type=h[5];m.sequence=uint32_t(le(h+12,4));m.timestamp=le(h+16,8);
      m.payload.assign(h+header_size,h+header_size+length);emit(std::move(m));offset+=header_size+length;
    }
    if(offset)bytes_.erase(bytes_.begin(),bytes_.begin()+offset);
    if(bytes_.size()>max_payload+header_size)throw std::runtime_error("USB parser buffer exceeded");
  }
};
}
