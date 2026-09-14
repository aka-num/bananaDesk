#pragma once
#include <QObject>
#include <QImage>
#include <QRect>
#include <memory>
#include <atomic>

namespace ld {
class ScreenCapture;
class VideoEncoder;
class VideoDecoder;

// One capture job per host; callers reserve frame credit before submitting work.
class CaptureWorker : public QObject {
    Q_OBJECT
public:
    explicit CaptureWorker(QObject *parent = nullptr);
    ~CaptureWorker() override;
    void activate(quint64 generation) { activeGeneration_.store(generation, std::memory_order_relaxed); }
public slots:
    void produce(quint64 generation);
signals:
    void captured(quint64 generation, QImage image, QRect bounds, double captureMs, QString backend);
    void failed(quint64 generation, QString error);
private:
    quint64 generation_ = 0;
    std::atomic<quint64> activeGeneration_{0};
    std::unique_ptr<ScreenCapture> capture_;
};
class EncodeWorker : public QObject {
    Q_OBJECT
public:
    explicit EncodeWorker(QObject *parent = nullptr);
    ~EncodeWorker() override;
    void activate(quint64 generation) { activeGeneration_.store(generation, std::memory_order_relaxed); }
public slots:
    void encode(quint64 generation, QImage image, QRect bounds, QString codec, int fps,
                double captureMs, QString backend);
signals:
    void produced(quint64 generation, QRect bounds, QByteArray bytes, QString codec,
                  double captureMs, double encodeMs, QString backend);
    void failed(quint64 generation, QString error);
private:
    quint64 generation_ = 0;
    std::atomic<quint64> activeGeneration_{0};
    std::unique_ptr<VideoEncoder> encoder_;
};

class DecodeWorker : public QObject {
    Q_OBJECT
public:
    explicit DecodeWorker(QObject *parent = nullptr);
    ~DecodeWorker() override;
    void activate(quint64 generation) { activeGeneration_.store(generation, std::memory_order_relaxed); }
public slots:
    void decode(quint64 generation, QString codec, QByteArray bytes);
signals:
    void decoded(quint64 generation, QImage image, quint64 fingerprint, double decodeMs);
    void failed(quint64 generation, QString error);
private:
    quint64 generation_ = 0;
    std::atomic<quint64> activeGeneration_{0};
    std::unique_ptr<VideoDecoder> decoder_;
};
}
