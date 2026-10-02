#pragma once
#include "game_protocol.hpp"
#include "jump_motion.hpp"
namespace aion {
// One gravity-start request, not a complete simulated jump/landing trajectory.
// Coordinates and timestamp must belong to the same current world connection.
struct JumpRequest {
    std::array<float,3> position{};
    float rotation{};
    std::array<float,3> velocity{};
    uint64_t clientTime{};
};
Bytes encodeJumpRequest(const JumpRequest& request);
struct JumpState {
    bool ready{},learned{},grounded{},stopPositionUnconfirmed{},simulated{};
    uint64_t ageMs{};
    JumpRequest request;
    std::string status;
    bool movementPenaltyObserved{};
    std::string serverFeedback="尚无已关联的自身服务器反馈";
    std::string serverStateFeedback;
    size_t trajectoryFrames{};uint64_t trajectoryDurationMs{};
};
// Per-connection native traffic only. Generated frames never become new inputs.
class JumpTracker {
public:
    void observe(std::span<const uint8_t> frame,bool outbound,uint64_t nowMs);
    JumpState state(uint64_t nowMs,bool stationaryReuse=false) const;
    JumpRequest prepare(uint64_t nowMs) const;
    std::vector<JumpStep> prepareSequence(uint64_t nowMs,bool stationaryReuse=false)const;
    uint64_t revision()const{return revision_;}
    bool worldEntered()const{return selfKey_.has_value();}
    void completed(const JumpMotion& stop,uint64_t nowMs);
    void submitted(uint64_t nowMs);
    void tick(uint64_t nowMs);
    void ended(uint64_t nowMs,const char* side);
    std::vector<std::pair<std::string,std::string>> takeEvidence(){auto result=std::move(evidence_);evidence_.clear();return result;}
private:
    void inbound(std::span<const uint8_t> frame,unsigned depth,size_t& budget,uint64_t nowMs);
    void record(std::string event,std::string details);
    JumpRequest ground_;
    float jumpSpeed_{};
    uint64_t observedAt_{};
    // Keep the native clock anchor independent of generated landing/send latency.
    uint64_t nativeClockTime_{},nativeClockAt_{};
    bool grounded_{},stopped_{};
    JumpTrajectory trajectory_;
    uint64_t revision_{};
    std::optional<uint64_t> selfKey_;
    uint64_t attempts_{},submittedAt_{},lastNaturalMoveAt_{},lastServerAt_{},lastSummaryAt_{};
    bool windowClosed_=true,penaltyObserved_{};
    std::string feedback_="尚无已关联的自身服务器反馈";
    std::string stateFeedback_;
    std::vector<std::pair<std::string,std::string>> evidence_;
};
}
