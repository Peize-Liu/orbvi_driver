#include "orbvi_driver/usb_protocol.hpp"
#include <iostream>
using namespace orbvi_driver::usb;
void check(bool value){if(!value)throw std::runtime_error("USB framing regression");}
template<class F> void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected);}
int main(){try{
 std::vector<uint8_t> frame(29,0);std::memcpy(frame.data(),"X5US",4);frame[4]=1;frame[5]=camera;frame[8]=5;frame[12]=42;frame[16]=7;frame[24]=2;frame[28]=0x65;
 Parser parser;int count=0;auto emit=[&](Message m){check(m.type==camera&&m.sequence==42&&m.timestamp==7&&m.payload.size()==5&&m.payload[0]==2);++count;};
 for(auto c:frame)parser.feed(&c,1,emit);check(count==1);
 std::vector<uint8_t> both=frame;both.insert(both.end(),frame.begin(),frame.end());parser.feed(both.data(),both.size(),emit);check(count==3);
 auto invalid=frame;invalid[0]='?';rejects([&]{Parser p;p.feed(invalid.data(),invalid.size(),emit);});
 invalid=frame;invalid[8]=invalid[9]=invalid[10]=invalid[11]=255;rejects([&]{Parser p;p.feed(invalid.data(),invalid.size(),emit);});
 invalid=frame;invalid[6]=1;rejects([&]{Parser p;p.feed(invalid.data(),invalid.size(),emit);});
 std::vector<uint8_t> prefix(67,0xaa);prefix.insert(prefix.end(),frame.begin(),frame.end());Parser joined(true);joined.feed(prefix.data(),prefix.size(),emit);check(count==4);
 Parser partial;partial.feed(frame.data(),28,emit);check(count==4);partial.feed(frame.data()+28,1,emit);check(count==5);
 std::cout<<"PASS USB split/coalesced messages, length bounds, reserved fields, initial resync\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
