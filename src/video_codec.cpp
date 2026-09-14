#include "video_codec.h"

#include <QThread>
#include <QThreadPool>
#include <QRunnable>
#include <QSemaphore>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#ifdef LANDESK_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace ld {
bool VideoDecoder::available() {
#ifdef LANDESK_HAVE_FFMPEG
    return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
#else
    return false;
#endif
}
#ifdef LANDESK_HAVE_FFMPEG
namespace {
constexpr int MaxWidth = 1920;
constexpr int MaxHeight = 1080;
constexpr int MaxCodedHeight = (MaxHeight + 15) & ~15; // H.264 macroblock padding
constexpr int MaxVideoPacket = 4 * 1024 * 1024;

QString avError(const char *operation, int result) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, detail, sizeof(detail));
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(operation), QString::fromUtf8(detail));
}

// Parse only the bounded baseline syntax that our encoder emits. Validation
// happens BEFORE libavcodec sees the packet, so untrusted SPS dimensions cannot
// cause image/reference allocation. max_pixels/get_buffer2 are a second guard.
class Bits {
public:
    explicit Bits(const QByteArray &data) : data_(data) {}
    uint32_t get(int count) {
        if (count < 0 || count > 32 || pos_ + count > data_.size() * 8) { ok = false; return 0; }
        uint32_t value = 0;
        for (int n = 0; n < count; ++n, ++pos_)
            value = (value << 1) | ((uint8_t(data_[pos_ / 8]) >> (7 - pos_ % 8)) & 1);
        return value;
    }
    uint32_t ue() {
        int zeros = 0;
        while (ok && get(1) == 0) {
            if (++zeros > 25) { ok = false; return 0; }
        }
        return ok ? ((uint32_t(1) << zeros) - 1 + get(zeros)) : 0;
    }
    bool ok = true;
private:
    const QByteArray &data_;
    int pos_ = 0;
};

