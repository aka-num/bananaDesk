#include "clipboard_sync.h"
#include <QClipboard>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QMimeData>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace ld {
namespace {
const QString OriginMime = QStringLiteral("application/x-bananadesk-clipboard-origin");

bool validUtf8(const QByteArray &bytes) {
    if (bytes.size() > MaxClipboardBytes) return false;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    for (int i = 0; i < bytes.size();) {
        const unsigned char first = data[i++];
        if (!first) return false;
        if (first < 0x80) continue;
        int remaining = 0;
        unsigned char lower = 0x80, upper = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) remaining = 1;
        else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            if (first == 0xe0) lower = 0xa0;
            if (first == 0xed) upper = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            if (first == 0xf0) lower = 0x90;
            if (first == 0xf4) upper = 0x8f;
        } else return false;
        if (i + remaining > bytes.size() || data[i] < lower || data[i] > upper) return false;
        ++i;
        for (int j = 1; j < remaining; ++j, ++i) if (data[i] < 0x80 || data[i] > 0xbf) return false;
    }
    return true;
}

QByteArray digest(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }

bool clipboardText(QClipboard *clipboard, QByteArray &bytes, bool &marked, QString *error = nullptr) {
    const QMimeData *mime = clipboard->mimeData(QClipboard::Clipboard);
    marked = mime && mime->hasFormat(OriginMime);
    if (!mime || mime->formats().isEmpty()) { bytes.clear(); return true; }
    if (mime->hasImage()) return false;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) if (url.isLocalFile()) return false;
        if (!mime->hasFormat(QStringLiteral("text/plain"))) return false;
    }
    if (!mime->hasText()) return false;
    const QString text = mime->text();
    if (text.size() > MaxClipboardBytes) {
        if (error) *error = QStringLiteral("剪贴板文本超过 64 KiB，未同步");
        return false;
    }
    bytes = text.toUtf8();
    // Reject malformed UTF-16 in local text instead of silently replacing it.
    if (QString::fromUtf8(bytes) != text || !validUtf8(bytes)) {
        if (error) *error = bytes.size() > MaxClipboardBytes ? QStringLiteral("剪贴板文本超过 64 KiB，未同步")
            : QStringLiteral("剪贴板文本编码无效或含空字符，未同步");
        return false;
    }
    return true;
}
}

struct ClipboardSync::Impl {
    struct Shared {
        int writing = 0;
        QSet<Impl *> instances;
        QByteArray protectedDigest;
        bool protectedCurrent = false;
        void protect(const QByteArray &hash) {
            protectedDigest = hash; protectedCurrent = true;
        }
        void forgetProtection() { protectedDigest.clear(); protectedCurrent = false; }
        bool isProtected(const QByteArray &hash) const { return protectedCurrent && protectedDigest == hash; }
        void changedLocally(const QByteArray &hash) {
            if (!isProtected(hash)) forgetProtection();
        }
    };
    static Shared &shared() { static Shared state; return state; }

    ClipboardSync *q;
    QClipboard *clipboard;
    QTimer timer;
    bool enabled = false, hasBaseline = false, hasPending = false, initialSnapshot = false;
    QByteArray baseline, pending;

    explicit Impl(ClipboardSync *owner) : q(owner), clipboard(QGuiApplication::clipboard()) {
        shared().instances.insert(this);
        timer.setSingleShot(true); timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { flush(); });
        if (clipboard) QObject::connect(clipboard, &QClipboard::changed, q, [this](QClipboard::Mode mode) {
            if (mode == QClipboard::Clipboard) changed();
        });
    }
    ~Impl() { clearPending(); shared().instances.remove(this); }
    void clearPending() { timer.stop(); pending.clear(); hasPending = false; }
    void suppress(const QByteArray &hash) {
        clearPending();
        initialSnapshot = false;
        if (enabled) { baseline = hash; hasBaseline = true; }
        else { baseline.clear(); hasBaseline = false; }
    }
    void changed() {
        if (!enabled || !clipboard) return;
        QByteArray bytes; bool marked = false; QString error;
        if (!clipboardText(clipboard, bytes, marked, &error)) {
            clearPending();
            hasBaseline = false; baseline.clear(); initialSnapshot = false;
            if (!marked && !shared().writing) shared().forgetProtection();
            if (!error.isEmpty()) emit q->status(error);
            return;
        }
        const auto hash = digest(bytes);
        if (marked || shared().writing || shared().isProtected(hash)) {
            if (marked) shared().protect(hash);
            suppress(hash); return;
        }
        shared().changedLocally(hash);
        // A first explicit copy after connection may equal the initial
        // snapshot. Later same-value owner announcements are duplicates.
        if (hasBaseline && baseline == hash && !initialSnapshot) return;
        initialSnapshot = false;
        baseline = hash; hasBaseline = true; pending = bytes; hasPending = true;
        timer.start();
    }
    void flush() {
        if (!enabled || !clipboard || !hasPending) { clearPending(); return; }
        const QByteArray candidate = pending;
        clearPending();
        QByteArray current; bool marked = false;
        if (!clipboardText(clipboard, current, marked) || marked || shared().writing ||
            current != candidate || shared().isProtected(digest(candidate))) return;
        emit q->send(candidate);
    }
    static void writeProtected(QClipboard *clipboard, const QByteArray &bytes, const QByteArray &origin) {
        if (!clipboard) return;
        auto &state = shared();
        const auto hash = digest(bytes); state.protect(hash);
        for (auto *instance : state.instances) instance->suppress(hash);
        auto *mime = new QMimeData;
        mime->setData(QStringLiteral("text/plain"), bytes);
        mime->setData(OriginMime, origin);
        ++state.writing;
        clipboard->setMimeData(mime, QClipboard::Clipboard);
        --state.writing;
    }
};

ClipboardSync::ClipboardSync(QObject *parent) : QObject(parent), impl_(new Impl(this)) {}
ClipboardSync::~ClipboardSync() = default;
bool ClipboardSync::enabled() const { return impl_->enabled; }
void ClipboardSync::setEnabled(bool enabled) {
    auto &s = *impl_;
    if (s.enabled == enabled) return;
    s.enabled = enabled; s.clearPending(); s.baseline.clear(); s.hasBaseline = false; s.initialSnapshot = false;
    if (enabled && s.clipboard) {
        QByteArray bytes; bool marked = false;
        if (clipboardText(s.clipboard, bytes, marked)) {
            s.baseline = digest(bytes); s.hasBaseline = true;
            if (marked) Impl::shared().protect(s.baseline);
            else Impl::shared().changedLocally(s.baseline);
            s.initialSnapshot = !marked && !Impl::shared().isProtected(s.baseline);
        } else if (!marked) Impl::shared().forgetProtection();
    }
}
bool ClipboardSync::receive(const QByteArray &utf8) {
    if (!impl_->enabled || !impl_->clipboard) return false;
    if (!validUtf8(utf8)) {
        emit status(QStringLiteral("剪贴板文本无效、含空字符或超过 64 KiB，已忽略"));
        return false;
    }
    Impl::writeProtected(impl_->clipboard, utf8, QByteArrayLiteral("remote"));
    return true;
}
void ClipboardSync::setLocalOnlyText(const QString &text) {
    Impl::writeProtected(QGuiApplication::clipboard(), text.toUtf8(), QByteArrayLiteral("local-only"));
}
}
