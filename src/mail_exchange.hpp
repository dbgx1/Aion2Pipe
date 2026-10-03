#pragma once
#include "game_protocol.hpp"
#include <utility>
namespace aion {
struct MailStatus {
    bool pending{},blocked{},nativePending{};
    uint64_t sent{};
    std::string status="尚未发送测试邮件";
    GameMessage response;
};
// Single manual operation. There is no request ID in E202, so an overlap or
// timeout makes later acknowledgments ambiguous for the life of the connection.
class MailExchange {
public:
    MailStatus view;
    std::optional<MailRequest> command;
    bool queue(const MailRequest& request,uint64_t now){
        encodeMailRequest(request);
        if(view.pending || view.blocked || view.nativePending)return false;
        command=request;queuedAt_=now;sentAt_=0;view.pending=true;view.response={};
        view.status="邮件已排队，尚未发送";return true;
    }
    void submitted(uint64_t now){
        command.reset();sentAt_=now;++view.sent;view.status="邮件已提交，等待服务器确认";
    }
    void cancelQueued(){
        if(!command)return;command.reset();view.pending=false;view.status="邮件未发送：连接忙碌或不可用";
    }
    void nativeRequest(uint64_t now){
        if(view.nativePending){view.blocked=true;view.status="多个原生邮件请求重叠；本连接暂停邮件测试";}
        view.nativePending=true;nativeAt_=now;
        if(command){cancelQueued();view.status="邮件未发送：游戏正在发送邮件";}
        else if(view.pending){view.pending=false;view.blocked=true;view.status="与游戏原生邮件重叠，结果归属未知；本连接暂停测试发送";}
    }
    void receive(GameMessage message){
        view.response=std::move(message);
        if(view.blocked){view.status="收到邮件响应，但先前已超时或重叠；不能确认是本次测试结果";return;}
        if(!view.response.structureComplete || !view.response.mailResult){
            if(view.pending || view.nativePending){command.reset();view.pending=false;view.blocked=true;}
            view.status="邮件响应不完整，结果未确认；请查看原始包";return;
        }
        if(view.pending && !command && sentAt_){
            view.pending=false;view.status=gameMailResultText(*view.response.mailResult);
        }else {cancelQueued();view.status="观察到游戏邮件响应："+gameMailResultText(*view.response.mailResult);}
        view.nativePending=false;
    }
    void tick(uint64_t now){
        if(command && now-queuedAt_>=5000)cancelQueued();
        if((view.pending && !command && sentAt_ && now-sentAt_>=15000) ||
           (view.nativePending && now-nativeAt_>=15000)){
            view.pending=view.nativePending=false;view.blocked=true;
            view.status="15 秒未收到确认，结果未知，不自动重发；本连接暂停邮件测试";
        }
    }
    void close(){
        if(command){cancelQueued();view.status="连接结束，排队邮件未发送";}
        else if(view.pending)view.status="连接结束，邮件可能已发送，结果未知，不自动重试";
        view.pending=view.nativePending=false;view.blocked=true;
    }
private:
    uint64_t queuedAt_{},sentAt_{},nativeAt_{};
};
}
