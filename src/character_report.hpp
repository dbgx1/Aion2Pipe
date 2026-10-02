#pragma once
#include "nearby.hpp"
#include <memory>
#include <functional>
#include <stdexcept>
#include <nlohmann/json.hpp>
struct sqlite3;
namespace aion {
using ReportJson = nlohmann::json;
std::string reportHash(std::string_view value);
std::optional<ReportJson> reportObservation(const NearbyObject& object);
struct ReportItem {std::string server,character,hash;int64_t revision{};ReportJson record;};
class ReportCapacityError : public std::runtime_error {
public: ReportCapacityError():std::runtime_error("角色缓存已达 100000 条上限"){}
};
// Single worker owns this connection. No network or UI calls inside the store.
class CharacterReportStore {
public:
    explicit CharacterReportStore(const std::filesystem::path& path);
    ~CharacterReportStore();
    bool observe(const std::string& target,const ReportJson& snapshot,int64_t now);
    std::vector<ReportItem> pending(const std::string& target,int64_t now,bool force=false);
    void acknowledge(const std::string& target,const ReportItem& sent,int64_t now);
    void fail(const std::string& target,const ReportItem& sent,int64_t now,int64_t delay,bool blocked);
    void retry(const std::string& target);
    size_t count(const std::string& target,bool blocked=false);
    std::string clientId();
private: sqlite3* db_{};
};
struct CharacterReportConfig {std::string url,token;};
struct CharacterReportStatus {
    bool enabled{},uploading{},authPaused{},storageBlocked{};size_t pending{},blocked{},success{},deduplicated{},overflow{},cacheFull{};
    int64_t lastSuccess{};std::string message="尚未启用角色上报";
};
struct ReportHttpResult {unsigned status{};std::string body;int64_t retryAfterMs{};};
ReportHttpResult postCharacterReport(const CharacterReportConfig& config,const std::string& body);
class CharacterReporter {
public:
    explicit CharacterReporter(std::filesystem::path directory);
    ~CharacterReporter();
    CharacterReportConfig config() const;
    CharacterReportStatus status() const;
    void configure(CharacterReportConfig config,bool enabled);
    void observe(const NearbyObject& object);
    void flush(bool retryFailed=false);
    static std::filesystem::path defaultDirectory();
private: struct Impl;std::unique_ptr<Impl> impl_;
};
}
