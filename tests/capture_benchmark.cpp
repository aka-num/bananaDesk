/*
Linux/X11 capture-only benchmark. Reads the current desktop into transient
process memory; it does not change the desktop, save screenshots, encode video,
or transmit image data. Prints aggregate timing/dimension metadata only.
Each path warms up for 20 captures, then measures 120 unpaced captures.
This measures QScreen and the current ScreenCapture implementation. The
historical native-4K FD-SHM result in docs/capture-benchmark.json was measured
before server-side downscaling was added; this executable now captures <=1080p.
Build from the source root (Qt5 example; replace Qt5 with Qt6 if required):
c++ -O2 -std=c++17 -fPIC tests/capture_benchmark.cpp src/screen_capture.cpp \
  -Isrc -o capture_benchmark $(pkg-config --cflags --libs Qt5Gui Qt5Core \
  x11 xext xrandr xrender xcb xcb-shm) -pthread
Run inside the X11 session: ./capture_benchmark
*/
#include "screen_capture.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPixmap>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <X11/Xlib.h>
#include <algorithm>
#include <vector>
#include <thread>

QJsonObject summary(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    double sum=0; for(auto v:values) sum+=v;
    return {{"mean_ms", sum/values.size()}, {"p50_ms",values[values.size()/2]},
      {"p95_ms", values[values.size()*95/100]}, {"samples",int(values.size())}};
}
int main(int argc, char **argv) {
    XInitThreads(); QGuiApplication app(argc,argv);
    QJsonObject output;
    std::vector<double> legacy;
    for(int i=0;i<140;i++) {
        QElapsedTimer timer; timer.start();
        QImage frame=app.primaryScreen()->grabWindow(0).toImage();
        const double ms=timer.nsecsElapsed()/1e6;
        if(frame.isNull()) return 2;
        if(i>=20) legacy.push_back(ms);
    }
    output["qscreen_grab_toimage"] = summary(legacy);
    int result = 0;
    std::thread worker([&]{
      ld::ScreenCapture capture;
      std::vector<double> native;
      QImage retained;
      for(int i=0;i<140;i++) {
        QImage frame; QRect bounds; QString error;
        QElapsedTimer timer; timer.start();
        if(!capture.capture(frame,bounds,error)) {qWarning()<<error;result=3;return;}
        const double ms=timer.nsecsElapsed()/1e6;
        if(i==0) retained=frame;
        if(i>=20) native.push_back(ms);
        if(i==139) {output["backend"]=capture.backend();output["width"]=frame.width();output["height"]=frame.height();
          output["x"]=bounds.x();output["y"]=bounds.y();}
      }
      if(retained.isNull()) result=4;
      output["native_owned_worker"] = summary(native);
      capture.reset();
      QImage frame; QRect bounds; QString error;
      output["reset_recaptures"]=capture.capture(frame,bounds,error);
    });
    worker.join();
    qInfo().noquote()<<QJsonDocument(output).toJson(QJsonDocument::Indented);
    return result;
}