QByteArray rbsp(const char *data, int size) {
    QByteArray out;
    out.reserve(size);
    int zeros = 0;
    for (int n = 0; n < size; ++n) {
        const auto byte = uint8_t(data[n]);
        if (zeros >= 2 && byte == 3) { zeros = 0; continue; }
        out.append(char(byte));
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

bool validateSps(const char *data, int size) {
    if (size < 4 || size > 512) return false;
    const QByteArray raw = rbsp(data, size);
    Bits bits(raw);
    if (bits.get(8) != 66) return false; // constrained baseline, 8-bit 4:2:0
    bits.get(8); // constraint flags
    if (bits.get(8) > 42 || bits.ue() != 0) return false;
    if (bits.ue() > 12) return false; // log2_max_frame_num_minus4
    const uint32_t poc = bits.ue();
    if (poc == 0) { if (bits.ue() > 12) return false; }
    else if (poc != 2) return false; // our no-B-frame stream uses POC 2
    if (bits.ue() > 1) return false; // bound DPB reference count
    bits.get(1); // gaps_in_frame_num_value_allowed_flag
    const uint32_t widthMbs = bits.ue();
    const uint32_t heightMbs = bits.ue();
    if (widthMbs >= uint32_t(MaxWidth / 16) || heightMbs >= uint32_t((MaxHeight + 15) / 16)) return false;
    if (bits.get(1) != 1) return false; // progressive frames only
    bits.get(1); // direct_8x8_inference_flag
    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (bits.get(1)) {
        cropLeft = bits.ue(); cropRight = bits.ue(); cropTop = bits.ue(); cropBottom = bits.ue();
    }
    const uint64_t codedWidth = (uint64_t(widthMbs) + 1) * 16;
    const uint64_t codedHeight = (uint64_t(heightMbs) + 1) * 16;
    const uint64_t cropX = (uint64_t(cropLeft) + cropRight) * 2;
    const uint64_t cropY = (uint64_t(cropTop) + cropBottom) * 2;
    return bits.ok && cropX < codedWidth && cropY < codedHeight &&
           codedWidth - cropX <= MaxWidth && codedHeight - cropY <= MaxHeight;
}

int startCode(const QByteArray &packet, int from, int &length) {
    for (int i = from; i + 2 < packet.size(); ++i) {
        if (packet[i] != 0 || packet[i + 1] != 0) continue;
        if (packet[i + 2] == 1) { length = 3; return i; }
        if (i + 3 < packet.size() && packet[i + 2] == 0 && packet[i + 3] == 1) { length = 4; return i; }
    }
    return -1;
}

bool validateAccessUnit(const QByteArray &packet, bool &hasSps, QString &error) {
    if (packet.isEmpty() || packet.size() > MaxVideoPacket) {
        error = QStringLiteral("H.264 packet exceeds the permitted size"); return false;
    }
    int prefix = 0;
    int position = startCode(packet, 0, prefix);
    if (position != 0) { error = QStringLiteral("Expected Annex-B H.264 packet"); return false; }
    bool foundSps = hasSps;
    bool hasSlice = false;
    int count = 0;
    while (position >= 0) {
        if (++count > 64) { error = QStringLiteral("Too many H.264 NAL units"); return false; }
        const int begin = position + prefix;
        int nextPrefix = 0;
        const int next = startCode(packet, begin, nextPrefix);
        const int end = next < 0 ? packet.size() : next;
        if (begin >= end || (uint8_t(packet[begin]) & 0x80)) {
            error = QStringLiteral("Malformed H.264 NAL unit"); return false;
        }
        const int type = uint8_t(packet[begin]) & 31;
        if (type == 7) {
            if (!validateSps(packet.constData() + begin + 1, end - begin - 1)) {
                error = QStringLiteral("Rejected H.264 SPS: only bounded 1920x1080 baseline video is accepted"); return false;
            }
            foundSps = true;
        } else if (type == 8) {
            if (end - begin > 512) { error = QStringLiteral("H.264 PPS too large"); return false; }
            const auto raw = rbsp(packet.constData() + begin + 1, end - begin - 1);
            Bits bits(raw);
            if (bits.ue() != 0 || bits.ue() != 0 || bits.get(1) != 0 || !bits.ok) {
                error = QStringLiteral("Unsupported H.264 PPS"); return false;
            }
        } else if (type == 1 || type == 5) {
            if (!foundSps) { error = QStringLiteral("H.264 stream must begin with SPS/IDR"); return false; }
            hasSlice = true;
        } else if (type != 6 && type != 9) {
            error = QStringLiteral("Unsupported H.264 NAL type"); return false;
        }
        position = next;
        prefix = nextPrefix;
    }
    if (!hasSlice) { error = QStringLiteral("H.264 packet has no picture"); return false; }
    hasSps = foundSps;
    return true;
}

int boundedBuffer(AVCodecContext *context, AVFrame *frame, int flags) {
    if (frame->width < 1 || frame->height < 1 || frame->width > MaxWidth || frame->height > MaxCodedHeight ||
        frame->format != AV_PIX_FMT_YUV420P) return AVERROR(EINVAL);
    return avcodec_default_get_buffer2(context, frame, flags);
}
}

struct VideoEncoder::Impl {
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *scale = nullptr;
    std::array<SwsContext *, 4> bandScales{};
    QThreadPool conversionPool;
    int width = 0;
    int height = 0;
    int fps = 0;
    int64_t sequence = 0;
    Impl() { conversionPool.setMaxThreadCount(4); conversionPool.setExpiryTimeout(-1); }
    ~Impl() {
        conversionPool.waitForDone();
        for (auto band : bandScales) sws_freeContext(band);
        sws_freeContext(scale);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
    bool initialize(int newWidth, int newHeight, int newFps, QString &error) {
        const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) { error = QStringLiteral("FFmpeg libx264 encoder is unavailable"); return false; }
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet) { error = QStringLiteral("Cannot allocate H.264 encoder"); return false; }
        width = newWidth; height = newHeight; fps = newFps;
        context->width = width;
        context->height = height;
        context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = AVRational{1, fps};
        context->framerate = AVRational{fps, 1};
        context->gop_size = fps;
        context->max_b_frames = 0;
        context->refs = 1;
        context->thread_count = std::clamp(QThread::idealThreadCount() / 2, 1, 8);
        context->thread_type = FF_THREAD_SLICE;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
#ifdef AV_PROFILE_H264_BASELINE
        context->profile = AV_PROFILE_H264_BASELINE;
#else
        context->profile = FF_PROFILE_H264_BASELINE;
#endif
        context->color_range = AVCOL_RANGE_MPEG;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
        // 12 Mbit/s VBV ceiling limits congestion without requiring a frame queue.
        // CRF preserves desktop text when the scene is inexpensive to encode.
        av_opt_set(context->priv_data, "preset", "ultrafast", 0);
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
        av_opt_set(context->priv_data, "crf", "23", 0);
        av_opt_set(context->priv_data, "profile", "baseline", 0);
        av_opt_set(context->priv_data, "level", "4.2", 0);
        av_opt_set(context->priv_data, "x264-params", "repeat-headers=1:annexb=1:scenecut=0:ref=1:bframes=0:sync-lookahead=0:rc-lookahead=0:vbv-maxrate=12000:vbv-bufsize=1000", 0);
        int result = avcodec_open2(context, codec, nullptr);
        if (result < 0) { error = avError("Open H.264 encoder", result); return false; }
        frame->format = context->pix_fmt;
        frame->width = width;
        frame->height = height;
        result = av_frame_get_buffer(frame, 32);
        if (result < 0) { error = avError("Allocate H.264 frame", result); return false; }
        return true;
    }
};

