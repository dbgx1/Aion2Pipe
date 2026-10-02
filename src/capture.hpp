#pragma once
#include "protocol.hpp"
#include <winsock2.h>
#include <windows.h>
#include <windivert.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace aion {
struct CaptureStats { uint64_t received{}, matched{}, queueDropped{}, unsupported{}, unmatched{}; size_t processes{}, connections{}; };
class Capture {
public:
    ~Capture();
    bool start(const std::wstring& executable);
    void stop();
    bool running() const {return running_;}
    std::string status() const;
    CaptureStats stats() const;
    std::vector<Packet> drain();
private:
    struct Owner {Endpoint local, remote; uint8_t protocol{}; uint32_t pid{}; bool udpWildcard{};};
    struct Flow {Owner owner; int64_t begin{}, end{}; uint64_t endpoint{};};
    struct Pending {Packet packet; int64_t qpc{}; uint64_t queuedMs{};};
    bool isTarget(uint32_t pid) const;
    void receiveLoop();
    void flowLoop();
    void resolveLoop();
    std::vector<Owner> snapshotOwners(size_t& targets);
    void setStatus(std::string s);
    HANDLE network_=INVALID_HANDLE_VALUE, flow_=INVALID_HANDLE_VALUE;
    std::atomic<bool> running_{false};
    std::atomic<bool> receiverFinished_{true}, flowFinished_{true};
    std::thread receiver_, flows_, resolver_;
    std::wstring executable_;
    std::string lastTargetConnections_;
    mutable std::mutex mutex_;
    std::string status_="Ready / 就绪";
    CaptureStats stats_;
    std::deque<Pending> pending_;
    std::deque<Packet> ready_;
    std::vector<Flow> owners_;
    int64_t baseQpc_{}, frequency_{};
    uint64_t baseUs_{};
    size_t pendingBytes_{}, readyBytes_{};
};
}
