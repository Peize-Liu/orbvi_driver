#include "usb_device.hpp"
#include "orbvi_driver/usb_protocol.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>
#include <deque>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
namespace {volatile sig_atomic_t stopped=0;void stop(int){stopped=1;}
struct Peer {
 int fd=-1,kind=-1;size_t queued=0,offset=0;std::deque<std::shared_ptr<std::vector<uint8_t>>> frames;
};
int listen_on(int port){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)throw std::runtime_error("bridge socket failed");
 int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(fd,8)){close(fd);throw std::runtime_error("USB bridge port busy: "+std::to_string(port));}fcntl(fd,F_SETFL,O_NONBLOCK);return fd;}
std::shared_ptr<std::vector<uint8_t>> encode(const orbvi_driver::usb::Message& m){
 auto v=std::make_shared<std::vector<uint8_t>>(24+m.payload.size(),0);auto* h=v->data();std::memcpy(h,"X5US",4);h[4]=1;h[5]=m.type;
 auto put=[&](unsigned offset,uint64_t value,unsigned n){for(unsigned i=0;i<n;++i)h[offset+i]=uint8_t(value>>(8*i));};
 put(8,m.payload.size(),4);put(12,m.sequence,4);put(16,m.timestamp,8);std::memcpy(h+24,m.payload.data(),m.payload.size());return v;
}}
int main(int argc,char** argv){
 std::vector<int> listeners;std::vector<Peer> peers;
 auto cleanup=[&]{for(auto& p:peers)if(p.fd>=0)close(p.fd);for(int fd:listeners)close(fd);};
 try {
  std::string serial;int port=15700;bool probe=false;
  for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--serial"&&i+1<argc)serial=argv[++i];else if(a=="--port"&&i+1<argc)port=std::stoi(argv[++i]);else if(a=="--probe")probe=true;else throw std::runtime_error("usage: orbvi_usb_bridge [--serial SERIAL] [--port PORT] [--probe]");}
  if(port<1024||port>65522)throw std::runtime_error("USB bridge port must be 1024..65522");
  signal(SIGINT,stop);signal(SIGTERM,stop);signal(SIGPIPE,SIG_IGN);
  orbvi_driver::usb::Device device(serial);
  std::cerr<<"ORBVI USB bulk interface="<<device.interface_number()<<" endpoint=0x"<<std::hex<<device.endpoint()<<std::dec<<'\n';
  if(probe)return 0;
  for(unsigned i=0;i<5;++i)listeners.push_back(listen_on(i?port+9+i:port));
  std::cerr<<"USB bridge ready on 127.0.0.1:"<<port<<"; H264 preview ports "<<port+10<<".."<<port+13<<'\n';
  orbvi_driver::usb::Parser parser(true);std::vector<uint8_t> input(256*1024);
  uint64_t received=0,slow=0;auto last=std::chrono::steady_clock::now();
  while(!stopped){
   for(unsigned i=0;i<listeners.size();++i){int fd=accept(listeners[i],nullptr,nullptr);if(fd>=0){
    if(peers.size()>=16){close(fd);continue;}fcntl(fd,F_SETFL,O_NONBLOCK);Peer p;p.fd=fd;p.kind=int(i)-1;peers.push_back(std::move(p));}}
   int n=device.read(input.data(),input.size());if(n>0){received+=n;parser.feed(input.data(),n,[&](orbvi_driver::usb::Message m){
    auto wire=encode(m);std::shared_ptr<std::vector<uint8_t>> h264;int camera=-2;
    if(m.type==orbvi_driver::usb::camera){if(m.payload.size()<5||(camera=int(orbvi_driver::usb::le(m.payload.data(),4)))>=4)throw std::runtime_error("invalid camera payload");
     h264=std::make_shared<std::vector<uint8_t>>(m.payload.begin()+4,m.payload.end());}
    for(auto& p:peers){if(p.fd<0||(p.kind!=-1&&p.kind!=camera))continue;auto data=p.kind==-1?wire:h264;
     if(p.queued+data->size()>8*1024*1024){close(p.fd);p.fd=-1;++slow;continue;}
     p.queued+=data->size();p.frames.push_back(data);}
   });}
   for(auto& p:peers){if(p.fd<0)continue;
    char byte;auto alive=recv(p.fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
    if(alive==0||(alive<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){close(p.fd);p.fd=-1;continue;}
    while(!p.frames.empty()){
     auto& bytes=*p.frames.front();auto count=send(p.fd,bytes.data()+p.offset,bytes.size()-p.offset,MSG_NOSIGNAL);
     if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))break;
     if(count<=0){close(p.fd);p.fd=-1;break;}
     p.queued-=count;p.offset+=count;if(p.offset==bytes.size()){p.frames.pop_front();p.offset=0;}
    }
   }
   peers.erase(std::remove_if(peers.begin(),peers.end(),[](const Peer& p){return p.fd<0;}),peers.end());
   if(std::chrono::steady_clock::now()-last>std::chrono::seconds(10)){std::cerr<<"USB bytes="<<received<<" clients="<<peers.size()<<" slow_disconnects="<<slow<<'\n';last=std::chrono::steady_clock::now();}
  }
  cleanup();return 0;
 }catch(const std::exception& e){cleanup();std::cerr<<"orbvi_usb_bridge: "<<e.what()<<'\n';return 1;}
}