struct VideoDecoder::Impl {
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    SwsContext *scale = nullptr;
    bool hasSps = false;
    ~Impl() {
        sws_freeContext(scale);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
    bool initialize(QString &error) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) { error = QStringLiteral("FFmpeg H.264 decoder is unavailable"); return false; }
        context = avcodec_alloc_context3(codec);
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        if (!context || !frame || !packet) { error = QStringLiteral("Cannot allocate H.264 decoder"); return false; }
        context->thread_count = std::clamp(QThread::idealThreadCount() / 3, 1, 4);
        context->thread_type = FF_THREAD_SLICE;
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->max_pixels = int64_t(MaxWidth) * MaxCodedHeight;
        context->get_buffer2 = boundedBuffer;
#if LIBAVCODEC_VERSION_MAJOR < 60
        // Before libavcodec 60 this opt-in is required for custom allocation
        // callbacks with threading. The field was then removed and callbacks
        // are unconditionally required to be thread-safe. boundedBuffer has no
        // mutable shared state; retain the old opt-in without warning on 58/59.
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        context->thread_safe_callbacks = 1;
        QT_WARNING_POP
#endif
        context->err_recognition = AV_EF_EXPLODE | AV_EF_BITSTREAM;
        const int result = avcodec_open2(context, codec, nullptr);
        if (result < 0) { error = avError("Open H.264 decoder", result); return false; }
        return true;
    }
};

VideoEncoder::VideoEncoder() : impl_(new Impl) {}
VideoEncoder::~VideoEncoder() = default;
void VideoEncoder::reset() { impl_.reset(new Impl); }
bool VideoEncoder::available() { return avcodec_find_encoder_by_name("libx264") != nullptr; }
QString VideoEncoder::backend() const {
    return avcodec_find_encoder_by_name("libx264") ? QStringLiteral("H.264 / libx264 CPU") : QStringLiteral("H.264 unavailable");
}

