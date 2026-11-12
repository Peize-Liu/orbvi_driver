#pragma once
#include "orbvi_driver/board.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <sstream>
#include <stdexcept>

// Local-only, read-only configuration handshake. USB forwards this port.
// Created before fork, but the worker starts afterwards; CLOEXEC protects child.
class ControlServer {
  int fd_=-1;
  std::atomic<bool> done_{false};
  std::thread worker_;
 public:
  ControlServer() {
    fd_=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd_<0)throw std::runtime_error("control socket failed");
    int one=1;setsockopt(fd_,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(6562);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))||listen(fd_,4)) {close(fd_);fd_=-1;throw std::runtime_error("control port 6562 busy");}
  }
  void start(orbvi_driver::Config c) {
    worker_=std::thread([this,c] {
      while(!done_) {
        pollfd p{fd_,POLLIN,0};if(poll(&p,1,100)<=0)continue;
        int client=accept4(fd_,nullptr,nullptr,SOCK_CLOEXEC);if(client<0)continue;
        timeval timeout{1,0};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        char request[1024];auto size=recv(client,request,sizeof(request),0);
        if(size>0) {
          std::string body,status="200 OK";
          try {
            auto actual=orbvi_driver::probe_imu();orbvi_driver::check_board(c,actual);
            std::ostringstream s;s<<"{\"schema_version\":1,\"transport_mode\":"<<orbvi_driver::json_quote(c.transport_mode)<<",\"width\":"<<c.width<<",\"height\":"<<c.height<<",\"fps\":"<<c.fps
              <<",\"bitrate_kbps\":"<<c.bitrate_kbps<<",\"imu_frequency_hz\":"<<c.frequency_hz
              <<",\"accel_range_g\":"<<c.accel_range_g<<",\"gyro_range_dps\":"<<c.gyro_range_dps
              <<",\"capabilities\":"<<orbvi_driver::capabilities_json(actual)<<'}';body=s.str();
          }catch(const std::exception& e){status="503 Service Unavailable";body="{\"error\":"+orbvi_driver::json_quote(e.what())+"}";}
          const std::string response="HTTP/1.0 "+status+"\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
          size_t sent=0;while(sent<response.size()){auto n=send(client,response.data()+sent,response.size()-sent,MSG_NOSIGNAL);if(n<=0)break;sent+=size_t(n);}
        }
        close(client);
      }
    });
  }
  ~ControlServer(){done_=true;if(worker_.joinable())worker_.join();if(fd_>=0)close(fd_);}
};
