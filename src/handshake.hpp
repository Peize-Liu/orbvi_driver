#pragma once
#include "orbvi_driver/config.hpp"
#include <yaml-cpp/yaml.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <stdexcept>
namespace orbvi_driver {
inline void verify_configuration(const Config& c,const YAML::Node& root) {
  auto equal=[&](const YAML::Node& node,const char* key,double expected,double tol=1e-6){
    const auto value=node[key].as<double>();if(!std::isfinite(value)||std::abs(value-expected)>std::max(tol,std::abs(expected)*tol))throw std::runtime_error(std::string("board configuration mismatch: ")+key);};
  equal(root,"schema_version",1);equal(root,"width",c.width);equal(root,"height",c.height);equal(root,"fps",c.fps);equal(root,"bitrate_kbps",c.bitrate_kbps);
  equal(root,"imu_frequency_hz",c.frequency_hz);equal(root,"accel_range_g",c.accel_range_g);equal(root,"gyro_range_dps",c.gyro_range_dps);
  bool accel=false,gyro=false;
  for(auto device:root["capabilities"]["imu"]) {
    equal(device,"frequency_hz",c.frequency_hz);
    if(device["name"].as<std::string>()=="icm42688-gyro") {equal(device,"scale",c.gyro_range_dps*3.141592653589793/180/32768,1e-8);gyro=true;}
    else if(device["name"].as<std::string>()=="icm42688-accel") {equal(device,"scale",c.accel_range_g*9.80665/32768,1e-8);accel=true;}
  }
  if(!accel||!gyro)throw std::runtime_error("board handshake missing IMU devices");
  if(root["transport_mode"]&&root["transport_mode"].as<std::string>()!=c.transport_mode)throw std::runtime_error("board transport mode mismatch");
}
inline void handshake(const Config& c) {
  addrinfo hints{},*addresses=nullptr;hints.ai_socktype=SOCK_STREAM;hints.ai_family=AF_INET;
  const auto port=std::to_string(c.control_port);
  if(getaddrinfo(c.host.c_str(),port.c_str(),&hints,&addresses))throw std::runtime_error("cannot resolve board control host");
  int fd=-1;
  for(auto* a=addresses;a;a=a->ai_next) {
    fd=socket(a->ai_family,SOCK_STREAM,0);if(fd<0)continue;
    fcntl(fd,F_SETFL,O_NONBLOCK);int rc=connect(fd,a->ai_addr,a->ai_addrlen);
    if(rc<0&&errno==EINPROGRESS){pollfd p{fd,POLLOUT,0};rc=poll(&p,1,3000);int error=0;socklen_t len=sizeof(error);
      if(rc>0&&getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)==0&&error==0)rc=0;else rc=-1;}
    if(rc==0){fcntl(fd,F_SETFL,0);break;}close(fd);fd=-1;
  }
  freeaddrinfo(addresses);if(fd<0)throw std::runtime_error("board config handshake unavailable; start board with this package and forward control port");
  struct Guard{int fd;~Guard(){close(fd);}} guard{fd};
  timeval timeout{3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
  const std::string request="GET /config HTTP/1.0\r\n\r\n";
  if(send(fd,request.data(),request.size(),0)!=ssize_t(request.size()))throw std::runtime_error("control request failed");
  std::string response;char buffer[4096];ssize_t n;
  while((n=recv(fd,buffer,sizeof(buffer),0))>0){response.append(buffer,size_t(n));if(response.size()>65536)throw std::runtime_error("control response too large");}
  auto split=response.find("\r\n\r\n");if(n<0||split==std::string::npos||response.find("HTTP/1.0 200 OK")!=0)throw std::runtime_error("board config handshake failed");
  verify_configuration(c,YAML::Load(response.substr(split+4)));
}
}
