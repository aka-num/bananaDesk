#include "screen_capture.h"

#include <QElapsedTimer>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#endif

namespace ld {

#ifndef Q_OS_WIN
namespace {
// Xlib's error handler is process-wide. Keep one dispatcher rather than
// temporarily replacing it for every frame (which races Qt and input threads).
// Only displays owned by this module are intercepted; all others are delegated.
struct XErrorRegistry {
    std::mutex mutex;
    std::unordered_map<Display *, std::atomic<int> *> displays;
    XErrorHandler previous = nullptr;
};

XErrorRegistry &errorRegistry() {
    // Deliberately process lifetime: Qt may still issue requests during shutdown.
    static auto *registry = new XErrorRegistry;
    return *registry;
}

int captureErrorHandler(Display *display, XErrorEvent *event) {
    auto &registry = errorRegistry();
    XErrorHandler previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto it = registry.displays.find(display);
        if (it != registry.displays.end()) {
            it->second->store(event->error_code, std::memory_order_relaxed);
            return 0;
        }
        previous = registry.previous;
    }
    return previous ? previous(display, event) : 0;
}

void registerDisplay(Display *display, std::atomic<int> *error) {
    auto &registry = errorRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    // Application creates its GUI before starting capture, so its existing
    // handler is preserved. Do not reinstall/swap while frames are in flight.
    static bool installed = false;
    if (!installed) {
        registry.previous = XSetErrorHandler(captureErrorHandler);
        installed = true;
    }
    registry.displays.emplace(display, error);
}

void unregisterDisplay(Display *display) {
    auto &registry = errorRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.displays.erase(display);
}

int colorChannel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    unsigned shift = 0;
    while (!(mask & 1UL)) { mask >>= 1; ++shift; }
    return int((((pixel >> shift) & mask) * 255UL + mask / 2) / mask);
}

QImage ownedImage(XImage *source) {
    const bool nativeOrder = source->byte_order == (Q_BYTE_ORDER == Q_LITTLE_ENDIAN ? LSBFirst : MSBFirst);
    if (nativeOrder && source->bits_per_pixel == 32 && source->red_mask == 0xff0000 &&
        source->green_mask == 0x00ff00 && source->blue_mask == 0x0000ff) {
        return QImage(reinterpret_cast<const uchar *>(source->data), source->width, source->height,
                      source->bytes_per_line, QImage::Format_RGB32).copy();
    }
    if (nativeOrder && source->bits_per_pixel == 16 && source->red_mask == 0xf800 &&
        source->green_mask == 0x07e0 && source->blue_mask == 0x001f) {
        return QImage(reinterpret_cast<const uchar *>(source->data), source->width, source->height,
                      source->bytes_per_line, QImage::Format_RGB16).copy();
    }
    // Unusual visuals (including 30-bit color) must not silently scramble color.
    QImage result(source->width, source->height, QImage::Format_RGB32);
    if (result.isNull()) return {};
    for (int y = 0; y < source->height; ++y) {
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source->width; ++x) {
            const unsigned long pixel = XGetPixel(source, x, y);
            row[x] = qRgb(colorChannel(pixel, source->red_mask), colorChannel(pixel, source->green_mask),
                          colorChannel(pixel, source->blue_mask));
        }
    }
    return result;
}
}
#endif

struct ScreenCapture::Impl {
    QRect bounds;
    QSize pixelSize;
    QString backend;
#ifdef Q_OS_WIN
    HDC desktop = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void *pixels = nullptr;

    void releaseImage() {
        if (memory && previousBitmap) SelectObject(memory, previousBitmap);
        previousBitmap = nullptr;
        if (bitmap) DeleteObject(bitmap);
        bitmap = nullptr;
        pixels = nullptr;
    }

    ~Impl() {
        releaseImage();
        if (memory) DeleteDC(memory);
        if (desktop) ReleaseDC(nullptr, desktop);
    }

