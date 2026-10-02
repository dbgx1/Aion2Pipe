#pragma once
#include "query_stream.hpp"
#include <memory>
namespace aion {
// Windows CNG owns private key storage. No game process access or key files.
class RsaOaepKey {
public:
    RsaOaepKey();~RsaOaepKey();
    RsaOaepKey(const RsaOaepKey&)=delete;
    RsaOaepKey& operator=(const RsaOaepKey&)=delete;
    void generate();void importPublic(std::span<const uint8_t> der);
    Bytes publicDer() const;
    Bytes encrypt(std::span<const uint8_t> plain) const;
    Bytes decrypt(std::span<const uint8_t> cipher) const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
Bytes frameGameBody(std::span<const uint8_t> body);
class WorldMitm {
public:
    WorldMitm(Endpoint client,Endpoint server,uint32_t sequence):cipher_(client,server,sequence){}
    Bytes fromClient(std::span<const uint8_t> bytes);
    Bytes fromServer(std::span<const uint8_t> bytes);
    Bytes query(uint32_t server,uint64_t dbid);
    Bytes guild(bool search,uint8_t order,std::string_view name);
    Bytes jump(const JumpRequest& request);
    Bytes jumpMotion(std::span<const uint8_t> frame);
    // Replaced by each fromClient/fromServer/query call; no unbounded backlog.
    std::vector<ObservedFrame> observations;
    bool ready()const{return established_ && cipher_.verified();}
    bool established()const{return established_;}
    bool changed()const{return changed_;}
    bool clientBoundary()const{return clientPending_.empty() && cipher_.boundary();}
    bool serverBoundary()const{return serverPending_.empty();}
    bool takeNativeQuery(){return cipher_.takeNativeQuery();}
    const std::string& status()const{return status_;}
private:
    Bytes clientFrame(std::span<const uint8_t> frame,size_t prefix);
    Bytes serverFrame(std::span<const uint8_t> frame,size_t prefix);
    QueryStream cipher_;
    RsaOaepKey clientKey_,proxyKey_;
    Bytes clientPending_,serverPending_;
    bool changed_{},established_{},passthrough_{};
    std::string status_="等待游戏握手";
};
}
