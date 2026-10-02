#pragma once
#include "game_protocol.hpp"
#include <mutex>
namespace aion {
enum class QueryPhase { Queued, Sent, Completed, Failed };
struct QueryOutcome {
    QueryPhase phase=QueryPhase::Queued;
    std::string presence="unknown",error;
};
// One immutable request identity and one terminal outcome, independent of the UI's
// most recent response. A late reply cannot revive a timed-out attempt.
class QueryReceipt {
public:
    QueryReceipt(uint32_t server,uint64_t character):serverId(server),characterId(character){}
    const uint32_t serverId;
    const uint64_t characterId;
    QueryOutcome snapshot() const;
    void sent();
    void fail(std::string reason);
    void complete(const GameMessage& response,bool ambiguous);
private:
    mutable std::mutex mutex_;
    QueryOutcome outcome_;
};
}
