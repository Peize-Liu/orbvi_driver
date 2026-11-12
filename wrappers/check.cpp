#include "capture_statistics.hpp"
#include <orbvi_driver/config.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#ifdef ORBVI_ROS2
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <orbvi_driver_ros2/msg/frame_metadata.hpp>
#include <orbvi_driver_ros2/msg/imu_metadata.hpp>
namespace sensor=sensor_msgs::msg;namespace metadata=orbvi_driver_ros2::msg;
#else
#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <orbvi_driver_ros1/FrameMetadata.h>
#include <orbvi_driver_ros1/ImuMetadata.h>
namespace sensor=sensor_msgs;namespace metadata=orbvi_driver_ros1;
#endif
template<class T> int64_t ns(const T& t){
#ifdef ORBVI_ROS2
  return int64_t(t.sec)*1000000000LL+t.nanosec;
#else
  return int64_t(t.sec)*1000000000LL+t.nsec;
#endif
}
using Clock=std::chrono::steady_clock;
int main(int argc,char** argv){
  try{
    if(argc!=3)throw std::runtime_error("usage: orbvi_ros_check CONFIG SECONDS");
    const auto config=orbvi_driver::load_config(argv[1]);int duration=std::stoi(argv[2]);
    if(duration<1||duration>3600)throw std::runtime_error("invalid duration");
    std::array<orbvi_bridge::CaptureStatistics,4> images;orbvi_bridge::CaptureStatistics imu;
    auto warm=Clock::now()+std::chrono::seconds(5),end=warm+std::chrono::seconds(duration+5);
    const int64_t window=int64_t(duration)*1000000000LL;
    auto image_cb=[&](unsigned i,const sensor::Image& m){images[i].add_data(ns(m.header.stamp),
      m.width==unsigned(config.width)&&m.height==unsigned(config.height)&&m.encoding=="rgb8"&&m.step==m.width*3&&m.data.size()==size_t(m.step)*m.height,
      Clock::now()>=warm);};
    auto frame_cb=[&](unsigned i,const metadata::FrameMetadata& m){images[i].add_metadata(ns(m.header.stamp),m.sequence,ns(m.header.stamp)==m.sensor_timestamp_ns,Clock::now()>=warm);};
    auto imu_cb=[&](const sensor::Imu& m){imu.add_data(ns(m.header.stamp),ns(m.header.stamp)>0&&std::isfinite(m.linear_acceleration.x)&&std::isfinite(m.linear_acceleration.y)&&std::isfinite(m.linear_acceleration.z)&&std::isfinite(m.angular_velocity.x)&&std::isfinite(m.angular_velocity.y)&&std::isfinite(m.angular_velocity.z),Clock::now()>=warm);};
    auto imu_meta_cb=[&](const metadata::ImuMetadata& m){imu.add_metadata(ns(m.header.stamp),m.sequence,ns(m.header.stamp)==std::min(m.accel_timestamp_ns,m.gyro_timestamp_ns),Clock::now()>=warm);};
#ifdef ORBVI_ROS2
    rclcpp::init(argc,argv);auto node=std::make_shared<rclcpp::Node>("orbvi_ros_check");
    std::vector<rclcpp::SubscriptionBase::SharedPtr> subscriptions;
    std::vector<rclcpp::CallbackGroup::SharedPtr> groups;
    for(unsigned i=0;i<4;++i){auto base=config.topic_prefix+"/camera_"+std::to_string(i);
      auto group=node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);groups.push_back(group);
      rclcpp::SubscriptionOptions options;options.callback_group=group;
      subscriptions.push_back(node->create_subscription<sensor::Image>(base+"/image",rclcpp::QoS(32).reliable(),[&,i](sensor::Image::ConstSharedPtr m){image_cb(i,*m);},options));
      subscriptions.push_back(node->create_subscription<metadata::FrameMetadata>(base+"/metadata",rclcpp::QoS(256).reliable(),[&,i](metadata::FrameMetadata::ConstSharedPtr m){frame_cb(i,*m);},options));
    }
    subscriptions.push_back(node->create_subscription<sensor::Imu>(config.topic_prefix+"/imu",rclcpp::QoS(8192).reliable(),[&](sensor::Imu::ConstSharedPtr m){imu_cb(*m);}));
    subscriptions.push_back(node->create_subscription<metadata::ImuMetadata>(config.topic_prefix+"/imu/metadata",rclcpp::QoS(8192).reliable(),[&](metadata::ImuMetadata::ConstSharedPtr m){imu_meta_cb(*m);}));
    rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(),4);executor.add_node(node);
    std::thread worker([&]{executor.spin();});
#else
    ros::init(argc,argv,"orbvi_ros_check");ros::NodeHandle node;std::vector<ros::Subscriber> subscriptions;
    for(unsigned i=0;i<4;++i){auto base=config.topic_prefix+"/camera_"+std::to_string(i);
      subscriptions.push_back(node.subscribe<sensor::Image>(base+"/image",32,[&,i](const sensor::Image::ConstPtr& m){image_cb(i,*m);},ros::VoidConstPtr(),ros::TransportHints().tcpNoDelay()));
      subscriptions.push_back(node.subscribe<metadata::FrameMetadata>(base+"/metadata",256,[&,i](const metadata::FrameMetadata::ConstPtr& m){frame_cb(i,*m);}));
    }
    subscriptions.push_back(node.subscribe<sensor::Imu>(config.topic_prefix+"/imu",8192,[&](const sensor::Imu::ConstPtr& m){imu_cb(*m);}));
    subscriptions.push_back(node.subscribe<metadata::ImuMetadata>(config.topic_prefix+"/imu/metadata",8192,[&](const metadata::ImuMetadata::ConstPtr& m){imu_meta_cb(*m);}));
    ros::AsyncSpinner spinner(4);spinner.start();
#endif
    auto complete=[&]{for(const auto& s:images)if(!s.complete(window))return false;return imu.complete(window);};
    while(Clock::now()<end&&!complete())std::this_thread::sleep_for(std::chrono::milliseconds(20));
#ifdef ORBVI_ROS2
    executor.cancel();worker.join();rclcpp::shutdown();
#else
    spinner.stop();ros::shutdown();
#endif
    bool pass=complete();std::ostringstream out;out<<"{\"seconds\":"<<duration<<",\"camera\":[";
    auto report=[&](const orbvi_bridge::CaptureSummary& s,double expected,double tolerance){
      const double rate=double(s.count)/duration;bool ok=std::abs(rate-expected)<=expected*tolerance&&s.count==s.metadata_count&&!s.gaps&&!s.discontinuities&&!s.invalid&&!s.missing_data&&!s.missing_metadata;
      pass=pass&&ok;out<<"{\"count\":"<<s.count<<",\"rate_hz\":"<<rate<<",\"metadata\":"<<s.metadata_count<<",\"gaps\":"<<s.gaps<<",\"discontinuities\":"<<s.discontinuities<<",\"invalid\":"<<s.invalid<<",\"missing_data\":"<<s.missing_data<<",\"missing_metadata\":"<<s.missing_metadata<<",\"pass\":"<<(ok?"true":"false")<<'}';};
    for(unsigned i=0;i<4;++i){if(i)out<<',';report(images[i].summarize(window,true),config.fps,.02);}
    out<<"],\"imu\":";report(imu.summarize(window,false),config.frequency_hz,.05);
    out<<",\"window_complete\":"<<(complete()?"true":"false")<<",\"pass\":"<<(pass?"true":"false")<<'}';std::cout<<out.str()<<'\n';return pass?0:1;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
