#pragma once
#include "protocol.hpp"
#include "game_protocol.hpp"
#include "nearby.hpp"
#include "query_proxy.hpp"
#include "character_report.hpp"
#include "query_worker.hpp"
#include "chat_bridge.hpp"
#include <unordered_map>
namespace aion {
class App {
public:
    App();
    ~App();
    void autoStart();
    void draw();
    void pump();
    void demo();
    void open(const std::filesystem::path& path);
    void startCapture();
    void openCipherState(const std::filesystem::path& path);
    void smokeFrames() {source_=2; lengthWidth_=1; headerSize_=1; opcodeWidth_=0; lengthAdjustment_=-3; selectFrameTab_=true;}
    void smokeProtocol(uint16_t port=13328) {source_=2; selectGameTab_=true;for(size_t i=0;i<packets_.size();++i)if(!packets_[i].wireObservation && !packets_[i].payload.empty() && (packets_[i].source.port==port || (packets_[i].proxyConnection && !packets_[i].outbound && packets_[i].source.port!=13700))){selected_=int(i);break;}}
    void smokeOutgoing() {source_=0;selectGameTab_=true;for(size_t i=0;i<packets_.size();++i){const auto& p=packets_[i];if(p.proxyConnection && !p.wireObservation && p.outbound && p.plaintext && !p.toolGenerated){auto split=splitGameFrames(p.payload);if(!split.frames.empty()){auto op=readInteger(p.payload,split.frames[0].prefixBytes,2,false);if(op>=0x3700 && op<=0x3703){selected_=int(i);break;}}}}}
    void showNearby() {selectNearbyTab_=true;}
    void showGuild() {selectNearbyTab_=selectQueryTab_=selectDebugTab_=selectReportTab_=selectGameTab_=selectFrameTab_=false;selectGuildTab_=true;}
    void showQueries() {selectQueryTab_=true;}
    void showDebug() {selectDebugTab_=true;}
    void showReport() {selectReportTab_=true;}
    bool canClose();
private:
    friend struct AppRetentionTest;
    void observeProxy(const Packet& packet,uint64_t record);
    void add(std::vector<Packet> packets);
    void clear();
    void toolbar();
    void packetsView();
    void inspector();
    void hexView(std::span<const uint8_t> bytes);
    void fieldsView(std::span<const uint8_t> bytes);
    void framesView(std::span<const uint8_t> bytes);
    void gameView(std::span<const uint8_t> bytes,const Packet& packet);
    void nearbyView();
    void queryView();
    void guildView();
    bool selectGuildTab_{};
    size_t guildConnection_{};
    int guildOrder_{};
    char guildName_[257]{};
    std::string guildNotice_;
    void debugView();
    void reportView();
    void queryWorkerView();
    void chatBridgeView();
    ChatBridge chatBridge_;
    bool chatAutoStart_=true,chatWorldSeen_{};
    std::unique_ptr<CharacterReporter> reporter_;
    char reportUrl_[512]{},reportToken_[4097]{};
    uint64_t reportTick_{};
    bool selectReportTab_=false;
    bool selectDebugTab_=false;
    size_t debugConnection_{};
    QueryProxy queryProxy_;
    std::unique_ptr<QueryWorker> queryWorker_;
    char queryWorkerId_[65]{};
    bool selectQueryTab_=false;
    size_t queryConnection_{};
    int queryPort_=13328,upstreamProxyPort_=0;
    char queryServer_[16]{},queryDbid_[32]{};
    void diagnosticSnapshot(bool force=false);
    void refreshSelfPositions();
    std::map<std::string,std::string> reverseStreams_;
    std::map<std::string,std::pair<Endpoint,Endpoint>> streamEndpoints_;
    std::map<std::string,std::vector<StreamObservation>> streamObservations_;
    std::map<std::string,SelfMovementStream> selfMovement_;
    std::map<std::string,size_t> diagnosticStreamIds_;
    std::map<std::string,std::string> diagnosticLastStreams_;
    std::map<std::string,size_t> worldProbeSizes_;
    size_t diagnosticNextStreamId_{};
    uint64_t diagnosticTick_{};
    std::map<std::string,NearbyObjects> nearby_;
    std::string nearbyConnection_;
    bool selectNearbyTab_=false,showDeparted_=false,followNearby_=true;
    std::optional<uint32_t> nearbySelected_;
    int nearbyKind_=0;
    char nearbyFilter_[160]{};
    std::vector<Packet> packets_;
    std::unordered_map<std::string,Stream> streams_;
    std::unordered_map<std::string,size_t> generations_;
    std::vector<std::string> packetStreams_;
    size_t memory_=0, streamMemory_=0;
    uint64_t observedRecords_{};
    int recordKind_=0;
    int selected_=-1, byteOffset_=0, source_=0, direction_=0, transport_=0;
    int startOffset_=0, lengthOffset_=0, lengthWidth_=2, headerSize_=4, maxFrame_=1048576, opcodeOffset_=2, opcodeWidth_=2;
    int lengthAdjustment_=0;
    bool scrollToByte_=false;
    bool selectFrameTab_=false;
    bool selectGameTab_=false;
    int gameMode_=0, gameFrame_=0;
    Bytes expandedGame_;
    Bytes decryptedGame_;
    std::optional<CipherSnapshot> cipherSnapshot_;
    bool pendingDecrypt_=false;
    uint64_t expandedPacket_=0;
    float packetPanelRatio_=0.30f;
    bool bigEndian_=false, includesHeader_=true, payloadOnly_=true, autoScroll_=true, limit_=false;
    char process_[260]="AION2.exe", filter_[128]{}, search_[256]{};
    std::string message_="启用独立代理，或加载示例数据。";
    std::string mode_="EMPTY";
    bool proxyData_=false;
    size_t proxySelfUpdates_{};
    std::map<uint64_t,QueryConnection> recordedConnections_;
    std::map<std::string,uint64_t> proxyStreamIds_;
};
void applyTheme();
}