bool VideoEncoder::encode(const QImage &input, int targetFps, QByteArray &output, QString &error) {
    output.clear(); error.clear();
    if (input.isNull() || input.width() < 2 || input.height() < 2 || input.width() > 8192 || input.height() > 8192) {
        error = QStringLiteral("H.264 capture size must be between 2x2 and 8192x8192"); return false;
    }
    const double factor = std::min({1.0, double(MaxWidth) / input.width(), double(MaxHeight) / input.height()});
    const int width = std::max(2, int(input.width() * factor) & ~1);
    const int height = std::max(2, int(input.height() * factor) & ~1);
    const int fps = std::clamp(targetFps, 1, 60);
    if (!impl_->context || impl_->width != width || impl_->height != height || impl_->fps != fps) {
        reset();
        if (!impl_->initialize(width, height, fps, error)) { reset(); return false; }
    }
    QImage converted;
    const QImage *source = &input;
    AVPixelFormat format;
    if (input.format() == QImage::Format_RGBA8888) format = AV_PIX_FMT_RGBA;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    else if (input.format() == QImage::Format_RGB32 || input.format() == QImage::Format_ARGB32) format = AV_PIX_FMT_BGRA;
#endif
    else { converted = input.convertToFormat(QImage::Format_RGBA8888); source = &converted; format = AV_PIX_FMT_RGBA; }
    if (source->isNull()) { error = QStringLiteral("Cannot convert capture image"); return false; }
    int result = av_frame_make_writable(impl_->frame);
    if (result < 0) { error = avError("Prepare H.264 frame", result); return false; }
    const uint8_t *planes[] = { source->constBits(), nullptr, nullptr, nullptr };
    const int strides[] = { int(source->bytesPerLine()), 0, 0, 0 };
    const int *matrix = sws_getCoefficients(SWS_CS_ITU709);
    const int integerRatio = source->width() / width;
    // libswscale's packed-RGB -> 4:2:0 conversion is single-threaded. Split
    // exact/integer resizes into four chroma-aligned, independent bands. For
    // fractional resizes use one scaler to preserve its global sampling grid.
    if (height >= 64 && QThread::idealThreadCount() >= 4 && source->width() == width * integerRatio &&
        source->height() == height * integerRatio) {
        QSemaphore completed;
        std::array<int, 4> results{};
        for (int n = 0; n < 4; ++n) {
            const int top = (height * n / 4) & ~1;
            const int bottom = (height * (n + 1) / 4) & ~1;
            impl_->conversionPool.start(QRunnable::create([&, n, top, bottom] {
                const int rows = bottom - top;
                auto &scaler = impl_->bandScales[size_t(n)];
                scaler = sws_getCachedContext(scaler, source->width(), rows * integerRatio, format,
                                             width, rows, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (scaler) {
                    sws_setColorspaceDetails(scaler, matrix, 1, matrix, 0, 0, 1 << 16, 1 << 16);
                    const uint8_t *bandSource[] = { source->constBits() + top * integerRatio * source->bytesPerLine(), nullptr, nullptr, nullptr };
                    uint8_t *bandDest[] = { impl_->frame->data[0] + top * impl_->frame->linesize[0],
                                           impl_->frame->data[1] + top / 2 * impl_->frame->linesize[1],
                                           impl_->frame->data[2] + top / 2 * impl_->frame->linesize[2], nullptr };
                    results[size_t(n)] = sws_scale(scaler, bandSource, strides, 0, rows * integerRatio,
                                                  bandDest, impl_->frame->linesize);
                }
                completed.release();
            }));
        }
        completed.acquire(4);
        result = results[0] + results[1] + results[2] + results[3];
    } else {
        impl_->scale = sws_getCachedContext(impl_->scale, source->width(), source->height(), format,
                                          width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl_->scale) { error = QStringLiteral("Cannot allocate H.264 color conversion"); return false; }
        sws_setColorspaceDetails(impl_->scale, matrix, 1, matrix, 0, 0, 1 << 16, 1 << 16);
        result = sws_scale(impl_->scale, planes, strides, 0, source->height(), impl_->frame->data, impl_->frame->linesize);
    }
    if (result != height) { error = QStringLiteral("H.264 color conversion failed"); return false; }
    impl_->frame->pts = impl_->sequence++;
    result = avcodec_send_frame(impl_->context, impl_->frame);
    if (result < 0) { error = avError("Encode H.264 frame", result); return false; }
    result = avcodec_receive_packet(impl_->context, impl_->packet);
    if (result < 0) { error = avError("Read low-latency H.264 frame", result); return false; }
    if (impl_->packet->size <= 0 || impl_->packet->size > MaxVideoPacket) {
        av_packet_unref(impl_->packet);
        error = QStringLiteral("Encoded H.264 frame exceeds packet limit"); reset(); return false;
    }
    output = QByteArray(reinterpret_cast<const char *>(impl_->packet->data), impl_->packet->size);
    av_packet_unref(impl_->packet);
    return true;
}

VideoDecoder::VideoDecoder() : impl_(new Impl) {}
VideoDecoder::~VideoDecoder() = default;
void VideoDecoder::reset() { impl_.reset(new Impl); }

bool VideoDecoder::decode(const QByteArray &input, QImage &output, QString &error) {
    output = QImage(); error.clear();
    if (!validateAccessUnit(input, impl_->hasSps, error)) return false;
    if (!impl_->context && !impl_->initialize(error)) { reset(); return false; }
    int result = av_new_packet(impl_->packet, input.size());
    if (result < 0) { error = avError("Allocate H.264 packet", result); return false; }
    // av_new_packet provides FFmpeg's required zero-filled input padding.
    std::copy(input.cbegin(), input.cend(), reinterpret_cast<char *>(impl_->packet->data));
    result = avcodec_send_packet(impl_->context, impl_->packet);
    av_packet_unref(impl_->packet);
    if (result < 0) { error = avError("Decode H.264 frame", result); reset(); return false; }
    result = avcodec_receive_frame(impl_->context, impl_->frame);
    if (result < 0) { error = avError("Read low-latency H.264 picture", result); reset(); return false; }
    AVFrame *frame = impl_->frame;
    if (frame->width <= 0 || frame->height <= 0 || frame->width > MaxWidth || frame->height > MaxHeight || frame->format != AV_PIX_FMT_YUV420P) {
        error = QStringLiteral("Decoded H.264 picture exceeds limits"); reset(); return false;
    }
    // Qt's raster painter has a direct opaque RGB32 path. Its integer pixel
    // layout is always 0xffRRGGBB; the byte order depends on the host endian.
    // FFmpeg supplies opaque alpha in both BGRA and ARGB output formats.
    output = QImage(frame->width, frame->height, QImage::Format_RGB32);
    if (output.isNull()) { error = QStringLiteral("Cannot allocate decoded image"); av_frame_unref(frame); return false; }
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    constexpr AVPixelFormat displayFormat = AV_PIX_FMT_BGRA;
#else
    constexpr AVPixelFormat displayFormat = AV_PIX_FMT_ARGB;
#endif
    impl_->scale = sws_getCachedContext(impl_->scale, frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                         frame->width, frame->height, displayFormat, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->scale) { error = QStringLiteral("Cannot allocate H.264 display conversion"); av_frame_unref(frame); return false; }
    const int *matrix = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(impl_->scale, matrix, 0, matrix, 1, 0, 1 << 16, 1 << 16);
    uint8_t *planes[] = { output.bits(), nullptr, nullptr, nullptr };
    const int strides[] = { int(output.bytesPerLine()), 0, 0, 0 };
    result = sws_scale(impl_->scale, frame->data, frame->linesize, 0, frame->height, planes, strides);
    av_frame_unref(frame);
    if (result != output.height()) { output = QImage(); error = QStringLiteral("H.264 display conversion failed"); return false; }
    return true;
}

#else
// The application can keep its JPEG path when FFmpeg was not found at build
// time (for example, a Windows build without the optional FFmpeg SDK).
struct VideoEncoder::Impl {};
struct VideoDecoder::Impl {};
VideoEncoder::VideoEncoder() : impl_(new Impl) {}
VideoEncoder::~VideoEncoder() = default;
void VideoEncoder::reset() {}
bool VideoEncoder::available() { return false; }
QString VideoEncoder::backend() const { return QStringLiteral("H.264 unavailable"); }
bool VideoEncoder::encode(const QImage &, int, QByteArray &packet, QString &error) {
    packet.clear(); error = QStringLiteral("Built without FFmpeg H.264 support"); return false;
}
VideoDecoder::VideoDecoder() : impl_(new Impl) {}
VideoDecoder::~VideoDecoder() = default;
void VideoDecoder::reset() {}
bool VideoDecoder::decode(const QByteArray &, QImage &image, QString &error) {
    image = QImage(); error = QStringLiteral("Built without FFmpeg H.264 support"); return false;
}
#endif
}
