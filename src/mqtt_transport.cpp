#include "mqtt_transport.hpp"
#include <windows.h>
#include <winhttp.h>
#include <condition_variable>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
namespace aion {
namespace {
using Clock=std::chrono::steady_clock;
std::wstring wide(std::string_view value){
    if(value.size()>4096)throw std::invalid_argument("MQTT configuration too long");
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(n<=0)throw std::invalid_argument("Invalid UTF-8 MQTT configuration");
    std::wstring out(size_t(n),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),n);return out;
}
[[noreturn]] void networkError(const char* operation,DWORD error){throw std::runtime_error(std::string(operation)+" ("+std::to_string(error)+")");}
struct Internet {
    HINTERNET value{};
    ~Internet(){if(value)WinHttpCloseHandle(value);}
};
struct Event {DWORD kind{},error{},bytes{};WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};};
struct Async {
    HINTERNET handle{};
    std::mutex mutex;std::condition_variable changed;std::deque<Event> events;bool closed=false;
    static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD kind,void* information,DWORD length){
        if(!context)return;auto& self=*reinterpret_cast<Async*>(context);
        Event event{kind};
        if(kind==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && length>=sizeof(WINHTTP_ASYNC_RESULT))
            event.error=static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        if((kind==WINHTTP_CALLBACK_STATUS_READ_COMPLETE || kind==WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE) && length>=sizeof(WINHTTP_WEB_SOCKET_STATUS)){
            auto status=static_cast<WINHTTP_WEB_SOCKET_STATUS*>(information);event.bytes=status->dwBytesTransferred;event.type=status->eBufferType;
        }
        std::lock_guard lock(self.mutex);
        if(kind==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)self.closed=true;
        else if(kind==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || kind==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                kind==WINHTTP_CALLBACK_STATUS_READ_COMPLETE || kind==WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE || kind==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){
            if(self.events.size()<32)self.events.push_back(event);
        }
        self.changed.notify_all();
    }
    void adopt(HINTERNET value){
        handle=value;if(!handle)networkError("MQTT handle creation failed",GetLastError());
        auto context=reinterpret_cast<DWORD_PTR>(this);
        // Install the callback before context: no I/O is started during adoption.
        if(WinHttpSetStatusCallback(handle,callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK){
            auto error=GetLastError();WinHttpCloseHandle(std::exchange(handle,nullptr));networkError("MQTT callback setup failed",error);
        }
        if(!WinHttpSetOption(handle,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))){
            auto error=GetLastError();WinHttpCloseHandle(std::exchange(handle,nullptr));networkError("MQTT context setup failed",error);
        }
    }
    ~Async(){
        if(!handle)return;
        WinHttpCloseHandle(handle);
        // Closing an asynchronous handle can deliver callbacks after returning.
        // Keep both the context and its receive buffer alive until HANDLE_CLOSING.
        std::unique_lock lock(mutex);changed.wait(lock,[&]{return closed;});
    }
    std::optional<Event> wait(DWORD wanted,unsigned timeoutMs,const std::atomic<bool>& stop){
        auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);std::unique_lock lock(mutex);
        for(;;){
            if(stop)throw std::runtime_error("MQTT stopped");
            for(auto it=events.begin();it!=events.end();++it){
                if(it->kind==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)networkError("MQTT I/O failed",it->error);
                if(it->kind==wanted){auto event=*it;events.erase(it);return event;}
            }
            if(Clock::now()>=deadline)return {};
            changed.wait_until(lock,std::min(deadline,Clock::now()+std::chrono::milliseconds(100)));
        }
    }
    void require(DWORD wanted,const std::atomic<bool>& stop){if(!wait(wanted,10000,stop))throw std::runtime_error("MQTT I/O timed out");}
};
}
struct MqttTransport::Impl {
    const std::atomic<bool>& stop;
    Internet session,connection;
    // Buffers outlive websocket callbacks, including cancellation during destruction.
    std::array<uint8_t,16384> receiveBuffer{};
    Bytes sendBuffer;
    Async request,socket;
    mqtt::Decoder decoder;
    bool reading=false,pingWaiting=false;
    Clock::time_point lastSent=Clock::now(),pingAt{};
    uint16_t nextId=1;
    std::map<uint16_t,Clock::time_point> pending;
    std::vector<uint16_t> acked;
    Impl(const MqttConfig& config,const std::atomic<bool>& stopped):stop(stopped){
        if(config.host.empty() || config.host.size()>253 || config.host.find_first_of("/:\\?#@")!=std::string::npos || !config.port ||
           config.path.empty() || config.path.front()!='/' || config.path.size()>512 || config.path.find_first_of("\r\n")!=std::string::npos)
            throw std::invalid_argument("Invalid MQTT endpoint");
        session.value=WinHttpOpen(L"Aion2Pipe-query/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        if(!session.value)networkError("MQTT session failed",GetLastError());
        if(!WinHttpSetTimeouts(session.value,10000,10000,10000,10000))networkError("MQTT timeout setup failed",GetLastError());
        connection.value=WinHttpConnect(session.value,wide(config.host).c_str(),config.port,0);
        if(!connection.value)networkError("MQTT connection failed",GetLastError());
        request.adopt(WinHttpOpenRequest(connection.value,L"GET",wide(config.path).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)) ||
           !WinHttpSetOption(request.handle,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0))networkError("MQTT upgrade setup failed",GetLastError());
        if(!WinHttpSendRequest(request.handle,L"Sec-WebSocket-Protocol: mqtt\r\n",DWORD(-1),nullptr,0,0,reinterpret_cast<DWORD_PTR>(&request)) && GetLastError()!=ERROR_IO_PENDING)
            networkError("MQTT upgrade request failed",GetLastError());
        request.require(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,stop);
        if(!WinHttpReceiveResponse(request.handle,nullptr) && GetLastError()!=ERROR_IO_PENDING)networkError("MQTT upgrade response failed",GetLastError());
        request.require(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,stop);
        DWORD status=0,size=sizeof(status);
        if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr) || status!=101)
            throw std::runtime_error("MQTT WebSocket upgrade rejected");
        wchar_t protocol[32]{};size=sizeof(protocol);
        if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_CUSTOM,L"Sec-WebSocket-Protocol",protocol,&size,nullptr) || std::wstring(protocol)!=L"mqtt")
            throw std::runtime_error("MQTT WebSocket subprotocol missing");
        socket.adopt(WinHttpWebSocketCompleteUpgrade(request.handle,0));
        send(mqtt::connect(config.clientId,config.username,config.password));
        auto connack=packet(10000);
        if(!connack || connack->header!=0x20 || connack->body!=Bytes{0,0})throw std::runtime_error("MQTT authentication rejected or timed out");
        if(!config.topic.empty()){
            send(mqtt::subscribe(nextId++,config.topic));auto suback=packet(10000);
            if(!suback)throw std::runtime_error("MQTT task subscription timed out");
            if(suback->header!=0x90 || suback->body.size()!=3 || suback->body[0]!=0 || suback->body[1]!=1)
                throw std::runtime_error("Invalid MQTT subscription acknowledgement");
            if(suback->body[2]==0x80)throw std::runtime_error("MQTT task subscription rejected");
            if(suback->body[2]!=1)throw std::runtime_error("MQTT task subscription requires QoS 1");
        }
    }
    void send(Bytes bytes){
        sendBuffer=std::move(bytes);
        auto error=WinHttpWebSocketSend(socket.handle,WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,sendBuffer.data(),DWORD(sendBuffer.size()));
        if(error!=NO_ERROR && error!=ERROR_IO_PENDING)networkError("MQTT send failed",error);
        socket.require(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,stop);lastSent=Clock::now();
        SecureZeroMemory(sendBuffer.data(),sendBuffer.size());sendBuffer.clear();
    }
    std::optional<mqtt::Packet> packet(unsigned timeoutMs){
        const auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
        for(;;){
            if(auto p=decoder.next())return p;
            if(!reading){
                auto error=WinHttpWebSocketReceive(socket.handle,receiveBuffer.data(),DWORD(receiveBuffer.size()),nullptr,nullptr);
                if(error!=NO_ERROR && error!=ERROR_IO_PENDING)networkError("MQTT receive failed",error);reading=true;
            }
            auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            auto event=socket.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,unsigned(std::max<int64_t>(0,remaining)),stop);
            if(!event)return {};reading=false;
            if(event->type!=WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && event->type!=WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                throw std::runtime_error("MQTT WebSocket closed or received non-binary data");
            if(event->bytes>receiveBuffer.size())throw std::runtime_error("MQTT receive overflow");
            decoder.feed(std::span(receiveBuffer).first(event->bytes));
        }
    }
};
MqttTransport::MqttTransport(const MqttConfig& config,const std::atomic<bool>& stop):impl_(std::make_unique<Impl>(config,stop)){}
MqttTransport::~MqttTransport()=default;
std::optional<mqtt::Publication> MqttTransport::poll(unsigned timeoutMs){
    auto& p=*impl_;const auto now=Clock::now();
    if(p.pingWaiting && now-p.pingAt>std::chrono::seconds(15))throw std::runtime_error("MQTT keepalive timed out");
    for(const auto& [id,sent]:p.pending)if(now-sent>std::chrono::seconds(15))throw std::runtime_error("MQTT publish acknowledgement timed out");
    if(!p.pingWaiting && now-p.lastSent>=std::chrono::seconds(15)){p.send(Bytes{0xc0,0});p.pingWaiting=true;p.pingAt=now;}
    auto packet=p.packet(std::min(timeoutMs,1000u));if(!packet)return {};
    if((packet->header>>4)==3)return mqtt::publication(*packet);
    if(packet->header==0x40 && packet->body.size()==2){
        auto id=uint16_t((packet->body[0]<<8)|packet->body[1]);
        if(p.pending.erase(id)){if(p.acked.size()>=64)throw std::runtime_error("MQTT ACK consumer stalled");p.acked.push_back(id);}return {};
    }
    if(packet->header==0xd0 && packet->body.empty()){p.pingWaiting=false;return {};}
    throw std::runtime_error("Unexpected MQTT control packet");
}
uint16_t MqttTransport::publish(std::string_view topic,std::string_view payload){
    auto& p=*impl_;if(p.pending.size()>=32)throw std::runtime_error("MQTT publish queue full");
    uint16_t id;
    do{id=p.nextId++;if(!p.nextId)p.nextId=1;}while(!id || p.pending.contains(id));
    p.send(mqtt::publish(id,topic,payload));p.pending[id]=Clock::now();return id;
}
void MqttTransport::acknowledge(uint16_t id){if(id)impl_->send(mqtt::ack(id));}
std::vector<uint16_t> MqttTransport::acknowledgements(){return std::exchange(impl_->acked,{});}
}
