#include "mail_exchange.hpp"
#include <iostream>
#include <stdexcept>
using namespace aion;
static size_t checks=0;
static void check(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(what);}
template<class F>void reject(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed,"invalid mail accepted");}
static GameMessage response(uint16_t result){
    // Native reader: u16 opcode, u16 result, u64 reset time, two u8 counts.
    Bytes b{18,2,0xe2,uint8_t(result),uint8_t(result>>8),1,2,3,4,5,6,7,8,3,4};
    return decodeGameFrame(b,true);
}
int main(){try{
    MailRequest request{"A","B","C"};
    check(encodeMailRequest(request)==Bytes({13,1,0xe2,1,1,'A',1,'B',1,'C'}),"native field order fixture");
    auto guild=request;guild.type=2;
    check(encodeMailRequest(guild)==Bytes({13,1,0xe2,2,1,'A',1,'B',1,'C'}),"guild type written into packet");
    guild.receiver.clear();check(encodeMailRequest(guild)==Bytes({12,1,0xe2,2,0,1,'B',1,'C'}),"guild empty receiver encodes zero length");
    for(uint8_t type:{uint8_t(0),uint8_t(3),uint8_t(255)}){auto bad=request;bad.type=type;reject([&]{encodeMailRequest(bad);});}
    MailRequest unicode{"玩家","标题","正文"};auto wire=encodeMailRequest(unicode);
    check(wire[0]==28 && wire[4]==6 && wire[11]==6 && wire[18]==6,"UTF-8 byte lengths, not character lengths");
    auto decoded=decodeGameFrame(wire,true,true);check(decoded.structureComplete,"unicode request decoding");
    bool title=false;for(const auto& f:decoded.fields)title|=f.name=="标题" && f.value=="标题";check(title,"title text preserved");
    auto max=request;max.title=std::string(50,'x');max.body=std::string(500,'y');wire=encodeMailRequest(max);
    auto frames=splitGameFrames(wire);check(frames.frames.size()==1 && frames.frames[0].prefixBytes==2 && decodeGameFrame(wire,true,true).structureComplete,"multi-byte framing and body length");
    max.title+='x';reject([&]{encodeMailRequest(max);});max.title="x";max.body+='y';reject([&]{encodeMailRequest(max);});
    for(auto invalid:{std::string(),std::string("  "),std::string("a\0b",3),std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),std::string("\xf4\x90\x80\x80"),std::string("\xe4\xb8")}){
        auto bad=request;bad.title=invalid;reject([&]{encodeMailRequest(bad);});
    }
    auto ok=response(0);check(ok.structureComplete && ok.mailResult==0,"success response");
    auto fail=response(0x2183);check(fail.structureComplete && fail.mailResult==0x2183 && gameMailResultText(0x2183).find("ReceiverNotFound")!=std::string::npos,"raw failure code");
    check(gameMailResultText(65535).find("未知错误")!=std::string::npos,"unknown code not guessed");
    for(size_t n=0;n<12;++n){Bytes b{uint8_t(n+6),2,0xe2};b.resize(n+3);auto m=decodeGameFrame(b,true);check(!m.structureComplete,"truncated response accepted");}
    MailExchange m;check(m.queue(request,1),"queue");check(!m.queue(request,2),"double click rejected");m.submitted(3);
    check(m.view.pending && m.view.sent==1 && !m.command,"submission is not acceptance");m.receive(ok);
    check(!m.view.pending && !m.view.blocked && m.view.status.find("发送成功")!=std::string::npos,"confirmed success");
    check(m.queue(request,10),"new manual send");m.submitted(11);m.receive(fail);check(m.view.status.find("ReceiverNotFound")!=std::string::npos,"confirmed failure surfaced");
    check(m.queue(request,20),"queue timeout");m.tick(5020);check(!m.view.pending && !m.view.blocked && !m.command,"unsent expires safely");
    check(m.queue(request,6000),"queue again");m.submitted(6001);m.tick(21000);check(m.view.pending,"no early timeout");m.tick(21001);
    check(m.view.blocked && !m.view.pending && !m.queue(request,21002),"uncertain send cannot be retried");m.receive(ok);check(m.view.status.find("不能确认")!=std::string::npos,"late response not attributed");
    MailExchange native;native.nativeRequest(1);check(!native.queue(request,2),"native mail in progress");native.receive(ok);check(native.queue(request,3),"native response clears gate");native.nativeRequest(4);check(!native.command && !native.view.pending,"native activity cancels queued send");
    native.receive(ok);check(native.queue(request,5),"queue after native");native.submitted(6);native.nativeRequest(7);native.receive(ok);check(native.view.blocked && native.view.status.find("不能确认")!=std::string::npos,"overlap cannot confirm tool success");
    MailExchange truncated;truncated.queue(request,1);truncated.submitted(2);GameMessage broken;broken.mailResult=uint16_t(0);truncated.receive(broken);check(truncated.view.blocked && !truncated.view.pending,"truncated success never accepted");
    MailExchange multiple;multiple.nativeRequest(1);multiple.nativeRequest(2);multiple.receive(ok);check(!multiple.queue(request,3),"multiple native requests cannot clear gate on first response");
    MailExchange closed;closed.queue(request,1);closed.close();check(!closed.command && !closed.view.pending,"close discards queue");
    MailExchange lost;lost.queue(request,1);lost.submitted(2);lost.close();check(lost.view.status.find("结果未知")!=std::string::npos,"disconnect not success");
    std::cout<<checks<<" mail checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
