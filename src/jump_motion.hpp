#pragma once
#include "game_protocol.hpp"
namespace aion {
enum class JumpPhase {Start,Update,Fall,Stop};
// Only the four ordinary world-coordinate layouts verified in stationary jumps.
struct JumpMotion {
    JumpPhase phase{};
    std::array<float,3> position{},velocity{};
    float rotation{};
    uint64_t clientTime{};
};
struct JumpStep {uint64_t delayMs{};Bytes frame;};
std::optional<JumpMotion> decodeJumpMotion(std::span<const uint8_t> frame);
Bytes encodeJumpMotion(const JumpMotion& sample);
// Flat, stationary ballistic model fitted to native samples. Protocol units;
// no terrain/collision simulation and no claim of server acceptance.
inline constexpr float FlatJumpSpeed=1000.f;
inline constexpr float FlatJumpGravity=2254.f;
inline constexpr uint64_t FlatJumpFallMs=444,FlatJumpDurationMs=888;
inline constexpr size_t FlatJumpFrames=11;
std::vector<JumpStep> simulateFlatJump(std::array<float,3> position,float rotation,uint64_t clientTime);
class JumpTrajectory {
public:
    void observe(std::span<const uint8_t> nativeFrame);
    void reset(){candidate_.clear();learned_.clear();}
    bool ready()const{return !learned_.empty();}
    size_t size()const{return learned_.size();}
    uint64_t duration()const{return ready()?learned_.back().clientTime-learned_.front().clientTime:0;}
    float speed()const{return ready()?learned_.front().velocity[2]:0;}
    std::vector<JumpStep> plan(std::array<float,3> position,float rotation,uint64_t clientTime)const;
private:
    std::vector<JumpMotion> candidate_,learned_;
};
}