    bool capture(QImage &image, QRect &outputBounds, QString &error) {
        POINT origin{};
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &info)) {
            error = QStringLiteral("无法获取 Windows 主显示器");
            return false;
        }
        const QRect current(info.rcMonitor.left, info.rcMonitor.top,
                            info.rcMonitor.right - info.rcMonitor.left,
                            info.rcMonitor.bottom - info.rcMonitor.top);
        if (current.width() <= 0 || current.height() <= 0 ||
            qint64(current.width()) * current.height() > 134217728) {
            error = QStringLiteral("显示器尺寸无效或过大");
            return false;
        }
        if (!desktop) desktop = GetDC(nullptr);
        if (desktop && !memory) memory = CreateCompatibleDC(desktop);
        if (!desktop || !memory) {
            error = QStringLiteral("无法创建 Windows 桌面采集上下文");
            return false;
        }
        if (!bitmap || bounds != current) {
            releaseImage();
            bounds = current;
            pixelSize = bounds.size().scaled(QSize(1920, 1080), Qt::KeepAspectRatio);
            if (pixelSize.width() > bounds.width()) pixelSize = bounds.size();
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = pixelSize.width();
            bitmapInfo.bmiHeader.biHeight = -pixelSize.height(); // top-down BGRA
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            bitmap = CreateDIBSection(desktop, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!bitmap || !pixels) {
                releaseImage();
                error = QStringLiteral("无法创建 Windows 桌面采集缓冲区");
                return false;
            }
            previousBitmap = SelectObject(memory, bitmap);
            if (!previousBitmap || previousBitmap == HGDI_ERROR) {
                previousBitmap = nullptr;
                releaseImage();
                error = QStringLiteral("无法选择 Windows 桌面采集缓冲区");
                return false;
            }
        }
        bool copied = false;
        if (pixelSize == bounds.size()) {
            copied = BitBlt(memory, 0, 0, pixelSize.width(), pixelSize.height(), desktop,
                            bounds.x(), bounds.y(), SRCCOPY | CAPTUREBLT);
        } else {
            SetStretchBltMode(memory, HALFTONE);
            SetBrushOrgEx(memory, 0, 0, nullptr);
            copied = StretchBlt(memory, 0, 0, pixelSize.width(), pixelSize.height(), desktop,
                                bounds.x(), bounds.y(), bounds.width(), bounds.height(), SRCCOPY | CAPTUREBLT);
        }
        if (!copied || !GdiFlush()) {
            error = QStringLiteral("Windows 桌面采集失败（会话可能已切换或锁屏）");
            return false;
        }
        image = QImage(static_cast<const uchar *>(pixels), pixelSize.width(), pixelSize.height(),
                       pixelSize.width() * 4, QImage::Format_RGB32).copy();
        if (image.isNull()) {
            error = QStringLiteral("无法分配桌面图像内存");
            return false;
        }
        outputBounds = bounds;
        backend = QStringLiteral("Windows GDI DIB");
        return true;
    }
