#include "query_handshake.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace aion {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void nt(NTSTATUS status){if(status<0)throw std::runtime_error("Windows RSA 处理失败："+std::to_string(uint32_t(status)));}
void append(Bytes& out,std::span<const uint8_t> b){out.insert(out.end(),b.begin(),b.end());}
void leb(Bytes& out,size_t n){do{uint8_t c=uint8_t(n&127);n>>=7;out.push_back(c|(n?128:0));}while(n);}
size_t readLeb(std::span<const uint8_t> b,size_t& at){
    size_t n=0;for(unsigned i=0;i<5;++i){require(at<b.size(),"握手长度被截断");auto c=b[at++];require(i!=4 || !(c&0xf0),"握手长度溢出");n|=size_t(c&127)<<(7*i);if(!(c&128))return n;}throw std::runtime_error("握手长度无效");
}
Bytes derTag(uint8_t tag,std::span<const uint8_t> content){Bytes out{tag};size_t n=content.size();if(n<128)out.push_back(uint8_t(n));else{Bytes length;for(;n;n>>=8)length.insert(length.begin(),uint8_t(n));out.push_back(uint8_t(128|length.size()));append(out,length);}append(out,content);return out;}
std::span<const uint8_t> derRead(std::span<const uint8_t>& b,uint8_t tag){
    require(b.size()>=2 && b[0]==tag,"RSA 公钥格式不是 PKCS#1 DER");size_t pos=2,n=b[1];
    if(n&128){size_t count=n&127;require(count && count<=2 && b.size()>=pos+count && b[pos]!=0,"DER 长度无效");n=0;while(count--)n=(n<<8)|b[pos++];require(n>=128,"DER 非规范长度");}
    require(n<=b.size()-pos,"DER 内容被截断");auto value=b.subspan(pos,n);b=b.subspan(pos+n);return value;
}
std::span<const uint8_t> positive(std::span<const uint8_t> b){
    require(!b.empty() && !(b.front()&128),"RSA 整数不是正数");
    if(b.front()==0){require(b.size()>1 && b[1]&128,"RSA 整数编码无效");b=b.subspan(1);}return b;
}
Bytes integerDer(std::span<const uint8_t> b){Bytes value;if(b.front()&128)value.push_back(0);append(value,b);return derTag(2,value);}
bool invalid(const GameFrames& split){return split.status.starts_with("无效") || split.status.starts_with("帧超过");}
}
struct RsaOaepKey::Impl {BCRYPT_ALG_HANDLE algorithm{};BCRYPT_KEY_HANDLE key{};~Impl(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}};
RsaOaepKey::RsaOaepKey():impl_(std::make_unique<Impl>()){}
RsaOaepKey::~RsaOaepKey()=default;
void RsaOaepKey::generate(){
    require(!impl_->key,"RSA 密钥已初始化");if(!impl_->algorithm)nt(BCryptOpenAlgorithmProvider(&impl_->algorithm,BCRYPT_RSA_ALGORITHM,nullptr,0));
    nt(BCryptGenerateKeyPair(impl_->algorithm,&impl_->key,2048,0));nt(BCryptFinalizeKeyPair(impl_->key,0));
}
void RsaOaepKey::importPublic(std::span<const uint8_t> der){
    require(!impl_->key,"RSA 密钥已初始化");auto seq=derRead(der,0x30);require(der.empty(),"RSA 公钥存在多余数据");
    auto modulus=positive(derRead(seq,2)),exponent=positive(derRead(seq,2));
    require(seq.empty() && modulus.size()==256 && (modulus.front()&128) && exponent.size()<=8 && (modulus.back()&1) && (exponent.back()&1),"仅支持 RSA-2048 公钥");
    require(exponent.size()>1 || exponent[0]>=3,"RSA 公钥指数无效");
    BCRYPT_RSAKEY_BLOB header{BCRYPT_RSAPUBLIC_MAGIC,2048,ULONG(exponent.size()),ULONG(modulus.size()),0,0};Bytes blob(sizeof(header));memcpy(blob.data(),&header,sizeof(header));append(blob,exponent);append(blob,modulus);
    if(!impl_->algorithm)nt(BCryptOpenAlgorithmProvider(&impl_->algorithm,BCRYPT_RSA_ALGORITHM,nullptr,0));
    nt(BCryptImportKeyPair(impl_->algorithm,nullptr,BCRYPT_RSAPUBLIC_BLOB,&impl_->key,blob.data(),ULONG(blob.size()),0));
}
Bytes RsaOaepKey::publicDer()const{
    require(impl_->key!=nullptr,"RSA 密钥未初始化");ULONG size=0;nt(BCryptExportKey(impl_->key,nullptr,BCRYPT_RSAPUBLIC_BLOB,nullptr,0,&size,0));Bytes blob(size);
    nt(BCryptExportKey(impl_->key,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),size,&size,0));BCRYPT_RSAKEY_BLOB h;memcpy(&h,blob.data(),sizeof(h));
    auto values=std::span(blob).subspan(sizeof(h));auto exponent=values.first(h.cbPublicExp);auto modulus=values.subspan(h.cbPublicExp,h.cbModulus);
    Bytes sequence=integerDer(modulus);append(sequence,integerDer(exponent));return derTag(0x30,sequence);
}
Bytes RsaOaepKey::encrypt(std::span<const uint8_t> plain)const{
    require(impl_->key && !plain.empty() && plain.size()<=214,"RSA OAEP 明文长度无效");
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA1_ALGORITHM,nullptr,0};Bytes out(256);ULONG n=0;
    nt(BCryptEncrypt(impl_->key,const_cast<PUCHAR>(plain.data()),ULONG(plain.size()),&padding,nullptr,0,out.data(),ULONG(out.size()),&n,BCRYPT_PAD_OAEP));out.resize(n);return out;
}
Bytes RsaOaepKey::decrypt(std::span<const uint8_t> cipher)const{
    require(impl_->key && cipher.size()==256,"RSA OAEP 密文长度无效");BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA1_ALGORITHM,nullptr,0};Bytes out(256);ULONG n=0;
    auto result=BCryptDecrypt(impl_->key,const_cast<PUCHAR>(cipher.data()),ULONG(cipher.size()),&padding,nullptr,0,out.data(),ULONG(out.size()),&n,BCRYPT_PAD_OAEP);
    if(result<0){SecureZeroMemory(out.data(),out.size());nt(result);}out.resize(n);return out;
}
Bytes frameGameBody(std::span<const uint8_t> body){require(body.size()>=2 && body.size()<=2*1024*1024-4,"游戏消息长度无效");Bytes out;leb(out,body.size()+4);append(out,body);return out;}
Bytes WorldMitm::clientFrame(std::span<const uint8_t> frame,size_t prefix){
    if(established_){auto out=cipher_.feed(frame);for(auto& f:cipher_.observations)observations.push_back(std::move(f));if(cipher_.verified())status_="握手与出站编码已自动验证，可以查询";return out;}
    observations.push_back({true,false,false,Bytes(frame.begin(),frame.end()),Bytes(frame.begin(),frame.end())});
    if(changed_)return Bytes(frame.begin(),frame.end());
    auto body=frame.subspan(prefix);
    if(readInteger(body,0,2,false)!=0x3610){passthrough_=true;status_="未识别首条世界握手，仅原样转发";return Bytes(frame.begin(),frame.end());}
    size_t pos=2,n=readLeb(body,pos);require(n<=512 && n<=body.size()-pos,"握手公钥长度无效");
    // The suffix is version/platform/language/string/flag. Validate the known
    // envelope but preserve every byte; never invent or change client identity.
    auto suffix=body.subspan(pos+n);require(suffix.size()>=9,"握手尾部被截断");size_t tail=7,str=readLeb(suffix,tail);require(str<=suffix.size()-tail && tail+str+1==suffix.size(),"握手尾部格式变化");
    clientKey_.importPublic(body.subspan(pos,n));proxyKey_.generate();auto der=proxyKey_.publicDer();
    Bytes replacement{0x10,0x36};leb(replacement,der.size());append(replacement,der);append(replacement,suffix);
    changed_=true;status_="已接管公钥握手，等待服务器会话密钥";return frameGameBody(replacement);
}
Bytes WorldMitm::serverFrame(std::span<const uint8_t> frame,size_t prefix){
    if(!changed_ || established_)return Bytes(frame.begin(),frame.end());auto body=frame.subspan(prefix);
    if(readInteger(body,0,2,false)!=0x3611)return Bytes(frame.begin(),frame.end());
    require(body.size()>=9,"握手响应被截断");
    require(readInteger(body,2,2,false)==0,"服务器拒绝握手，连接终止");
    size_t pos=8,n=readLeb(body,pos);require(n==256 && n<=body.size()-pos && body.size()-pos-n==12,"握手响应结构发生变化");
    auto secret=proxyKey_.decrypt(body.subspan(pos,n));
    try{
        auto wrapped=clientKey_.encrypt(secret);Bytes replacement(body.begin(),body.begin()+pos);append(replacement,wrapped);append(replacement,body.subspan(pos+n));
        cipher_.initialize(secret);SecureZeroMemory(secret.data(),secret.size());established_=true;status_="握手已完成，正在自动核对出站编码";return frameGameBody(replacement);
    }catch(...){SecureZeroMemory(secret.data(),secret.size());throw;}
}
Bytes WorldMitm::fromClient(std::span<const uint8_t> bytes){
    observations.clear();
    if(passthrough_){observations.push_back({true,false,false,Bytes(bytes.begin(),bytes.end()),Bytes(bytes.begin(),bytes.end())});return Bytes(bytes.begin(),bytes.end());}
    require(bytes.size()<=2*1024*1024 && clientPending_.size()+bytes.size()<=4*1024*1024,"客户端流缓存超过限制");append(clientPending_,bytes);
    // Local accelerators carry HTTPS and login traffic on the same listener.
    // Recognize the world opcode from its prefix without waiting for a whole
    // alleged game frame (a TLS/random length could otherwise stall traffic).
    if(!changed_){
        size_t prefix=0;while(prefix<clientPending_.size() && prefix<5){
            const auto byte=clientPending_[prefix++];
            if(!(byte&128)){
                if(clientPending_.size()>=prefix+2 && readInteger(clientPending_,prefix,2,false)!=0x3610){
                    passthrough_=true;status_="非世界握手连接，仅原样转发";
                    Bytes wire;wire.swap(clientPending_);observations.push_back({true,false,false,wire,wire});return wire;
                }
                break;
            }
        }
    }
    auto split=splitGameFrames(clientPending_);Bytes out;
    if(invalid(split)){if(changed_)throw std::runtime_error("客户端帧边界无效");passthrough_=true;status_="客户端帧格式未识别，仅原样转发";out.swap(clientPending_);for(size_t at=0;at<out.size();at+=65536){auto n=std::min<size_t>(65536,out.size()-at);Bytes b(out.begin()+at,out.begin()+at+n);observations.push_back({true,false,false,b,b});}return out;}
    for(auto f:split.frames){
        auto frame=std::span(clientPending_).subspan(f.offset,f.length);
        const auto observedBefore=observations.size();
        try{append(out,clientFrame(frame,f.prefixBytes));}catch(...){if(changed_)throw;passthrough_=true;status_="握手格式不受支持，连接保持原样";}
        if(passthrough_){ // Include the rejected frame exactly once, then its buffered tail.
            auto tail=std::span(clientPending_).subspan(f.offset);
            if(out.size()>f.offset)out.resize(f.offset);append(out,tail);
            // clientFrame observed only the first complete frame. Replace that
            // observation with the full pass-through tail, including fragments.
            observations.resize(observedBefore);
            for(size_t at=0;at<tail.size();at+=65536){auto part=tail.subspan(at,std::min<size_t>(65536,tail.size()-at));Bytes b(part.begin(),part.end());observations.push_back({true,false,false,b,b});}
            clientPending_.clear();return out;
        }
    }
    clientPending_.erase(clientPending_.begin(),clientPending_.begin()+split.consumed);return out;
}
Bytes WorldMitm::fromServer(std::span<const uint8_t> bytes){
    observations.clear();
    if(passthrough_){Bytes out;out.swap(serverPending_);append(out,bytes);for(size_t at=0;at<out.size();at+=65536){auto n=std::min<size_t>(65536,out.size()-at);Bytes b(out.begin()+at,out.begin()+at+n);observations.push_back({false,false,false,b,b});}return out;}
    require(bytes.size()<=2*1024*1024 && serverPending_.size()+bytes.size()<=4*1024*1024,"服务器流缓存超过限制");append(serverPending_,bytes);
    auto split=splitGameFrames(serverPending_);require(!invalid(split),"服务器帧边界无效");Bytes out;
    for(auto f:split.frames){auto frame=std::span(serverPending_).subspan(f.offset,f.length);append(out,serverFrame(frame,f.prefixBytes));observations.push_back({false,true,false,Bytes(frame.begin(),frame.end()),Bytes(frame.begin(),frame.end())});}
    serverPending_.erase(serverPending_.begin(),serverPending_.begin()+split.consumed);return out;
}
Bytes WorldMitm::mail(const MailRequest& request){
    require(ready() && clientBoundary(),"握手和编码尚未自动验证");
    auto plain=encodeMailRequest(request);auto wire=cipher_.mail(request);
    observations.clear();observations.push_back({true,true,true,wire,std::move(plain)});return wire;
}
Bytes WorldMitm::guild(bool search,uint8_t order,std::string_view name){
    require(ready() && clientBoundary(),"握手和编码尚未自动验证");
    auto plain=encodeGuildRequest(search,order,name);auto wire=cipher_.guild(search,order,name);
    observations.clear();observations.push_back({true,true,true,wire,plain});return wire;
}
Bytes WorldMitm::query(uint32_t server,uint64_t dbid){require(ready() && clientBoundary(),"握手和编码尚未自动验证");observations.clear();auto wire=cipher_.query(server,dbid);observations.push_back({true,true,true,wire,encodeViewCharRequest(server,dbid)});return wire;}
Bytes WorldMitm::jump(const JumpRequest& request){
    require(ready() && clientBoundary(),"握手和编码尚未自动验证");
    auto plain=encodeJumpRequest(request);observations.clear();auto wire=cipher_.jump(request);
    observations.push_back({true,true,true,wire,std::move(plain)});return wire;
}
Bytes WorldMitm::jumpMotion(std::span<const uint8_t> frame){
    require(ready() && clientBoundary(),"握手和编码尚未自动验证");
    require(decodeJumpMotion(frame).has_value(),"不支持的跳跃运动帧");observations.clear();
    auto wire=cipher_.jumpMotion(frame);observations.push_back({true,true,true,wire,Bytes(frame.begin(),frame.end())});return wire;
}
}
