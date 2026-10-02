#pragma once
#include "mqtt_transport.hpp"
#include "query_proxy.hpp"
namespace aion {
struct QueryWorkerConfig {std::string clientId,password;};
struct QueryWorkerStatus {
    bool enabled{},connected{},ready{},busy{};
    uint64_t completed{},failed{},rejected{},duplicates{},reconnects{};
    std::string message="尚未启用查询服务",target,sessionId,serverId,lastRejection,lastError;
};
class QueryWorker {
public:
    QueryWorker(QueryProxy& proxy,std::filesystem::path directory);
    ~QueryWorker();
    QueryWorkerConfig config() const;
    QueryWorkerStatus status() const;
    void start(QueryWorkerConfig config);
    void stop();
private: struct Impl;std::unique_ptr<Impl> impl_;
};
}