#else
    Display *display = nullptr;
    Window root = 0;
    xcb_connection_t *xcb = nullptr;
    xcb_shm_seg_t fdSegment = 0;
    XImage *fdImage = nullptr;
    void *fdPixels = nullptr;
    size_t fdSize = 0;
    bool fdDisabled = false;
    bool renderAvailable = false;
    Pixmap scaledPixmap = 0;
    Picture sourcePicture = 0;
    Picture targetPicture = 0;
    XImage *shmImage = nullptr;
    XShmSegmentInfo shm{};
    bool shmAttached = false;
    bool shmMarkedForRemoval = false;
    bool shmDisabled = false;
    bool randrAvailable = false;
    bool randrMonitors = false;
    int randrEventBase = 0;
    std::atomic<int> xError{0};
    QElapsedTimer geometryTimer;
    Visual *visual = nullptr;
    unsigned int depth = 0;

    Impl() { shm.shmid = -1; }

    void releaseImage() {
        if (fdSegment && xcb) {
            auto *error = xcb_request_check(xcb, xcb_shm_detach_checked(xcb, fdSegment));
            std::free(error);
        }
        fdSegment = 0;
        if (fdImage) {
            fdImage->data = nullptr;
            XDestroyImage(fdImage);
            fdImage = nullptr;
        }
        if (fdPixels) munmap(fdPixels, fdSize);
        fdPixels = nullptr;
        fdSize = 0;
        if (display && shmAttached) {
            XShmDetach(display, &shm);
            XSync(display, False);
        }
        shmAttached = false;
        if (shmImage) {
            // XDestroyImage normally frees data; System V memory needs shmdt.
            shmImage->data = nullptr;
            XDestroyImage(shmImage);
            shmImage = nullptr;
        }
        if (shm.shmaddr && shm.shmaddr != reinterpret_cast<char *>(-1)) shmdt(shm.shmaddr);
        if (shm.shmid >= 0 && !shmMarkedForRemoval) shmctl(shm.shmid, IPC_RMID, nullptr);
        shm = {};
        shm.shmid = -1;
        shmMarkedForRemoval = false;
    }

    void releaseScaling() {
        if (display && sourcePicture) XRenderFreePicture(display, sourcePicture);
        if (display && targetPicture) XRenderFreePicture(display, targetPicture);
        if (display && scaledPixmap) XFreePixmap(display, scaledPixmap);
        sourcePicture = 0;
        targetPicture = 0;
        scaledPixmap = 0;
    }

    ~Impl() {
        releaseImage();
        releaseScaling();
        if (xcb) xcb_disconnect(xcb);
        if (display) {
            // Flush/close while the error route still points at live storage.
            XCloseDisplay(display);
            unregisterDisplay(display);
        }
    }

    bool open(QString &error) {
        if (display) return true;
        display = XOpenDisplay(nullptr);
        if (!display) {
            error = QStringLiteral("无法连接 X11 显示器（当前仅支持 X11 桌面）");
            return false;
        }
        registerDisplay(display, &xError);
        root = DefaultRootWindow(display);
        XSelectInput(display, root, StructureNotifyMask);
        int errorBase = 0;
        randrAvailable = XRRQueryExtension(display, &randrEventBase, &errorBase);
        if (randrAvailable) {
            int major = 1, minor = 5;
            if (XRRQueryVersion(display, &major, &minor))
                randrMonitors = major > 1 || (major == 1 && minor >= 5);
            XRRSelectInput(display, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
        }
        shmDisabled = !XShmQueryExtension(display);
        int renderEvent = 0, renderError = 0;
        renderAvailable = XRenderQueryExtension(display, &renderEvent, &renderError);
        // MIT-SHM 1.2 passes a memory FD over the local X socket. This also
        // works when client and server have separate System V IPC namespaces.
        int screenNumber = 0;
        xcb = xcb_connect(nullptr, &screenNumber);
        fdDisabled = !xcb || xcb_connection_has_error(xcb);
        if (!fdDisabled) {
            xcb_generic_error_t *xcbError = nullptr;
            auto *version = xcb_shm_query_version_reply(xcb, xcb_shm_query_version(xcb), &xcbError);
            fdDisabled = !version || xcbError ||
                (version->major_version < 1 || (version->major_version == 1 && version->minor_version < 2));
            std::free(xcbError);
            std::free(version);
        }
        XSync(display, False);
        xError.store(0);
        return true;
    }

    bool refreshGeometry(QString &error, bool force = false) {
        bool changed = false;
        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == ConfigureNotify ||
                (randrAvailable && (event.type == randrEventBase + RRScreenChangeNotify ||
                                    event.type == randrEventBase + RRNotify))) changed = true;
        }
        if (!force && !changed && geometryTimer.isValid() && geometryTimer.elapsed() < 1000) return true;
        XWindowAttributes attributes{};
        xError.store(0);
        if (!XGetWindowAttributes(display, root, &attributes) || xError.load()) {
            error = QStringLiteral("无法读取 X11 桌面尺寸");
            return false;
        }
        QRect current(0, 0, attributes.width, attributes.height);
        if (randrMonitors) {
            int count = 0;
            XRRMonitorInfo *monitors = XRRGetMonitors(display, root, True, &count);
            if (monitors && count > 0) {
                int selected = 0;
                for (int i = 0; i < count; ++i) if (monitors[i].primary) { selected = i; break; }
                const auto &monitor = monitors[selected];
                const QRect candidate(monitor.x, monitor.y, monitor.width, monitor.height);
                if (!candidate.isEmpty()) current = candidate.intersected(current);
            }
            if (monitors) XRRFreeMonitors(monitors);
        } else if (randrAvailable) {
            const RROutput primary = XRRGetOutputPrimary(display, root);
            XRRScreenResources *resources = XRRGetScreenResourcesCurrent(display, root);
            if (resources && primary != None) {
                XRROutputInfo *output = XRRGetOutputInfo(display, resources, primary);
                if (output && output->crtc != None) {
                    XRRCrtcInfo *crtc = XRRGetCrtcInfo(display, resources, output->crtc);
                    if (crtc) {
                        QRect candidate(crtc->x, crtc->y, int(crtc->width), int(crtc->height));
                        if (!candidate.isEmpty()) current = candidate.intersected(current);
                        XRRFreeCrtcInfo(crtc);
                    }
                }
                if (output) XRRFreeOutputInfo(output);
            }
            if (resources) XRRFreeScreenResources(resources);
        }
        if (current.isEmpty() || qint64(current.width()) * current.height() > 134217728) {
            error = QStringLiteral("X11 显示器尺寸无效或过大");
            return false;
        }
        if (bounds != current || visual != attributes.visual || depth != unsigned(attributes.depth)) {
            releaseImage();
            releaseScaling();
            bounds = current;
            visual = attributes.visual;
            depth = unsigned(attributes.depth);
            pixelSize = bounds.size().scaled(QSize(1920, 1080), Qt::KeepAspectRatio);
            if (pixelSize.width() > bounds.width()) pixelSize = bounds.size();
            if (pixelSize != bounds.size() && renderAvailable) {
                XRenderPictFormat *format = XRenderFindVisualFormat(display, visual);
                xError.store(0);
                if (format) {
                    scaledPixmap = XCreatePixmap(display, root, unsigned(pixelSize.width()), unsigned(pixelSize.height()), depth);
                    XRenderPictureAttributes pictureAttributes{};
                    pictureAttributes.subwindow_mode = IncludeInferiors;
                    sourcePicture = XRenderCreatePicture(display, root, format, CPSubwindowMode, &pictureAttributes);
                    targetPicture = XRenderCreatePicture(display, scaledPixmap, format, 0, nullptr);
                    XTransform transform{};
                    transform.matrix[0][0] = XDoubleToFixed(double(bounds.width()) / pixelSize.width());
                    transform.matrix[1][1] = XDoubleToFixed(double(bounds.height()) / pixelSize.height());
                    transform.matrix[0][2] = XDoubleToFixed(bounds.x());
                    transform.matrix[1][2] = XDoubleToFixed(bounds.y());
                    transform.matrix[2][2] = XDoubleToFixed(1);
                    XRenderSetPictureTransform(display, sourcePicture, &transform);
                    XRenderSetPictureFilter(display, sourcePicture, FilterBilinear, nullptr, 0);
                    XSync(display, False);
                }
                if (!format || xError.load() || !scaledPixmap || !sourcePicture || !targetPicture) {
                    releaseScaling();
                    pixelSize = bounds.size();
                }
            } else if (pixelSize != bounds.size()) pixelSize = bounds.size();
        }
        geometryTimer.restart();
        return true;
    }

    bool createShmImage() {
        if (shmImage) return true;
        xError.store(0);
        shmImage = XShmCreateImage(display, visual, depth, ZPixmap, nullptr, &shm,
                                  unsigned(pixelSize.width()), unsigned(pixelSize.height()));
        if (!shmImage) return false;
        const qint64 size = qint64(shmImage->bytes_per_line) * shmImage->height;
        if (size <= 0 || size > 512 * 1024 * 1024) { releaseImage(); return false; }
        shm.shmid = shmget(IPC_PRIVATE, size_t(size), IPC_CREAT | 0600);
        if (shm.shmid < 0) { releaseImage(); return false; }
        shm.shmaddr = static_cast<char *>(shmat(shm.shmid, nullptr, 0));
        if (shm.shmaddr == reinterpret_cast<char *>(-1)) { releaseImage(); return false; }
        shmImage->data = shm.shmaddr;
        shm.readOnly = False;
        const bool attached = XShmAttach(display, &shm);
        XSync(display, False); // Attach failure is asynchronous (remote X/containers).
        if (!attached || xError.load()) { releaseImage(); return false; }
        shmAttached = true;
        // Prevent orphaned segments after a client crash. Attach remains valid.
        shmMarkedForRemoval = shmctl(shm.shmid, IPC_RMID, nullptr) == 0;
        return true;
    }

    bool createFdImage() {
        if (fdImage) return true;
        fdImage = XCreateImage(display, visual, depth, ZPixmap, 0, nullptr,
                               unsigned(pixelSize.width()), unsigned(pixelSize.height()), 32, 0);
        if (!fdImage) return false;
        const qint64 size = qint64(fdImage->bytes_per_line) * fdImage->height;
        if (size <= 0 || size > 512 * 1024 * 1024) { releaseImage(); return false; }
        const auto segment = xcb_generate_id(xcb);
        xcb_generic_error_t *xcbError = nullptr;
        auto *reply = xcb_shm_create_segment_reply(xcb, xcb_shm_create_segment(xcb, segment, uint32_t(size), 0), &xcbError);
        const bool created = reply && !xcbError;
        std::free(xcbError);
        if (!created) { std::free(reply); releaseImage(); return false; }
        fdSegment = segment;
        int *fds = xcb_shm_create_segment_reply_fds(xcb, reply);
        void *pixels = reply->nfd == 1 ? mmap(nullptr, size_t(size), PROT_READ, MAP_SHARED, fds[0], 0) : MAP_FAILED;
        for (int i = 0; i < reply->nfd; ++i) close(fds[i]);
        std::free(reply);
        if (pixels == MAP_FAILED) { releaseImage(); return false; }
        fdPixels = pixels;
        fdSize = size_t(size);
        fdImage->data = static_cast<char *>(pixels);
        return true;
    }

    bool capture(QImage &image, QRect &outputBounds, QString &error) {
        if (!open(error)) return false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!refreshGeometry(error, attempt != 0)) return false;
            xError.store(0);
            if (scaledPixmap) {
                XRenderComposite(display, PictOpSrc, sourcePicture, None, targetPicture,
                                 0, 0, 0, 0, 0, 0, unsigned(pixelSize.width()), unsigned(pixelSize.height()));
                // FD capture uses another connection; complete the render first.
                XSync(display, False);
                if (xError.load()) {
                    error = QStringLiteral("X11 桌面缩放失败");
                    return false;
                }
            }
            const Drawable drawable = scaledPixmap ? scaledPixmap : root;
            const int captureX = scaledPixmap ? 0 : bounds.x();
            const int captureY = scaledPixmap ? 0 : bounds.y();
            if (!fdDisabled) {
                if (!createFdImage()) {
                    fdDisabled = true;
                } else {
                    xcb_generic_error_t *xcbError = nullptr;
                    auto *reply = xcb_shm_get_image_reply(xcb,
                        xcb_shm_get_image(xcb, uint32_t(drawable), int16_t(captureX), int16_t(captureY),
                                          uint16_t(pixelSize.width()), uint16_t(pixelSize.height()), ~0U,
                                          XCB_IMAGE_FORMAT_Z_PIXMAP, fdSegment, 0), &xcbError);
                    const bool ok = reply && !xcbError && reply->size <= fdSize;
                    std::free(xcbError);
                    std::free(reply);
                    if (ok) {
                        image = ownedImage(fdImage);
                        if (image.isNull()) { error = QStringLiteral("无法分配桌面图像内存"); return false; }
                        backend = scaledPixmap ? QStringLiteral("X11 XRender + MIT-SHM FD") : QStringLiteral("X11 MIT-SHM FD");
                        outputBounds = bounds;
                        return true;
                    }
                    releaseImage();
                    if (attempt == 0) continue;
                    fdDisabled = true;
                }
            }
            if (!shmDisabled) {
                if (!createShmImage()) {
                    shmDisabled = true;
                } else if (XShmGetImage(display, drawable, shmImage, captureX, captureY, AllPlanes) && !xError.load()) {
                    if (scaledPixmap) {
                        shmImage->red_mask = visual->red_mask;
                        shmImage->green_mask = visual->green_mask;
                        shmImage->blue_mask = visual->blue_mask;
                    }
                    image = ownedImage(shmImage);
                    if (image.isNull()) { error = QStringLiteral("无法分配桌面图像内存"); return false; }
                    backend = scaledPixmap ? QStringLiteral("X11 XRender + MIT-SHM") : QStringLiteral("X11 MIT-SHM");
                    outputBounds = bounds;
                    return true;
                } else {
                    // A topology change can invalidate dimensions between query
                    // and capture; re-query once before permanently falling back.
                    releaseImage();
                    if (attempt == 0) continue;
                    shmDisabled = true;
                }
            }
            xError.store(0);
            XImage *plain = XGetImage(display, drawable, captureX, captureY, unsigned(pixelSize.width()),
                                     unsigned(pixelSize.height()), AllPlanes, ZPixmap);
            if (plain && !xError.load()) {
                // XGetImage of a Pixmap has no associated visual and returns
                // zero channel masks. The scaling pixmap uses the root visual.
                if (scaledPixmap) {
                    plain->red_mask = visual->red_mask;
                    plain->green_mask = visual->green_mask;
                    plain->blue_mask = visual->blue_mask;
                }
                image = ownedImage(plain);
                XDestroyImage(plain);
                if (image.isNull()) { error = QStringLiteral("无法分配桌面图像内存"); return false; }
                backend = scaledPixmap ? QStringLiteral("X11 XRender + XGetImage") : QStringLiteral("X11 XGetImage");
                outputBounds = bounds;
                return true;
            }
            if (plain) XDestroyImage(plain);
        }
        error = QStringLiteral("X11 桌面采集失败（显示器配置或会话可能已变化）");
        return false;
    }
#endif
};

ScreenCapture::ScreenCapture() : impl_(new Impl) {}
ScreenCapture::~ScreenCapture() = default;

bool ScreenCapture::capture(QImage &image, QRect &bounds, QString &error) {
    image = {};
    bounds = {};
    error.clear();
    return impl_->capture(image, bounds, error);
}

QString ScreenCapture::backend() const { return impl_->backend; }
void ScreenCapture::reset() { impl_.reset(new Impl); }

}
