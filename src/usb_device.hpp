#pragma once
#include <libusb.h>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace orbvi_driver::usb {
class Device {
  libusb_context* context_=nullptr;libusb_device_handle* handle_=nullptr;
  int interface_=-1;uint8_t endpoint_=0;bool claimed_=false;
 public:
  explicit Device(const std::string& serial="") {
    try {
      if(libusb_init(&context_))throw std::runtime_error("libusb initialization failed");
      libusb_device** list=nullptr;auto count=libusb_get_device_list(context_,&list);
      if(count<0)throw std::runtime_error("USB enumeration failed");
      struct List {libusb_device** value;~List(){libusb_free_device_list(value,1);}} guard{list};
      for(ssize_t i=0;i<count;++i){libusb_device_descriptor d{};
        if(libusb_get_device_descriptor(list[i],&d)||d.idVendor!=0x3652||(d.idProduct!=0x1055&&d.idProduct!=0x1056))continue;
        libusb_device_handle* candidate=nullptr;int error=libusb_open(list[i],&candidate);
        if(error)throw std::runtime_error(std::string("cannot open ORBVI USB: ")+libusb_error_name(error));
        unsigned char value[256]={};int n=libusb_get_string_descriptor_ascii(candidate,d.iSerialNumber,value,255);
        if(!serial.empty()&&(n<0||serial!=reinterpret_cast<char*>(value))){libusb_close(candidate);continue;}
        if(handle_){libusb_close(candidate);throw std::runtime_error("multiple ORBVI boards; select usb_serial");}
        handle_=candidate;
      }
      if(!handle_)throw std::runtime_error("ORBVI USB device not found");
      libusb_config_descriptor* config=nullptr;
      if(libusb_get_active_config_descriptor(libusb_get_device(handle_),&config))throw std::runtime_error("cannot read USB configuration");
      for(unsigned i=0;i<config->bNumInterfaces;++i)for(int j=0;j<config->interface[i].num_altsetting;++j){
        const auto& a=config->interface[i].altsetting[j];
        if(a.bInterfaceClass!=0xff||a.bInterfaceSubClass!=0x58||a.bInterfaceProtocol!=0x35)continue;
        for(unsigned k=0;k<a.bNumEndpoints;++k){const auto& e=a.endpoint[k];
          if((e.bmAttributes&3)==LIBUSB_TRANSFER_TYPE_BULK&&(e.bEndpointAddress&LIBUSB_ENDPOINT_IN)){interface_=a.bInterfaceNumber;endpoint_=e.bEndpointAddress;}}
      }
      libusb_free_config_descriptor(config);
      if(interface_<0)throw std::runtime_error("ORBVI X5 bulk interface ff/58/35 missing");
      int error=libusb_claim_interface(handle_,interface_);
      if(error)throw std::runtime_error(std::string("cannot claim ORBVI bulk interface: ")+libusb_error_name(error));
      claimed_=true;
    }catch(...){close();throw;}
  }
  Device(const Device&)=delete;Device& operator=(const Device&)=delete;
  ~Device(){close();}
  void close(){if(handle_){if(claimed_)libusb_release_interface(handle_,interface_);libusb_close(handle_);handle_=nullptr;}if(context_){libusb_exit(context_);context_=nullptr;}}
  int read(uint8_t* data,int capacity){int actual=0;int e=libusb_bulk_transfer(handle_,endpoint_,data,capacity,&actual,20);
    if(e&&e!=LIBUSB_ERROR_TIMEOUT)throw std::runtime_error(std::string("USB receive failed: ")+libusb_error_name(e));return actual;}
  int interface_number()const{return interface_;}int endpoint()const{return endpoint_;}
};
}
