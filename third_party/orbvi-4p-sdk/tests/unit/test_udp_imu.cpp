#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include "orbvi_sdk/client.hpp"
#include "orbvi_sdk/udp_imu.hpp"
#include "test_support.hpp"

namespace {
std::array<std::uint8_t, 72U> golden() {
  return {0x4f,0x49,0x4d,0x55,0x01,0x01,0x00,0x48,
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
    0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,
    0,0,0,0,0x3f,0x80,0,0,0xc0,0x20,0,0,
    0x40,0x50,0,0,0xc0,0x80,0,0,0x40,0xb0,0,0,
    0xc0,0xd8,0,0,0xf3,0x60,0x80,0x31};
}
void update_crc(std::array<std::uint8_t,72U>& packet) {
  std::uint32_t crc=0xffffffffU;
  for (std::size_t i=0;i<68U;++i) {
    crc ^= packet[i];
    for (unsigned bit=0;bit<8U;++bit)
      crc=(crc>>1U)^((crc&1U)?0xedb88320U:0U);
  }
  crc=~crc;
  for (unsigned i=0;i<4U;++i)
    packet[68U+i]=static_cast<std::uint8_t>(crc>>(24U-8U*i));
}
void send_packet(std::uint16_t port, const std::array<std::uint8_t,72U>& data) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK_TRUE(fd >= 0);
  sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port);
  CHECK_EQ(inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr),1);
  CHECK_EQ(sendto(fd,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr)),72);
  close(fd);
}
}
int main() {
  using namespace orbvi_sdk;
  auto bytes=golden();
  auto parsed=parse_oimu_packet(bytes.data(),bytes.size(),1234);
  CHECK_TRUE(parsed.ok());
  CHECK_EQ(parsed.value().sequence,0x0102030405060708ULL);
  CHECK_EQ(parsed.value().timestamp_ns,1234);
  CHECK_EQ(parsed.value().sensor_timestamp_ns,0x1112131415161718LL);
  CHECK_EQ(parsed.value().board_send_timestamp_ns,0x3132333435363738ULL);
  CHECK_EQ(parsed.value().accel_x_mps2,1.0);
  CHECK_EQ(parsed.value().gyro_z_radps,-6.75);
  CHECK_TRUE(!parse_oimu_packet(nullptr,72,1).ok());
  CHECK_TRUE(!parse_oimu_packet(bytes.data(),71,1).ok());
  bytes[68]^=1; CHECK_TRUE(!parse_oimu_packet(bytes.data(),72,1).ok()); bytes=golden();
  bytes[0]^=1; CHECK_TRUE(!parse_oimu_packet(bytes.data(),72,1).ok()); bytes=golden();
  bytes[40]=1; update_crc(bytes); CHECK_TRUE(!parse_oimu_packet(bytes.data(),72,1).ok()); bytes=golden();
  bytes[44]=0x7f; bytes[45]=0xc0; update_crc(bytes);
  CHECK_TRUE(!parse_oimu_packet(bytes.data(),72,1).ok()); bytes=golden();
  bytes[16]=0; bytes[17]=0; bytes[18]=0; bytes[19]=0;
  bytes[20]=0; bytes[21]=0; bytes[22]=0; bytes[23]=0;
  update_crc(bytes); CHECK_TRUE(!parse_oimu_packet(bytes.data(),72,1).ok()); bytes=golden();
  UdpImuConfig bad; bad.port=0; CHECK_EQ(make_udp_imu_client(bad).open().code(),StatusCode::invalid_argument);
  UdpImuConfig config; config.bind_address="127.0.0.1"; config.source_address="127.0.0.1";
  config.port=0;
  // Acquire an ephemeral free UDP port then release it for the SDK bind.
  const int probe=socket(AF_INET,SOCK_DGRAM,0); CHECK_TRUE(probe>=0);
  sockaddr_in local{}; local.sin_family=AF_INET; local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  CHECK_EQ(bind(probe,reinterpret_cast<sockaddr*>(&local),sizeof(local)),0);
  socklen_t length=sizeof(local);
  CHECK_EQ(getsockname(probe,reinterpret_cast<sockaddr*>(&local),&length),0);
  config.port=ntohs(local.sin_port); close(probe);
  Client client=make_udp_imu_client(config);
  std::mutex mutex; std::condition_variable ready; std::uint64_t received=0;
  CallbackSet callbacks; callbacks.on_imu=[&](const ImuEvent& event) {
    std::lock_guard<std::mutex> lock(mutex); received=event.sample.sequence; ready.notify_all();
  };
  auto subscription=client.subscribe({},callbacks); CHECK_TRUE(subscription.ok());
  CHECK_TRUE(client.open().ok()); CHECK_TRUE(client.start().ok());
  bytes=golden(); bytes[68]^=1; send_packet(config.port,bytes);
  bytes=golden(); send_packet(config.port,bytes);
  { std::unique_lock<std::mutex> lock(mutex); CHECK_TRUE(ready.wait_for(lock,std::chrono::seconds(2),[&]{return received!=0;})); }
  CHECK_EQ(client.diagnostics().crc_errors,1U);
  CHECK_EQ(client.diagnostics().imu_samples_received,1U);
  bytes=golden(); bytes[15]+=2; update_crc(bytes); send_packet(config.port,bytes);
  for (unsigned tries=0;tries<100U && client.diagnostics().imu_samples_received<2U;++tries)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK_EQ(client.diagnostics().imu_samples_received,2U);
  CHECK_EQ(client.diagnostics().imu_sequence_gaps,1U);
  CHECK_TRUE(client.stop().ok()); CHECK_TRUE(client.close().ok());
  CHECK_TRUE(client.open().ok()); CHECK_TRUE(client.start().ok());
  CHECK_TRUE(client.stop().ok()); CHECK_TRUE(client.close().ok());
}
