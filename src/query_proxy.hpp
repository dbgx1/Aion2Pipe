#pragma once
#include "query_handshake.hpp"
#include "query_receipt.hpp"
#include "mail_exchange.hpp"
#include "relay_selection.hpp"
#include <winsock2.h>
#include <windows.h>
#include <windivert.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <deque>
namespace aion {
struct ProxyStats {uint64_t received{},matched{},queueDropped{},unsupported{},unmatched{};size_t processes{},connections{};};
struct QueryRoute {
    Endpoint client,remote,upstream;
    uint32_t firstSequence{};
    uint16_t alternatePort{};
};
// Only exact established routes are rewritten. TCP sequence/ACK/options stay intact.
bool rewriteQueryRoute(Bytes& raw,bool& outbound,const QueryRoute& route,uint16_t listener,uint16_t alternate);
struct QueryConnection {
    size_t id{};uint32_t pid{},serverId{};Endpoint client,remote;uint32_t firstSequence{};bool open{},ready{},waiting{},shifted{};
    uint64_t sent{};std::string status;GameMessage response;
    bool queryAvailable{};uint64_t queryCooldownMs{};
    bool guildPending{};
    uint64_t guildSent{},guildReceived{};
    uint16_t guildOpcode{};
    std::string guildStatus="尚未查询军团";
    GameMessage guildResponse;
    MailStatus mail;
    JumpState jumpState;
    bool jumpRepeat{};uint32_t jumpIntervalSeconds=5;uint64_t jumpNextInMs{};
    bool jumpPending{};uint64_t jumpsSent{};
    std::string jumpStatus="尚未提交跳跃序列";
};
class QueryProxy {
public:
    ~QueryProxy();
    bool start(std::wstring executable,uint16_t serverPort);
    // Draining blocks new connections but keeps existing TCP proxies alive.
    void drain();
    bool stop();
    bool running() const {return running_;}
    bool busy() const {return active_.load()!=0;}
    std::string status() const;
    std::string routingMode() const;
    std::vector<QueryConnection> connections() const;
    bool request(size_t id,uint32_t server,uint64_t dbid);
    std::shared_ptr<QueryReceipt> requestTracked(size_t id,uint32_t server,uint64_t dbid);
    bool requestGuild(size_t id,bool search,uint8_t order,std::string name);
    bool requestMail(size_t id,const MailRequest& request);
    bool requestJump(size_t id);
    bool setJumpRepeat(size_t id,bool enabled,uint32_t intervalSeconds=5);
    std::vector<Packet> takePackets();
    ProxyStats stats() const;
private:
    struct Session;
    void enqueue(Packet packet);
    void publishWire(const Session& session,Packet packet);
    void publish(Session& session,const ObservedFrame& frame,uint8_t flags=0x18);
    mutable std::mutex eventsMutex_;
    std::deque<Packet> events_;
    size_t eventBytes_{};
    uint64_t nextConnection_{};
    ProxyStats stats_;
    uint32_t target(const Packet& packet) const;
    void networkLoop();void acceptLoop();void sessionLoop(std::shared_ptr<Session> session,SOCKET client);
    void setStatus(std::string text);
    HANDLE handle_=INVALID_HANDLE_VALUE;
    SOCKET listener_=INVALID_SOCKET;
    uint16_t serverPort_{},listenPort_{},alternatePort_{},loginAlternatePort_{};
    std::wstring executable_;
    std::atomic<bool> running_{false},acceptNew_{false};
    std::atomic<bool> forwarding_{false};
    std::atomic<size_t> active_{};
    std::thread network_,accept_;
    mutable std::mutex mutex_;
    std::string status_="未启用网络查询";
    std::string routingMode_="尚未启用";
    std::vector<std::shared_ptr<Session>> sessions_;
};
}
