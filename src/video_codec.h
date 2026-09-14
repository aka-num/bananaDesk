#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <memory>

namespace ld {

// Each instance belongs to one worker thread. A successful encode returns one
// complete, ordered Annex-B access unit; never discard an encoded P frame.
// Drop uncoded captures instead, or reset the encoder after transport loss.
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;
    bool encode(const QImage &image, int targetFps, QByteArray &packet, QString &error);
    static bool available();
    QString backend() const;
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;
    bool decode(const QByteArray &packet, QImage &image, QString &error);
    static bool available();
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
