#include <orbvi_driver/driver.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#ifdef ORBVI_ROS2
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <orbvi_driver_ros2/msg/frame_metadata.hpp>
#include <orbvi_driver_ros2/msg/imu_metadata.hpp>
namespace sensor=sensor_msgs::msg;
namespace metadata=orbvi_driver_ros2::msg;
namespace diagnostic=diagnostic_msgs::msg;
template<class T> using Publisher=typename rclcpp::Publisher<T>::SharedPtr;
template<class T> void publish(Publisher<T>& p,T value){p->publish(std::move(value));}
#else
#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <diagnostic_msgs/DiagnosticArray.h>
#include <orbvi_driver_ros1/FrameMetadata.h>
#include <orbvi_driver_ros1/ImuMetadata.h>
namespace sensor=sensor_msgs;
namespace metadata=orbvi_driver_ros1;
namespace diagnostic=diagnostic_msgs;
template<class T> using Publisher=ros::Publisher;
template<class T> void publish(Publisher<T>& p,T value){p.publish(value);}
#endif
template<class Stamp> void stamp(Stamp& out,int64_t ns){
  if(ns<=0||ns/1000000000LL>2147483647LL)throw std::runtime_error("board timestamp outside ROS time range");
  out.sec=static_cast<decltype(out.sec)>(ns/1000000000LL);
#ifdef ORBVI_ROS2
  out.nanosec=uint32_t(ns%1000000000LL);
#else
  out.nsec=uint32_t(ns%1000000000LL);
#endif
}
int main(int argc,char** argv){
#ifdef ORBVI_ROS2
  rclcpp::init(argc,argv);auto node=std::make_shared<rclcpp::Node>("orbvi_driver");
  auto config_path=node->declare_parameter<std::string>("config","");
#else
  ros::init(argc,argv,"orbvi_driver");ros::NodeHandle node,private_node("~");std::string config_path;
  private_node.param<std::string>("config",config_path,"");
#endif
  try{
    const auto c=orbvi_driver::load_config(config_path);
    std::array<Publisher<sensor::Image>,4> images;
    std::array<Publisher<metadata::FrameMetadata>,4> frames;
    Publisher<sensor::Imu> imu;Publisher<metadata::ImuMetadata> imu_metadata;
    Publisher<diagnostic::DiagnosticArray> diagnostics;
    for(unsigned i=0;i<4;++i){auto base=c.topic_prefix+"/camera_"+std::to_string(i);
#ifdef ORBVI_ROS2
      images[i]=node->create_publisher<sensor::Image>(base+"/image",rclcpp::QoS(c.ros_image_queue).reliable());
      frames[i]=node->create_publisher<metadata::FrameMetadata>(base+"/metadata",rclcpp::QoS(256).reliable());
#else
      images[i]=node.advertise<sensor::Image>(base+"/image",c.ros_image_queue);
      frames[i]=node.advertise<metadata::FrameMetadata>(base+"/metadata",256);
#endif
    }
#ifdef ORBVI_ROS2
    imu=node->create_publisher<sensor::Imu>(c.topic_prefix+"/imu",rclcpp::QoS(c.ros_imu_queue).reliable());
    imu_metadata=node->create_publisher<metadata::ImuMetadata>(c.topic_prefix+"/imu/metadata",rclcpp::QoS(c.ros_imu_queue).reliable());
    diagnostics=node->create_publisher<diagnostic::DiagnosticArray>(c.topic_prefix+"/diagnostics",10);
#else
    imu=node.advertise<sensor::Imu>(c.topic_prefix+"/imu",c.ros_imu_queue);
    imu_metadata=node.advertise<metadata::ImuMetadata>(c.topic_prefix+"/imu/metadata",c.ros_imu_queue);
    diagnostics=node.advertise<diagnostic::DiagnosticArray>(c.topic_prefix+"/diagnostics",10);
#endif
    orbvi_driver::Driver driver(c);
    driver.start({[&](orbvi_driver::Image f){
      sensor::Image image;image.width=f.width;image.height=f.height;image.encoding="rgb8";image.is_bigendian=0;image.step=f.width*3;
      stamp(image.header.stamp,f.timestamp_ns);image.header.frame_id="orbvi_camera_"+std::to_string(f.camera);image.data=std::move(f.rgb);
      metadata::FrameMetadata m;m.header=image.header;m.sequence=f.sequence;m.sensor_timestamp_ns=f.timestamp_ns;
      publish<metadata::FrameMetadata>(frames[f.camera],std::move(m));publish<sensor::Image>(images[f.camera],std::move(image));
    },[&](orbvi_driver::Imu s){
      sensor::Imu message;stamp(message.header.stamp,s.timestamp_ns);message.header.frame_id="orbvi_imu";
      message.orientation_covariance[0]=-1;
      message.linear_acceleration.x=s.acceleration[0];message.linear_acceleration.y=s.acceleration[1];message.linear_acceleration.z=s.acceleration[2];
      message.angular_velocity.x=s.angular_velocity[0];message.angular_velocity.y=s.angular_velocity[1];message.angular_velocity.z=s.angular_velocity[2];
      metadata::ImuMetadata m;m.header=message.header;m.sequence=s.sequence;m.accel_timestamp_ns=s.accel_timestamp_ns;m.gyro_timestamp_ns=s.gyro_timestamp_ns;
      publish<metadata::ImuMetadata>(imu_metadata,std::move(m));publish<sensor::Imu>(imu,std::move(message));
    }});
    auto diagnostic_callback=[&]{
      const auto s=driver.stats();diagnostic::DiagnosticArray array;diagnostic::DiagnosticStatus status;
      status.name="orbvi_driver";status.hardware_id="ORBVI_4P_X5";status.level=s.error.empty()?0:2;
      status.message=s.error.empty()?"board monotonic timestamps; RGB decoded from H264":s.error;
      auto add=[&](const std::string& key,uint64_t value){diagnostic::KeyValue v;v.key=key;v.value=std::to_string(value);status.values.push_back(v);};
      uint64_t gaps=s.imu.gaps,invalid=s.imu.invalid,duplicates=0,discontinuities=s.imu.discontinuities;
      for(unsigned i=0;i<4;++i){add("camera_"+std::to_string(i)+"_count",s.cameras[i].count);gaps+=s.cameras[i].gaps;invalid+=s.cameras[i].invalid;duplicates+=s.cameras[i].duplicates_filtered;discontinuities+=s.cameras[i].discontinuities;}
      add("imu_count",s.imu.count);add("sequence_gaps",gaps);add("invalid",invalid);add("image_callback_drops",s.image_callback_drops);
      add("duplicates_filtered",duplicates);add("discontinuities",discontinuities);
      add("transport_drops",s.transport_drops);add("transport_invalid",s.transport_invalid);
      add("imu_last_sequence",s.imu.last_sequence);add("imu_last_timestamp_ns",s.imu.last_timestamp_ns);
      add("imu_gaps",s.imu.gaps);add("imu_discontinuities",s.imu.discontinuities);
      add("imu_callback_drops",s.imu_callback_drops);add("callback_failures",s.callback_failures);add("imu_pairing_errors",s.imu_pairing_errors);
      add("max_camera_skew_ns",s.max_camera_skew_ns);
      if(status.level==0&&(gaps||invalid||duplicates||discontinuities||s.transport_drops||s.transport_invalid||s.image_callback_drops||s.imu_callback_drops||s.callback_failures||s.imu_pairing_errors))status.level=1;
      array.status.push_back(status);publish<diagnostic::DiagnosticArray>(diagnostics,std::move(array));
    };
#ifdef ORBVI_ROS2
    auto timer=node->create_wall_timer(std::chrono::seconds(1),diagnostic_callback);rclcpp::spin(node);driver.stop();rclcpp::shutdown();
#else
    auto timer=node.createTimer(ros::Duration(1),[&](const ros::TimerEvent&){diagnostic_callback();});ros::spin();driver.stop();
#endif
    return 0;
  }catch(const std::exception& e){std::cerr<<"orbvi_node: "<<e.what()<<'\n';return 1;}
}
