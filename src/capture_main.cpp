#include "orbvi_driver/driver.hpp"
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {volatile sig_atomic_t stopping=0;void stop(int){stopping=1;}}
int main(int argc,char** argv) {
  try {
    if(argc!=3&&argc!=4)throw std::runtime_error("usage: orbvi_capture CONFIG SECONDS [WARMUP_SECONDS]");
    const auto c=orbvi_driver::load_config(argv[1]);double seconds=std::stod(argv[2]),warmup=argc==4?std::stod(argv[3]):3;
    if(!(seconds>=1&&seconds<=3600&&warmup>=0&&warmup<=60))throw std::runtime_error("invalid duration");
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);orbvi_driver::Driver driver(c);driver.start();
    auto wait=[&](double n){const auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(n);
      while(!stopping&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(10));};
    wait(warmup);auto baseline=driver.stats();auto start=std::chrono::steady_clock::now();wait(seconds);
    auto end=driver.stats();auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    driver.stop();std::string report;bool pass=orbvi_driver::capture_report(c,baseline,end,elapsed,report);
    std::cout<<report<<'\n';return pass&&!stopping?0:1;
  }catch(const std::exception& e){std::cerr<<"orbvi_capture: "<<e.what()<<'\n';return 2;}
}
