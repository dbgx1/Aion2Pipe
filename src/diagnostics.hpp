#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>
namespace aion {
class Diagnostics {
public:
    bool initialize(const std::filesystem::path& primary,const std::filesystem::path& fallback,size_t limit=4*1024*1024);
    void write(std::string_view event,std::string_view details) noexcept;
    bool exportTo(const std::filesystem::path& destination,std::string& error);
    std::filesystem::path path() const;
    std::string error() const;
private:
    std::filesystem::path part(unsigned index) const;
    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream stream_;
    size_t bytes_{},limit_=4*1024*1024;
    std::string error_;
    std::chrono::steady_clock::time_point start_=std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point retryAt_{};
};
Diagnostics& diagnostics();
std::string utf8Path(const std::filesystem::path& path);
void initializeDiagnostics();
}
