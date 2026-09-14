#include "video_pipeline.h"
#include "screen_capture.h"
#include "video_codec.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>

namespace ld {
CaptureWorker::CaptureWorker(QObject *parent) : QObject(parent) {}
CaptureWorker::~CaptureWorker() = default;
void CaptureWorker::produce(quint64 generation) {
    if (generation != activeGeneration_.load(std::memory_order_relaxed)) return;
    if (!capture_ || generation != generation_) {
        generation_ = generation;
        capture_.reset(new ScreenCapture);
    }
    QElapsedTimer clock; clock.start();
    QImage image; QRect bounds; QString error;
    if (!capture_->capture(image, bounds, error)) { emit failed(generation, error); return; }
    if (generation != activeGeneration_.load(std::memory_order_relaxed)) return;
    const double captureMs = clock.nsecsElapsed() / 1e6;
    emit captured(generation, image, bounds, captureMs, capture_->backend());
}
EncodeWorker::EncodeWorker(QObject *parent) : QObject(parent) {}
EncodeWorker::~EncodeWorker() = default;
void EncodeWorker::encode(quint64 generation, QImage image, QRect bounds, QString codec, int fps,
                          double captureMs, QString backend) {
    if (generation != activeGeneration_.load(std::memory_order_relaxed)) return;
    if (!encoder_ || generation != generation_) { generation_ = generation; encoder_.reset(new VideoEncoder); }
    QElapsedTimer clock; clock.start();
    QString error;
    QByteArray bytes;
    if (codec == "h264") {
        if (!encoder_->encode(image, fps, bytes, error)) { emit failed(generation, error); return; }
    } else {
        image.setDevicePixelRatio(1);
        if (image.width() > 1920 || image.height() > 1080) image = image.scaled(1920, 1080, Qt::KeepAspectRatio, Qt::FastTransformation);
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "JPEG", 80)) { emit failed(generation, QStringLiteral("JPEG 编码不可用")); return; }
    }
    if (bytes.isEmpty() || bytes.size() >= 8 * 1024 * 1024) { emit failed(generation, QStringLiteral("视频编码结果为空或超限")); return; }
    emit produced(generation, bounds, bytes, codec, captureMs, clock.nsecsElapsed() / 1e6, backend);
}

DecodeWorker::DecodeWorker(QObject *parent) : QObject(parent) {}
DecodeWorker::~DecodeWorker() = default;
void DecodeWorker::decode(quint64 generation, QString codec, QByteArray bytes) {
    if (generation != activeGeneration_.load(std::memory_order_relaxed)) return;
    if (!decoder_ || generation_ != generation) { generation_ = generation; decoder_.reset(new VideoDecoder); }
    QElapsedTimer clock; clock.start();
    QImage image; QString error;
    if (codec == "h264") {
        if (!decoder_->decode(bytes, image, error)) { emit failed(generation, error); return; }
    } else {
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "JPEG");
        const QSize size = reader.size();
        if (!size.isValid() || size.width() > 1920 || size.height() > 1080) { emit failed(generation, QStringLiteral("远端图像尺寸超限")); return; }
        image = reader.read();
        if (image.isNull()) { emit failed(generation, QStringLiteral("远端画面损坏")); return; }
    }
    // Sample only; store counts, never image fingerprints, in diagnostic output.
    quint64 fingerprint = 1469598103934665603ULL;
    for (int y = 0; y < image.height(); y += qMax(1, image.height() / 36))
        for (int x = 0; x < image.width(); x += qMax(1, image.width() / 64)) {
            fingerprint ^= image.pixel(x, y); fingerprint *= 1099511628211ULL;
        }
    emit decoded(generation, image, fingerprint, clock.nsecsElapsed() / 1e6);
}
}
