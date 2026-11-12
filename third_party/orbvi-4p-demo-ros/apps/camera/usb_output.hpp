#pragma once
#include "frame_metadata.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <vector>
namespace orbvi::camera {
class UsbOutput {
  int fd_=-1;uint64_t drops_=0;
  std::chrono::steady_clock::time_point retry_{};
  void close(){if(fd_>=0)::close(fd_);fd_=-1;}
 public:
  ~UsbOutput(){close();if(drops_)std::fprintf(stderr,"USB camera publisher dropped=%llu\n",(unsigned long long)drops_);}
  void push(const void* data,size_t size,const FrameMetadata& meta) {
    const char* path=std::getenv("ORBVI_USB_CAMERA_SOCKET");if(!path||!*path)return;
    if(fd_<0){
      auto now=std::chrono::steady_clock::now();if(now<retry_){++drops_;return;}
      retry_=now+std::chrono::seconds(1);sockaddr_un addr{};addr.sun_family=AF_UNIX;
      if(std::strlen(path)>=sizeof(addr.sun_path)){++drops_;return;}std::strcpy(addr.sun_path,path);
      fd_=::socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
      if(fd_<0||::connect(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))){close();++drops_;return;}
      fcntl(fd_,F_SETFL,O_NONBLOCK);
    }
    auto sei=frame_metadata_sei(meta);size_t length=4+sei.size()+size;
    if(length>4*1024*1024){++drops_;return;}
    std::vector<uint8_t> frame(24+length,0);
    std::memcpy(frame.data(),"X5US",4);frame[4]=1;frame[5]=0x10;
    auto put=[&](size_t off,uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)frame[off+i]=uint8_t(v>>(8*i));};
    put(8,length,4);put(12,meta.frame_id,4);put(16,meta.timestamp_ns,8);put(24,meta.camera_id,4);
    std::memcpy(frame.data()+28,sei.data(),sei.size());std::memcpy(frame.data()+28+sei.size(),data,size);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(5);
    size_t sent=0;while(sent<frame.size()){
      auto n=::send(fd_,frame.data()+sent,frame.size()-sent,MSG_NOSIGNAL|MSG_DONTWAIT);
      if(n<0&&errno==EINTR)continue;
      if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&std::chrono::steady_clock::now()<deadline){pollfd p{fd_,POLLOUT,0};poll(&p,1,1);continue;}
      if(n<=0){close();++drops_;return;}sent+=size_t(n);
    }
  }
};
}
