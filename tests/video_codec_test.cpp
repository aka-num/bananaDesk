// Standalone H.264 interoperability and resource-limit checks; uses synthetic images only.
#include "video_codec.h"
#include <QCoreApplication>
#include <cstdio>
#include <vector>
extern "C" {
#include <libavutil/log.h>
}
struct Bits {
    std::vector<int> bits;
    void fixed(unsigned v,int n) { for(int i=n-1;i>=0;--i)bits.push_back((v>>i)&1); }
    void ue(unsigned v) { unsigned x=v+1; int n=0; for(unsigned t=x;t>1;t>>=1)++n; fixed(0,n);fixed(x,n+1); }
    QByteArray bytes() { fixed(1,1);while(bits.size()%8)fixed(0,1);QByteArray a;for(size_t i=0;i<bits.size();i+=8) {unsigned b=0;for(int n=0;n<8;++n)b=b*2+bits[i+n];a.append(char(b));}return a; }
};
QByteArray sps(int widthMbs,int heightMbs,int refs=1) {
    Bits b; b.fixed(66,8);b.fixed(0xc0,8);b.fixed(42,8);b.ue(0);b.ue(0);b.ue(2);b.ue(refs);b.fixed(0,1);b.ue(widthMbs-1);b.ue(heightMbs-1);b.fixed(1,1);b.fixed(1,1);b.fixed(0,1);b.fixed(0,1);
    QByteArray out=QByteArray::fromHex("0000000167");out+=b.bytes();out+=QByteArray::fromHex("00000165888400");return out;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);av_log_set_level(AV_LOG_QUIET);
    int count=0;
    auto check=[&](bool ok,const char*name) {++count;std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)std::exit(1);};
    ld::VideoEncoder encoder;ld::VideoDecoder decoder;QString error;QByteArray packet;QImage decoded;
    check(ld::VideoEncoder::available(),"libx264 available");
    for (const QSize size : {QSize(640,360),QSize(1920,1080),QSize(1919,1079),QSize(3840,2160),QSize(3840,1600),QSize(1080,1920)}) {
        QImage image(size,QImage::Format_RGBA8888);image.fill(QColor(73,121,191));
        check(encoder.encode(image,60,packet,error),"encode resolution change");
        check(decoder.decode(packet,decoded,error),"decode resolution change");
        check(decoded.width()<=1920&&decoded.height()<=1080&&decoded.width()%2==0&&decoded.height()%2==0,"bounded even decoded size");
        const auto color=decoded.pixelColor(decoded.width()/2,decoded.height()/2);
        check(std::abs(color.red()-73)<8&&std::abs(color.green()-121)<8&&std::abs(color.blue()-191)<8,"BT709 RGB round-trip color");
    }
    ld::VideoDecoder malformed;
    check(!malformed.decode(QByteArray(),decoded,error),"reject empty packet");
    check(!malformed.decode(QByteArray(4*1024*1024+1,'x'),decoded,error),"reject oversized packet");
    check(!malformed.decode(sps(1000000,1000000),decoded,error)&&error.contains("SPS"),"reject huge SPS before codec initialization");
    check(!malformed.decode(sps(121,68),decoded,error)&&error.contains("SPS"),"reject width beyond 1920");
    check(!malformed.decode(sps(120,69),decoded,error)&&error.contains("SPS"),"reject height beyond coded 1088");
    check(!malformed.decode(sps(120,68),decoded,error)&&error.contains("SPS"),"reject uncropped 1088 display height");
    check(!malformed.decode(sps(120,67,16),decoded,error)&&error.contains("SPS"),"reject excess reference frame count");
    encoder.reset();decoder.reset();QImage image(640,360,QImage::Format_RGB32);image.fill(Qt::red);
    check(encoder.encode(image,60,packet,error)&&decoder.decode(packet,decoded,error),"reset produces independently decodable IDR");
    check(!malformed.decode(packet.left(packet.size()/2),decoded,error),"reject truncated packet");
    std::printf("%d codec checks passed\n",count);
}
