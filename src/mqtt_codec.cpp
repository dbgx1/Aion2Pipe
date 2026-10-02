#include "mqtt_codec.hpp"
#include <stdexcept>
namespace aion::mqtt {
namespace {
void number(Bytes& out,uint16_t value){out.push_back(uint8_t(value>>8));out.push_back(uint8_t(value));}
void string(Bytes& out,std::string_view value){
    if(value.size()>65535)throw std::invalid_argument("MQTT string too large");
    number(out,uint16_t(value.size()));out.insert(out.end(),value.begin(),value.end());
}
void topicName(std::string_view topic){
    if(topic.empty() || topic.size()>512 || topic.find_first_of("+#\0",0,3)!=std::string_view::npos)
        throw std::invalid_argument("MQTT requires an exact nonempty topic");
    // Query topics are ASCII identifiers, never arbitrary human-readable text.
    for(unsigned char c:topic)if(c<33 || c>126)throw std::invalid_argument("MQTT topic must be ASCII");
}
Bytes frame(uint8_t header,Bytes body){
    if(body.size()>MaxPacket-5)throw std::invalid_argument("MQTT packet too large");
    Bytes out{header};auto remaining=body.size();
    do{auto b=uint8_t(remaining%128);remaining/=128;out.push_back(b|(remaining?128:0));}while(remaining);
    out.insert(out.end(),body.begin(),body.end());return out;
}
void packetId(uint16_t id){if(!id)throw std::invalid_argument("MQTT packet ID cannot be zero");}
}
Bytes connect(std::string_view clientId,std::string_view username,std::string_view password){
    if(clientId.empty() || clientId.size()>200 || username.empty() || username.size()>200 || password.size()>4096)
        throw std::invalid_argument("Invalid MQTT credentials or client ID");
    Bytes body;string(body,"MQTT");body.insert(body.end(),{4,0xc2,0,30});
    string(body,clientId);string(body,username);string(body,password);return frame(0x10,std::move(body));
}
Bytes subscribe(uint16_t id,std::string_view topic){
    packetId(id);topicName(topic);Bytes body;number(body,id);string(body,topic);body.push_back(1);return frame(0x82,std::move(body));
}
Bytes publish(uint16_t id,std::string_view topic,std::string_view payload,bool duplicate){
    packetId(id);topicName(topic);if(payload.size()>32768)throw std::invalid_argument("MQTT payload too large");
    Bytes body;string(body,topic);number(body,id);body.insert(body.end(),payload.begin(),payload.end());
    return frame(duplicate?0x3a:0x32,std::move(body)); // QoS 1, never retained.
}
Bytes ack(uint16_t id){packetId(id);return Bytes{0x40,2,uint8_t(id>>8),uint8_t(id)};}
Publication publication(const Packet& packet){
    if((packet.header>>4)!=3 || packet.body.size()<2)throw std::runtime_error("Invalid MQTT PUBLISH");
    const auto qos=(packet.header>>1)&3;
    if(qos>1 || (!qos && (packet.header&8)))throw std::runtime_error("Unsupported MQTT QoS");
    const size_t n=(size_t(packet.body[0])<<8)|packet.body[1];
    if(n>packet.body.size()-2)throw std::runtime_error("Truncated MQTT topic");
    Publication p;p.topic.assign(packet.body.begin()+2,packet.body.begin()+2+n);topicName(p.topic);
    size_t offset=2+n;
    if(qos){
        if(packet.body.size()-offset<2)throw std::runtime_error("Truncated MQTT packet ID");
        p.id=uint16_t((packet.body[offset]<<8)|packet.body[offset+1]);packetId(p.id);offset+=2;
    }
    if(packet.body.size()-offset>32768)throw std::runtime_error("MQTT payload too large");
    p.payload.assign(packet.body.begin()+offset,packet.body.end());p.retained=packet.header&1;p.duplicate=packet.header&8;return p;
}
void Decoder::feed(std::span<const uint8_t> bytes){
    if(bytes.size()>MaxPacket || buffer_.size()>MaxPacket-bytes.size())throw std::runtime_error("MQTT receive limit exceeded");
    buffer_.insert(buffer_.end(),bytes.begin(),bytes.end());
}
std::optional<Packet> Decoder::next(){
    if(buffer_.size()<2)return {};
    size_t length=0,multiplier=1,at=1;
    for(;;){
        if(at==buffer_.size())return {};
        const auto b=buffer_[at++];length+=(b&127)*multiplier;
        if(length>MaxPacket-5)throw std::runtime_error("MQTT remaining length exceeds limit");
        if(!(b&128)){if(at>2 && !(b&127))throw std::runtime_error("Noncanonical MQTT length");break;}
        if(at==5)throw std::runtime_error("Malformed MQTT remaining length");
        multiplier*=128;
    }
    if(buffer_.size()-at<length)return {};
    const auto type=buffer_[0]>>4,flags=buffer_[0]&15;
    if(!type || type==15 || (type!=3 && flags!=((type==6 || type==8 || type==10)?2:0)))
        throw std::runtime_error("Invalid MQTT fixed header");
    Packet out{buffer_[0],Bytes(buffer_.begin()+at,buffer_.begin()+at+length)};
    buffer_.erase(buffer_.begin(),buffer_.begin()+at+length);return out;
}
}
