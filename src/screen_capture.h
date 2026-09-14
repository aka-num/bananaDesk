#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <memory>

namespace ld {

// Construct, use and destroy an instance on the same worker thread. On X11,
// the application must call XInitThreads before constructing QApplication.
// Returned images own their pixels and remain valid after the next capture.
// Images may be downscaled to fit 1920x1080; bounds always uses native desktop
// coordinates so input mapping must use bounds, not the returned image size.
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture &) = delete;
    ScreenCapture &operator=(const ScreenCapture &) = delete;

    bool capture(QImage &image, QRect &bounds, QString &error);
    QString backend() const;
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
