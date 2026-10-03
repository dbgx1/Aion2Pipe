#include "stationary_jump_fixture.hpp"
#include "query_proxy.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace aion;
static int checks=0;
static void check(bool ok,const char* what){++checks;if(!ok)throw std::runtime_error(what);}
template<class F>static void rejects(F f,const char* what){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,what);}
static Endpoint ep(uint8_t last,uint16_t port){Endpoint e;e.address[10]=e.address[11]=255;e.address[12]=127;e.address[15]=last;e.port=port;return e;}
static CipherSnapshot seed(Endpoint a,Endpoint b,uint32_t seq){CipherSnapshot s;s.source=a;s.destination=b;s.frameSequence=seq;s.i=1;for(unsigned i=0;i<256;++i)s.table[i]=uint8_t((i*73+19)%256);return s;}
static Bytes heartbeat(){return Bytes{14,1,0x36,1,2,3,4,5,6,7,8};}
static Bytes crypt(Bytes b,CipherSnapshot& s){for(auto f:splitGameFrames(b).frames)s.transform(std::span(b).subspan(f.offset+f.prefixBytes,f.length-f.prefixBytes));return b;}
static void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
static void streamTests(){
    auto a=ep(1,45000),b=ep(2,13328);auto initial=seed(a,b,0xfffffff0);Bytes history;
    for(int n=0;n<4;++n)append(history,heartbeat());
    auto client=initial;auto encrypted=crypt(history,client);
    for(size_t chunk=1;chunk<=encrypted.size();++chunk){
        QueryStream q(a,b,initial.frameSequence);Bytes forwarded;
        for(size_t at=0;at<encrypted.size();at+=chunk){auto input=std::span(encrypted).subspan(at,std::min(chunk,encrypted.size()-at));auto output=q.feed(input);check(output==Bytes(input.begin(),input.end()),"unarmed forwards immediately including partial frames");append(forwarded,output);}
        check(forwarded==encrypted,"unarmed preserves bytes");q.arm(initial);check(q.ready() && !q.takeNativeQuery(),"verified state arms without historical native request");
        auto receiver=client,sender=client;
        auto query=q.query(1005,282882351594255517ull);check(crypt(query,receiver)==encodeViewCharRequest(1005,282882351594255517ull),"server decrypts inserted request");
        check(crypt(q.guild(false,0,""),receiver)==encodeGuildRequest(false,0),"guild list advances upstream cipher only");
        auto longName=std::string(128,'x');check(crypt(q.guild(true,0,longName),receiver)==encodeGuildRequest(true,0,longName),"guild multi-byte frame prefix not encrypted");
        rejects([&]{q.guild(true,0,"");},"invalid guild request cannot advance cipher");
        MailRequest mail{"Test",std::string(50,'t'),std::string(500,'b')};
        check(crypt(q.mail(mail),receiver)==encodeMailRequest(mail),"mail insertion preserves multi-byte prefix and server cipher");
        rejects([&]{q.mail({"","",""});},"invalid mail cannot advance cipher");
        Bytes plain=heartbeat();append(plain,encodeViewCharRequest(99,123456));append(plain,heartbeat());auto wire=crypt(plain,sender);Bytes relayed;
        for(auto byte:wire){Bytes one{byte};append(relayed,q.feed(one));}
        check(relayed!=wire && crypt(relayed,receiver)==plain,"post insertion both ciphers remain synchronized across byte fragments");
        check(q.takeNativeQuery() && !q.takeNativeQuery(),"native queries detected once");
        check(crypt(q.query(2,456),receiver)==encodeViewCharRequest(2,456),"multiple insertions advance only server cipher");
        rejects([&]{q.arm(initial);},"cannot replace active state");
        rejects([&]{q.query(0,123);},"bad ID rejected");
        check(crypt(q.query(2,789),receiver)==encodeViewCharRequest(2,789),"invalid query does not advance state");
        wire=crypt(heartbeat(),sender);check(q.feed(std::span(wire).first(1)).empty(),"armed incomplete frame held");
        rejects([&]{q.query(1,2);},"no insertion inside frame");
        rejects([&]{q.guild(false,0,"");},"no guild insertion inside frame");
        rejects([&]{q.mail(mail);},"no mail insertion inside frame");
        check(crypt(q.feed(std::span(wire).subspan(1)),receiver)==heartbeat(),"partial frame completes after rejected insertion");
    }
    QueryStream q(a,b,initial.frameSequence);q.feed(encrypted);
    auto wrong=initial;wrong.source.port++;rejects([&]{q.arm(wrong);},"cross connection state rejected");
    wrong=initial;wrong.frameSequence++;rejects([&]{q.arm(wrong);},"non frame boundary rejected");
    wrong=initial;wrong.frameSequence-=1;rejects([&]{q.arm(wrong);},"stale anchor rejected");
    wrong=initial;wrong.i=127;rejects([&]{q.arm(wrong);},"wrong cipher rejected");check(!q.ready() && !q.takeNativeQuery(),"failed imports do not change state");
    wrong=initial;crypt(heartbeat(),wrong);wrong.frameSequence+=uint32_t(heartbeat().size());q.arm(wrong);check(q.ready(),"later valid anchor including TCP wrap supported");
    QueryStream few(a,b,initial.frameSequence);few.feed(std::span(encrypted).first(heartbeat().size()*2));rejects([&]{few.arm(initial);},"requires three validation frames");
    QueryStream opaque(a,b,1);Bytes invalid{0};check(opaque.feed(invalid)==invalid && !opaque.traceable(),"unknown framing fails open before arming");check(opaque.feed(encrypted)==encrypted,"opaque forwarding continues");rejects([&]{opaque.arm(initial);},"opaque connection cannot send queries");
    QueryStream partial(a,b,initial.frameSequence);partial.feed(std::span(encrypted).first(encrypted.size()-1));rejects([&]{partial.arm(initial);},"import at partial frame forbidden");
    auto state=initial;Bytes large{0x80,0x80,0x04};large.resize(65535,0xa5); // Encoded length 65536 -> 65535 wire bytes.
    QueryStream big(a,b,initial.frameSequence);big.feed(encrypted);big.arm(initial);state=client;auto largeWire=crypt(large,state);Bytes got;
    for(size_t at=0;at<largeWire.size();at+=37)append(got,big.feed(std::span(largeWire).subspan(at,std::min<size_t>(37,largeWire.size()-at))));
    check(got==largeWire,"multibyte length prefixes are not encrypted; unknown bodies preserved");
}
static Bytes ipPacket(Endpoint a,Endpoint b){
    Bytes raw(47);raw[0]=0x45;raw[3]=47;raw[8]=64;raw[9]=6;
    std::copy(a.address.begin()+12,a.address.end(),raw.begin()+12);std::copy(b.address.begin()+12,b.address.end(),raw.begin()+16);
    auto put=[&](size_t at,uint16_t v){raw[at]=uint8_t(v>>8);raw[at+1]=uint8_t(v);};put(20,a.port);put(22,b.port);
    for(unsigned i=24;i<32;++i)raw[i]=uint8_t(i);raw[32]=0x60;raw[33]=0x18;raw[34]=0x40;
    raw[40]=1;raw[41]=1;raw[42]=1;raw[43]=1;raw[44]=6;raw[45]=7;raw[46]=8;return raw;
}
static void routeTests(){
    check(defaultProxyPriority>1040 && defaultProxyPriority<=WINDIVERT_PRIORITY_HIGHEST,"game proxy runs before mitmproxy local redirector");
    QueryRoute r{ep(1,45000),ep(2,13328),ep(1,45001),1234};uint16_t listen=41000,alt=13329;
    auto run=[&](Endpoint from,Endpoint to,bool outbound,Endpoint wantFrom,Endpoint wantTo,bool wantOut){
        auto raw=ipPacket(from,to),before=raw;check(rewriteQueryRoute(raw,outbound,r,listen,alt),"exact route matches");Packet p;std::string error;
        check(parsePacket(raw,p,error) && p.source==wantFrom && p.destination==wantTo && outbound==wantOut,"route endpoints and direction");
        check(std::equal(raw.begin()+24,raw.end(),before.begin()+24),"TCP seq ACK flags options payload unchanged");
    };
    run(r.client,r.remote,true,ep(2,45000),ep(1,listen),false);
    run(ep(1,listen),ep(2,45000),true,r.remote,r.client,false);
    run(r.upstream,ep(2,alt),true,r.upstream,r.remote,true);
    run(r.remote,r.upstream,false,ep(2,alt),r.upstream,false);
    auto raw=ipPacket(ep(1,49999),r.remote),original=raw;bool out=true;
    check(!rewriteQueryRoute(raw,out,r,listen,alt) && raw==original && out,"unrelated same server connection untouched");
    raw=ipPacket(r.client,r.remote);out=false;check(!rewriteQueryRoute(raw,out,r,listen,alt),"inbound cannot impersonate client route");
    raw.resize(16);check(!rewriteQueryRoute(raw,out,r,listen,alt),"malformed packet untouched");
}
static void handshakeTests(){
    for(unsigned version:{3544u,3526u})for(size_t chunk:{1,2,11,1000}){
        RsaOaepKey client;client.generate();auto publicKey=client.publicDer();RsaOaepKey publicOnly;publicOnly.importPublic(publicKey);
        Bytes secret(32);for(size_t i=0;i<secret.size();++i)secret[i]=uint8_t(i*13+7);
        check(client.decrypt(publicOnly.encrypt(secret))==secret,"CNG OAEP public/private round trip");
        auto malformed=publicKey;malformed.push_back(0);RsaOaepKey rejected;rejects([&]{rejected.importPublic(malformed);},"DER trailing data rejected");
        Bytes body{0x10,0x36,uint8_t((publicKey.size()&127)|128),uint8_t(publicKey.size()>>7)};append(body,publicKey);append(body,Bytes{uint8_t(version),uint8_t(version>>8),0,0,3,6,3,0,0});auto request=frameGameBody(body);
        WorldMitm mitm(ep(1,45000),ep(2,13328),100);Bytes routed;
        for(size_t at=0;at<request.size();at+=chunk)append(routed,mitm.fromClient(std::span(request).subspan(at,std::min(chunk,request.size()-at))));
        check(mitm.changed() && !mitm.established() && !mitm.ready(),"handshake rewrite waits for server key");
        auto f=splitGameFrames(routed).frames[0];size_t keyAt=f.prefixBytes+4;size_t keyLen=(routed[f.prefixBytes+2]&127)|(size_t(routed[f.prefixBytes+3])<<7);
        RsaOaepKey server;server.importPublic(std::span(routed).subspan(keyAt,keyLen));check(server.publicDer()!=publicKey,"proxy supplies independent public key");
        check(std::equal(routed.end()-9,routed.end(),request.end()-9),"handshake client suffix retained exactly");
        Bytes reply{0x11,0x36,0,0,1,2,3,4,0x80,2};append(reply,server.encrypt(secret));reply.resize(reply.size()+12,0);auto response=frameGameBody(reply);Bytes wrapped;
        for(size_t at=0;at<response.size();at+=chunk)append(wrapped,mitm.fromServer(std::span(response).subspan(at,std::min(chunk,response.size()-at))));
        f=splitGameFrames(wrapped).frames[0];check(client.decrypt(std::span(wrapped).subspan(f.prefixBytes+10,256))==secret,"game receives original server secret under its own key");
        check(mitm.established() && !mitm.ready(),"no query until stream validation");rejects([&]{mitm.query(1,2);},"cannot query during validation");
        // Independent KSA formulation, with a known standard RC4 vector also
        // checked in Python/OpenSSL integration; fragmentation is this test's focus.
        CipherSnapshot c;c.i=1;for(unsigned i=0;i<256;++i)c.table[i]=uint8_t(i);unsigned j=0;for(unsigned i=0;i<256;++i){j=(j+c.table[i]+secret[i%secret.size()])%256;std::swap(c.table[i],c.table[j]);}auto receiver=c;
        for(int i=0;i<3;++i){auto wire=crypt(heartbeat(),c);check(mitm.fromClient(wire)==wire,"automatic initial cipher relays unmodified");crypt(wire,receiver);}
        check(mitm.ready(),"three parsed frames enable fully automatic queries");
        MailRequest mail{"Player","Test","Body"};
        check(crypt(mitm.mail(mail),receiver)==encodeMailRequest(mail),"mail uses synchronized server cipher");
        check(mitm.observations.size()==1 && mitm.observations[0].toolGenerated && mitm.observations[0].plain==encodeMailRequest(mail),"mail plaintext observation");
        JumpRequest jump{{1,2,3},90,{0,0,1000},10000};
        check(crypt(mitm.jump(jump),receiver)==encodeJumpRequest(jump),"jump uses server cipher without shifting native client cipher");
        check(mitm.observations.size()==1 && mitm.observations[0].toolGenerated && mitm.observations[0].plain==encodeJumpRequest(jump),"generated jump is recorded with source and plaintext");
        auto invalidJump=jump;invalidJump.velocity[2]=0;rejects([&]{mitm.jump(invalidJump);},"invalid jump cannot advance cipher");
        auto nextNative=crypt(heartbeat(),c);check(mitm.fromClient(std::span(nextNative).first(1)).empty(),"split native frame held before jump");
        rejects([&]{mitm.jump(jump);},"jump cannot split a native frame");
        check(crypt(mitm.fromClient(std::span(nextNative).subspan(1)),receiver)==heartbeat(),"invalid and boundary-rejected jumps preserve both cipher states");
        for(const auto& motion:stationaryJumpFixture()){
            auto wrong=motion;wrong[0]++;rejects([&]{mitm.jumpMotion(wrong);},"invalid motion cannot advance either cipher");
            check(crypt(mitm.jumpMotion(motion),receiver)==motion,"all jump phases encrypted exactly for server");
            check(mitm.observations.size()==1 && mitm.observations[0].toolGenerated && mitm.observations[0].plain==motion,"each generated phase recorded");
            auto beat=crypt(heartbeat(),c);check(mitm.fromClient(std::span(beat).first(1)).empty(),"native partial heartbeat buffered");
            rejects([&]{mitm.jumpMotion(motion);},"sequence step cannot split native frame");
            check(crypt(mitm.fromClient(std::span(beat).subspan(1)),receiver)==heartbeat(),"interleaved native heartbeat retains cipher sync");
        }
        check(crypt(mitm.query(1005,123456),receiver)==encodeViewCharRequest(1005,123456),"automatic query accepted by server cipher");
        check(crypt(mitm.fromClient(crypt(heartbeat(),c)),receiver)==heartbeat(),"automatic relay stays synchronized after query");
        WorldMitm opaque(ep(1,1),ep(2,2),0);Bytes unknown{0,1,2,3};check(opaque.fromClient(unknown)==unknown,"unknown first message is pass through");check(opaque.fromServer(unknown)==unknown,"opaque reverse stream preserves fragments");
        WorldMitm bad(ep(1,1),ep(2,2),0);auto changed=bad.fromClient(request);check(bad.changed(),"negative OAEP test began handshake");rejects([&]{bad.fromServer(response);},"cipher for a different proxy key rejected");
    }
}
static void opaqueObservationTests(){
    {
        WorldMitm relay(ep(1,45000),ep(1,57633),0);
        const Bytes tlsHeader{0x16,0x03,0x01};
        check(relay.fromClient(tlsHeader)==tlsHeader,"relay forwards non-world prefix without waiting for an alleged frame length");
        const Bytes reply{0x15,0x03,0x03,0,0};
        check(relay.fromServer(reply)==reply && !relay.changed() && !relay.ready(),"non-world relay traffic never changes handshake or enables queries");
    }

    // A valid but unsupported first frame followed by an incomplete second
    // frame in the same read must survive both forwarding and session export.
    for(const auto& body:{Bytes{0x34,0x12,0xaa},Bytes{0x10,0x36,1,0}}){
    const auto first=frameGameBody(body);
    Bytes input=first;append(input,Bytes{0x30,0x11,0x22,0x33});
    for(size_t chunk=1;chunk<=input.size();++chunk){
        WorldMitm proxy(ep(1,45000),ep(2,13328),0);Bytes wire,recorded,original;
        for(size_t at=0;at<input.size();at+=chunk){
            append(wire,proxy.fromClient(std::span(input).subspan(at,std::min(chunk,input.size()-at))));
            for(const auto& event:proxy.observations){
                check(event.outbound && !event.plaintext && !event.toolGenerated,"opaque observations never claim decryption or generated traffic");
                append(recorded,event.plain);append(original,event.wire);
            }
        }
        check(wire==input,"unsupported coalesced frames forward all bytes exactly once");
        check(recorded==input && original==input,"session observations preserve the entire opaque stream, including coalesced tail");
        check(!proxy.ready() && !proxy.changed(),"opaque recording cannot enable query or alter handshake");
        rejects([&]{proxy.query(1,2);},"opaque connections reject active queries");
        rejects([&]{proxy.jump(JumpRequest{{1,2,3},0,{0,0,1000},10000});},"opaque connections reject jumps");
    }
    }
}
int main(){try{streamTests();routeTests();handshakeTests();opaqueObservationTests();std::cout<<checks<<" query checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
