#include "orbvi_driver/board.hpp"
#include "orbvi_driver/synchronizer.hpp"
#include "../src/duplicate_filter.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace orbvi_driver;
namespace fs=std::filesystem;
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
template<class F> void rejects(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}check(threw,"expected rejection");}
int main(int argc,char** argv) {
  fs::path root=fs::temp_directory_path()/("orbvi-config-test-"+std::to_string(getpid()));
  try {
    check(argc==2,"config path required");auto c=load_config(argv[1]);
    StreamStats previous;previous.count=1;previous.last_sequence=7799;previous.last_timestamp_ns=1000000000;
    check(detail::discard_duplicate(previous,7799,1000000000),"BSP repeated frame not filtered");
    check(previous.duplicates_filtered==1,"duplicate not reported");
    check(!detail::discard_duplicate(previous,7799,1016666667),"changed timestamp concealed");
    check(!detail::discard_duplicate(previous,7800,1000000000),"changed sequence concealed");
    check(!detail::discard_duplicate(previous,0,100000000),"source restart concealed");
    check(!detail::discard_duplicate(previous,7801,1033333334),"sequence gap concealed");
    TimestampMatcher matcher(1000000,100000000);
    for(unsigned i=0;i<3;++i)check(!matcher.add(i,1000000000+i*40000),"incomplete group emitted");
    auto group=matcher.add(3,1000120000);check(group&&group->skew_ns==120000,"shared-clock grouping failed");
    for(unsigned i=0;i<3;++i)matcher.add(i,2000000000+i*40000);
    check(!matcher.add(3,2005000000),"out-of-phase camera incorrectly grouped");
    matcher.add(0,2200000000);check(matcher.unmatched==4,"unmatched frames concealed");
    for(auto size:camera_sizes())for(auto fps:camera_rates()){auto v=c;v.width=size.first;v.height=size.second;v.fps=fps;validate(v);}
    auto bad=c;bad.fps=31;rejects([&]{validate(bad);});bad=c;bad.width=800;rejects([&]{validate(bad);});
    bad=c;bad.width=1088;bad.height=1280;rejects([&]{validate(bad);});
    bad.width=720;rejects([&]{validate(bad);});
    fs::create_directories(root);std::ifstream file(argv[1]);std::string text{std::istreambuf_iterator<char>(file),{}};
    {std::ofstream f(root/"bad.yaml");f<<text<<"unknown: 1\n";}rejects([&]{load_config((root/"bad.yaml").string());});
    {std::ofstream f(root/"bad.yaml");f<<text<<"schema_version: 1\n";}rejects([&]{load_config((root/"bad.yaml").string());});
    for(int i=0;i<2;++i) {auto d=root/("iio:device"+std::to_string(i));fs::create_directories(d/"buffer");
      std::ofstream(d/"name")<<(i?"icm42688-accel":"icm42688-gyro");
      std::ofstream(d/"sampling_frequency")<<1000;std::ofstream(d/"sampling_frequency_available")<<"500 1000";
      std::ofstream(d/(i?"in_accel_scale":"in_anglvel_scale"))<<(i?"0.004788403":"0.001065264");
      std::ofstream(d/(i?"in_accel_scale_available":"in_anglvel_scale_available"))<<(i?"0.004788403":"0.001065264");
      std::ofstream(d/"buffer/enable")<<0;
    }
    auto devices=probe_imu(root);check_board(c,devices);
    auto requested=c;requested.accel_range_g=4;rejects([&]{BoardSettings settings(requested,devices);});
    check(probe_imu(root)[0].frequency==1000,"rejection changed hardware");
    requested=c;requested.frequency_hz=500;
    {BoardSettings settings(requested,devices);for(auto d:probe_imu(root))check(d.frequency==500,"frequency readback");}
    for(auto d:probe_imu(root))check(d.frequency==1000,"restore failed");
    std::ofstream(root/"iio:device0/buffer/enable")<<1;rejects([&]{BoardSettings settings(c,devices);});
    requested=c;requested.frequency_hz=200;rejects([&]{check_board(requested,devices);});
    fs::remove_all(root);std::cout<<"PASS: config schema, presets, capability rejection, readback, restore, busy guard\n";return 0;
  }catch(const std::exception& e){fs::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
