#include "mqtt_transport.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <thread>
using namespace aion;
int main(int argc,char** argv){try{
    if(argc<2)throw std::runtime_error("Usage: mqtt_probe encrypted-config.json [--roundtrip]");
    std::ifstream input(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[1]))));auto json=nlohmann::json::parse(input);
    auto encrypted=json.at("passwordDpapi").get<std::string>();DWORD length=0;
    if(!CryptStringToBinaryA(encrypted.c_str(),DWORD(encrypted.size()),CRYPT_STRING_BASE64,nullptr,&length,nullptr,nullptr))throw std::runtime_error("Invalid encrypted credential");
    Bytes bytes(length);if(!CryptStringToBinaryA(encrypted.c_str(),DWORD(encrypted.size()),CRYPT_STRING_BASE64,bytes.data(),&length,nullptr,nullptr))throw std::runtime_error("Invalid encrypted credential");
    DATA_BLOB blob{length,bytes.data()},plain{};
    if(!CryptUnprotectData(&blob,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))throw std::runtime_error("Credential belongs to another Windows user");
    MqttConfig config;config.host=json.at("host").get<std::string>();config.port=json.value("websocketPort",uint16_t(8084));config.username=json.at("username").get<std::string>();
    config.password.assign(reinterpret_cast<char*>(plain.pbData),plain.cbData);SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
    config.clientId="aion2-wss-probe-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
    const bool roundtrip=argc>2 && std::string(argv[2])=="--roundtrip";
    const bool denySubscribe=argc>2 && std::string(argv[2])=="--deny-query-subscribe";
    const bool deviceDelivery=argc>2 && std::string(argv[2])=="--device-delivery";
    if(roundtrip)config.topic="aion2/query-test/"+config.clientId;
    if(denySubscribe)config.topic="aion2/query-workers/acl-check/probe/task";
    if(deviceDelivery){
        const auto device=json.at("clientId").get<std::string>();
        const auto session="probe-"+std::to_string(GetTickCount64());
        config.clientId="query-"+device+"-"+session;
        config.topic="aion2/query-workers/"+device+"/"+session+"/task";
    }
    std::atomic<bool> stop=false;auto before=GetTickCount64();
    {
        if(denySubscribe){
            try{MqttTransport forbidden(config,stop);}
            catch(const std::exception& e){
                if(std::string(e.what()).find("subscription rejected")==std::string::npos)throw;
                std::cout<<"PASS: unrelated account cannot subscribe to query task topics\n";return 0;
            }
            throw std::runtime_error("Query topic subscription was unexpectedly allowed");
        }
        MqttTransport transport(config,stop);SecureZeroMemory(config.password.data(),config.password.size());config.password.clear();
        std::cout<<"WSS TLS and MQTT authentication accepted\n";
        if(deviceDelivery){
            std::cout<<"Awaiting management API delivery on "<<config.topic<<std::endl;
            bool received=false;const auto deadline=GetTickCount64()+45000;
            while(GetTickCount64()<deadline && !received)if(auto p=transport.poll()){
                if(p->topic!=config.topic || p->payload!="{\"type\":\"transport_probe\"}" || p->retained)throw std::runtime_error("Unexpected management probe payload");
                transport.acknowledge(p->id);received=true;
            }
            if(!received)throw std::runtime_error("Management API delivery timed out");
            std::cout<<"PASS: management API message received through scoped device task subscription\n";
        }
        if(roundtrip){
            const std::string payload="{\"test\":\"native-query-transport\"}";
            auto id=transport.publish(config.topic,payload);bool delivered=false,acknowledged=false;
            auto deadline=GetTickCount64()+10000;
            while(GetTickCount64()<deadline && (!delivered || !acknowledged)){
                if(auto p=transport.poll()){if(p->topic!=config.topic || p->payload!=payload || p->retained)throw std::runtime_error("Unexpected probe message");transport.acknowledge(p->id);delivered=true;}
                for(auto ack:transport.acknowledgements())if(ack==id)acknowledged=true;
            }
            if(!delivered || !acknowledged)throw std::runtime_error("Roundtrip or PUBACK timed out");
            std::cout<<"QoS 1 subscribe/publish/receive/ack passed in isolated test topic\n";
        }
        // Exercise cancellation while an asynchronous receive is outstanding.
        transport.poll(10);stop=true;
    }
    std::cout<<"Cancelled pending receive and closed cleanly ("<<GetTickCount64()-before<<" ms total)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
