#include "orbvi_driver/board.hpp"
#include "control_server.hpp"
#include <cerrno>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace { volatile sig_atomic_t stopping=0; void stop(int) { stopping=1; } }
int main(int argc,char** argv) {
  try {
    if(argc<2) throw std::runtime_error("usage: orbvi_board probe | check CONFIG | run CONFIG SENSOR_EXECUTABLE");
    const std::string mode=argv[1];
    if(mode=="transport"&&argc==3){std::cout<<orbvi_driver::load_config(argv[2]).transport_mode<<'\n';return 0;}
    auto devices=orbvi_driver::probe_imu();
    if(mode=="probe"&&argc==2) { std::cout<<orbvi_driver::capabilities_json(devices)<<'\n';return 0; }
    if((mode=="check"&&argc!=3)||(mode=="run"&&argc!=4)||(mode!="check"&&mode!="run"))
      throw std::runtime_error("invalid command arguments");
    const auto c=orbvi_driver::load_config(argv[2]); orbvi_driver::check_board(c,devices);
    if(mode=="check") { std::cout<<"configuration supported by exposed BSP capabilities\n";return 0; }
    orbvi_driver::BoardSettings settings(c,devices);
    if(c.transport_mode=="usb") {
      std::string path=argv[2];auto slash=path.find_last_of('/');
      path=(slash==std::string::npos?".":path.substr(0,slash))+"/camera.sock";
      setenv("ORBVI_USB_CAMERA_SOCKET",path.c_str(),1);
    }else unsetenv("ORBVI_USB_CAMERA_SOCKET");
    ControlServer server;
    std::vector<std::string> args{argv[3],"--external-sync","--channels","4","--width",std::to_string(c.width),
      "--height",std::to_string(c.height),"--fps",std::to_string(c.fps),"--bps",std::to_string(c.bitrate_kbps),
      "--sample-rate-hz",std::to_string(int(c.frequency_hz)),"--print-rate-hz","0","--udp-dest","127.0.0.1:5601",
      "--diagnostics","--print-metrics","--frame-timeout-ms","1000"};
    std::vector<char*> raw; for(auto& arg:args) raw.push_back(arg.data()); raw.push_back(nullptr);
    struct sigaction action{}; action.sa_handler=stop; sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,nullptr); sigaction(SIGTERM,&action,nullptr);
    const pid_t child=fork(); if(child<0) throw std::runtime_error("fork failed");
    if(child==0) { execv(raw[0],raw.data()); _exit(127); }
    try {server.start(c);}catch(...){kill(child,SIGTERM);waitpid(child,nullptr,0);throw;}
    int status=0; bool sent=false;
    while(true) {
      if(stopping&&!sent) { kill(child,SIGTERM);sent=true; }
      auto result=waitpid(child,&status,WNOHANG);
      if(result==child) break;
      if(result<0&&errno!=EINTR) throw std::runtime_error("waitpid failed");
      usleep(10000);
    }
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
  } catch(const std::exception& e) { std::cerr<<"orbvi_board: "<<e.what()<<'\n';return 1; }
}
