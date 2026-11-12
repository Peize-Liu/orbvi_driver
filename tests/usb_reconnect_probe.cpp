#include "orbvi_driver/driver.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
namespace {volatile sig_atomic_t stopped=0;void stop(int){stopped=1;}}
int main(int argc,char** argv){try{
 if(argc!=2)return 2;
 std::signal(SIGTERM,stop);std::signal(SIGINT,stop);
 orbvi_driver::Config config;config.transport_mode="usb";config.host="127.0.0.1";config.usb_port=std::stoi(argv[1]);
 orbvi_driver::Driver driver(config);driver.start();
 while(!stopped){auto stats=driver.stats();
  std::cout<<"{\"count\":"<<stats.imu.count<<",\"gaps\":"<<stats.imu.gaps<<",\"error\":"<<orbvi_driver::json_quote(stats.error)<<"}"<<std::endl;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
 }
 driver.stop();return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
