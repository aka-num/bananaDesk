#include "file_transfer.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <vector>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ld {
namespace {
enum class Mode { Idle, List, Upload, Download, Offer, AwaitReceipt };
constexpr int OfferTimeoutMs = 60 * 1000;
const QString StagingPrefix = QStringLiteral(".landesk-upload-");
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'); }
QByteArray encode(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject message(const QString &id, const char *op) {
    return {{"v", 1}, {"id", id}, {"op", QString::fromLatin1(op)}};
}
bool validId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}
bool number(const QJsonValue &value, qint64 &result) {
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || n < 0 || n > double(MaxTransferFileSize) || std::floor(n) != n) return false;
    result = qint64(n); return true;
}
bool validName(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ') ||
        name.startsWith(StagingPrefix, Qt::CaseInsensitive) || name.toUtf8().size() > 255 ||
        QString::fromUtf8(name.toUtf8()) != name) return false;
    const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (QChar c : name) if (c.unicode() < 32 || c.unicode() == 127 || forbidden.contains(c)) return false;
    const QString stem = name.section('.', 0, 0).toUpper();
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|COM[1-9¹²³]|LPT[1-9¹²³])$"));
    return !reserved.match(stem).hasMatch();
}
bool parse(const QByteArray &payload, QJsonObject &object) {
    if (payload.isEmpty() || payload.size() > MaxFilePayload) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    object = document.object();
    return object.value("v").isDouble() && object.value("v").toDouble() == 1 &&
        validId(object.value("id").toString()) && object.value("op").isString();
}
bool decodeChunk(const QJsonValue &value, QByteArray &bytes, bool allowEmpty) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.size() > ((FileChunkSize + 2) / 3) * 4) return false;
    const QByteArray encoded = text.toLatin1();
    bytes = QByteArray::fromBase64(encoded, QByteArray::AbortOnBase64DecodingErrors);
    return bytes.size() <= FileChunkSize && (allowEmpty || !bytes.isEmpty()) &&
        bytes.toBase64() == encoded && QString::fromLatin1(encoded) == text;
}

// Pin the selected directory for the entire operation. A local rename or
// symlink substitution must never redirect an upload outside the selected root.
class RootDirectory {
public:
    ~RootDirectory() {
#ifdef Q_OS_WIN
        for (HANDLE handle : handles_) CloseHandle(handle);
#else
        if (fd >= 0) ::close(fd);
#endif
    }
    bool open(const QString &root, bool create = false) {
#ifdef Q_OS_WIN
        QStringList components;
        QString current = root;
        for (;;) {
            components.prepend(current);
            const QString parent = QFileInfo(current).dir().absolutePath();
            if (parent == current) break;
            current = parent;
        }
        for (const auto &component : components) {
            if (create && !QFileInfo::exists(component) && !QDir().mkdir(component)) return false;
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(component.utf16()), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            handles_.push_back(handle);
            if (!GetFileInformationByHandle(handle, &identity_) ||
                !(identity_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (identity_.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
        path_ = root;
        return !handles_.empty();
#else
        fd = ::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        if (fd < 0) return false;
        for (const auto &component : root.split('/', Qt::SkipEmptyParts)) {
            const QByteArray name = QFile::encodeName(component);
            int next = ::openat(fd, name.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (next < 0 && create && errno == ENOENT) {
                if (::mkdirat(fd, name.constData(), 0700) != 0 && errno != EEXIST) return false;
                next = ::openat(fd, name.constData(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            }
            if (next < 0) return false;
            ::close(fd); fd = next;
        }
        return ::fstat(fd, &identity_) == 0;
#endif
    }
    QString path() const {
#ifdef Q_OS_WIN
        return path_;
#else
        return QStringLiteral("/proc/self/fd/%1").arg(fd);
#endif
    }
    bool unchanged(const QString &root) const {
        RootDirectory current;
        if (!current.open(root)) return false;
#ifdef Q_OS_WIN
        return identity_.dwVolumeSerialNumber == current.identity_.dwVolumeSerialNumber &&
            identity_.nFileIndexHigh == current.identity_.nFileIndexHigh && identity_.nFileIndexLow == current.identity_.nFileIndexLow;
#else
        return identity_.st_dev == current.identity_.st_dev && identity_.st_ino == current.identity_.st_ino;
#endif
    }
    void remove(const QString &name) {
#ifdef Q_OS_WIN
        QFile::remove(QDir(path()).filePath(name));
#else
        ::unlinkat(fd, QFile::encodeName(name).constData(), 0);
#endif
    }
    bool publish(const QString &name, const QString &destination) {
#ifdef Q_OS_WIN
        const QString from = QDir(path()).filePath(name), to = QDir(path()).filePath(destination);
        return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()), reinterpret_cast<LPCWSTR>(to.utf16()), MOVEFILE_WRITE_THROUGH) != 0;
#else
        if (::linkat(fd, QFile::encodeName(name).constData(), fd, QFile::encodeName(destination).constData(), 0) != 0) return false;
        remove(name); return true;
#endif
    }
#ifndef Q_OS_WIN
    int fd = -1;
#endif
private:
#ifdef Q_OS_WIN
    std::vector<HANDLE> handles_;
    BY_HANDLE_FILE_INFORMATION identity_{};
    QString path_;
#else
    struct stat identity_{};
#endif
};

// Open the remote file itself, not a symlink target. The OS handle is checked
// before being handed to QFile so path replacement cannot follow a symlink.
bool openRegular(QFile &file, const RootDirectory &root, const QString &name) {
#ifdef Q_OS_WIN
    const QString path = QDir(root.path()).filePath(name);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(handle); return false;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); return false; }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { _close(fd); return false; }
#else
    const int fd = ::openat(root.fd, QFile::encodeName(name).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info{};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode)) { ::close(fd); return false; }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return false; }
#endif
    return true;
}

}

struct FileTransferHost::Impl {
    explicit Impl(FileTransferHost *owner) : q(owner), offerTimer(owner) {
        offerTimer.setObjectName(QStringLiteral("fileOfferTimeout"));
        offerTimer.setSingleShot(true); offerTimer.setInterval(OfferTimeoutMs);
        QObject::connect(&offerTimer, &QTimer::timeout, q, [this] {
            if (!outgoingOffer) return;
            const QString transaction = id;
            const QString text = mode == Mode::AwaitReceipt
                ? QStringLiteral("接收确认超时，无法确认对方是否已保存文件")
                : QStringLiteral("文件发送等待超时，已取消");
            clear(); reply(message(transaction, "canceled")); emit q->status(text);
        });
    }
    FileTransferHost *q;
    QString root, id, target, staging;
    Mode mode = Mode::Idle;
    bool offerEnabled = false, outgoingOffer = false;
    QTimer offerTimer;
    qint64 size = 0, offset = 0;
    std::unique_ptr<QSaveFile> output;
    std::unique_ptr<QFile> input;
    std::unique_ptr<RootDirectory> pinnedRoot;

    void clear() {
        const bool wasBusy = mode != Mode::Idle;
        offerTimer.stop(); outgoingOffer = false;
        if (output) output->cancelWriting();
        output.reset(); input.reset();
        if (!staging.isEmpty() && pinnedRoot) pinnedRoot->remove(staging);
        pinnedRoot.reset();
        id.clear(); target.clear(); staging.clear(); mode = Mode::Idle; size = offset = 0;
        if (wasBusy) emit q->busyChanged(false);
    }
    void reply(QJsonObject object) { emit q->send(encode(object)); }
    void fail(const QString &transaction, const QString &error, bool abort = false) {
        if (abort && transaction == id) clear();
        auto response = message(transaction, "error"); response["error"] = error;
        reply(response); emit q->status(error);
    }
    bool directory(bool create, QString &error) {
        if (root.isEmpty()) { error = QStringLiteral("未设置共享文件夹"); return false; }
        // Reject a symlink anywhere in the configured path. Network requests
        // cannot select another directory or create child directories.
        QString path = root;
        for (;;) {
            const QFileInfo info(path);
            if (info.isSymLink() || (info.exists() && !info.isDir())) {
                error = QStringLiteral("共享文件夹路径含符号链接或不是目录"); return false;
            }
            const QString parent = info.dir().absolutePath();
            if (parent == path) break;
            path = parent;
        }
        Q_UNUSED(create); // Creation happens relative to pinned directory handles.
        return true;
    }
    void list(const QString &transaction) {
        QString error;
        if (!directory(false, error)) { fail(transaction, error); return; }
        QJsonArray files;
        bool truncated = false;
        int bytes = 180;
        RootDirectory pinned;
        if (!QFileInfo::exists(root)) {
            auto response = message(transaction, "list"); response["files"] = files; response["truncated"] = false;
            reply(response); return;
        }
        if (!pinned.open(root)) { fail(transaction, QStringLiteral("无法安全打开共享文件夹")); return; }
        QDirIterator iterator(pinned.path(), QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            iterator.next(); const QFileInfo info = iterator.fileInfo();
            if (!info.isFile() || info.isSymLink() || !validName(info.fileName()) || info.size() < 0 || info.size() > MaxTransferFileSize) continue;
            QJsonObject entry{{"name", info.fileName()}, {"size", double(info.size())}};
            const int entryBytes = encode(entry).size() + 1;
            if (files.size() >= 1000 || bytes + entryBytes > MaxFilePayload - 1) { truncated = true; break; }
            bytes += entryBytes; files.append(entry);
        }
        auto response = message(transaction, "list"); response["files"] = files; response["truncated"] = truncated;
        reply(response);
    }
    void begin(const QJsonObject &request, bool upload) {
        const QString transaction = request.value("id").toString(), name = request.value("name").toString();
        if (!validName(name)) { fail(transaction, QStringLiteral("文件名无效：不支持路径、保留名、尾点或尾空格")); return; }
        qint64 requestedSize = 0;
        if (upload && !number(request.value("size"), requestedSize)) {
            fail(transaction, QStringLiteral("文件大小无效，单个文件最多 8 GiB")); return;
        }
        QString error;
        if (!directory(upload, error)) { fail(transaction, error); return; }
        auto pinned = std::make_unique<RootDirectory>();
        if (!pinned->open(root, upload)) { fail(transaction, QStringLiteral("无法安全打开或创建共享文件夹")); return; }
        const QString destination = QDir(pinned->path()).filePath(name);
        const QFileInfo existing(destination);
        if (upload) {
            if (existing.exists() || existing.isSymLink()) { fail(transaction, QStringLiteral("远端已存在同名文件，请先在本地重命名再上传")); return; }
            const QString temporary = StagingPrefix + newId() + QStringLiteral(".part");
            auto file = std::make_unique<QSaveFile>(QDir(pinned->path()).filePath(temporary));
            file->setDirectWriteFallback(false);
            if (!file->open(QIODevice::WriteOnly) || !file->setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
                fail(transaction, QStringLiteral("无法创建上传临时文件")); return;
            }
            mode = Mode::Upload; output = std::move(file); staging = temporary; target = name; size = requestedSize;
        } else {
            auto file = std::make_unique<QFile>();
            if (!existing.isFile() || existing.isSymLink() || !openRegular(*file, *pinned, name)) {
                fail(transaction, QStringLiteral("远端文件不存在、不可读或不是普通文件")); return;
            }
            if (file->size() < 0 || file->size() > MaxTransferFileSize) {
                fail(transaction, QStringLiteral("单个文件最多 8 GiB")); return;
            }
            size = file->size(); input = std::move(file); mode = Mode::Download;
        }
        pinnedRoot = std::move(pinned); id = transaction; offset = 0;
        auto response = message(id, "ready"); response["size"] = double(size); response["offset"] = 0;
        emit q->busyChanged(true);
        if (id == transaction) reply(response);
    }
};

FileTransferHost::FileTransferHost(QObject *parent) : QObject(parent), impl_(new Impl(this)) {}
FileTransferHost::~FileTransferHost() { impl_->clear(); }
void FileTransferHost::configure(const QString &root) {
    reset(); impl_->root = root.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(root).absoluteFilePath());
}
void FileTransferHost::reset() {
    const bool wasAvailable = impl_->offerEnabled;
    impl_->offerEnabled = false; impl_->clear();
    if (wasAvailable) emit offerAvailableChanged(false);
}
bool FileTransferHost::busy() const { return impl_->mode != Mode::Idle; }
bool FileTransferHost::offering() const { return impl_->outgoingOffer; }
bool FileTransferHost::offerAvailable() const { return impl_->offerEnabled; }
void FileTransferHost::setOfferAvailable(bool available) {
    if (impl_->offerEnabled == available) return;
    impl_->offerEnabled = available;
    if (!available) cancelOffer();
    emit offerAvailableChanged(available);
}
void FileTransferHost::offerFile(const QString &localPath) {
    auto &s = *impl_;
    if (!s.offerEnabled) { emit status(QStringLiteral("当前连接不支持主动发送文件")); return; }
    if (busy()) { emit status(QStringLiteral("请等待当前文件传输完成，或先取消")); return; }
    const QFileInfo selected(localPath);
    if (!selected.isFile() || selected.isSymLink() || !validName(selected.fileName())) {
        emit status(QStringLiteral("请选择普通文件；不支持符号链接或无效文件名")); return;
    }
    // This local choice grants access to one already-opened file, never a new
    // network-visible root. No peer-supplied path participates in opening it.
    auto parent = std::make_unique<RootDirectory>();
    auto file = std::make_unique<QFile>();
    const QString parentPath = QFileInfo(selected.absolutePath()).canonicalFilePath();
    if (parentPath.isEmpty() || !parent->open(parentPath) || !openRegular(*file, *parent, selected.fileName()) ||
        file->size() < 0 || file->size() > MaxTransferFileSize) {
        emit status(QStringLiteral("无法安全读取所选普通文件，或文件超过 8 GiB")); return;
    }
    s.id = newId(); s.target = selected.fileName(); s.size = file->size(); s.offset = 0;
    s.input = std::move(file); s.pinnedRoot = std::move(parent);
    s.mode = Mode::Offer; s.outgoingOffer = true; s.offerTimer.start();
    const QString transaction = s.id;
    auto offer = message(transaction, "offer"); offer["name"] = s.target; offer["size"] = double(s.size);
    emit busyChanged(true);
    if (s.id != transaction) return;
    emit progress(0, s.size);
    if (s.id != transaction) return;
    emit status(QStringLiteral("等待对方接收 %1（60 秒内确认）").arg(s.target));
    if (s.id == transaction) s.reply(offer);
}
void FileTransferHost::cancelOffer() {
    auto &s = *impl_;
    if (!s.outgoingOffer) return;
    const QString transaction = s.id;
    s.clear(); s.reply(message(transaction, "canceled"));
    emit status(QStringLiteral("文件发送已取消"));
}
void FileTransferHost::receive(const QByteArray &payload) {
    auto &s = *impl_; QJsonObject request;
    if (!parse(payload, request)) { s.fail({}, QStringLiteral("文件传输消息无效或超过 64 KiB")); return; }
    const QString id = request.value("id").toString(), op = request.value("op").toString();
    if (op == "cancel") {
        if (id == s.id) {
            const bool offered = s.outgoingOffer; s.clear();
            if (offered) emit status(QStringLiteral("对方已拒绝或取消接收文件"));
        }
        s.reply(message(id, "canceled")); return;
    }
    if (op == "list" || op == "put" || op == "get") {
        if (s.mode != Mode::Idle) { s.fail(id, QStringLiteral("另一个文件正在传输，请等待或取消")); return; }
        if (op == "list") s.list(id);
        else s.begin(request, op == "put");
        return;
    }
    if (s.mode == Mode::Idle || id != s.id) { s.fail(id, QStringLiteral("文件传输事务已结束或标识不匹配")); return; }
    if (s.outgoingOffer && (op == "error" || op == "canceled")) {
        s.clear(); emit status(QStringLiteral("对方无法接收或已取消文件")); return;
    }
    if (s.outgoingOffer && s.mode == Mode::Offer && op == "accept" && s.offerEnabled) {
        s.mode = Mode::Download; s.offerTimer.start();
        auto response = message(id, "ready"); response["size"] = double(s.size); response["offset"] = 0;
        emit status(QStringLiteral("对方已接受，正在发送 %1").arg(s.target));
        if (s.id == id) s.reply(response);
        return;
    }
    if (s.outgoingOffer && s.mode == Mode::AwaitReceipt && op == "received") {
        qint64 size = 0;
        if (!number(request.value("size"), size) || size != s.size) {
            s.fail(id, QStringLiteral("接收完成大小不匹配"), true); return;
        }
        s.clear(); emit status(QStringLiteral("对方已接收并保存文件")); return;
    }
    qint64 offset = 0;
    if (!number(request.value("offset"), offset) || offset != s.offset) {
        s.fail(id, QStringLiteral("文件块偏移不匹配，传输已取消"), true); return;
    }
    if (op == "data" && s.mode == Mode::Upload) {
        QByteArray data;
        if (!decodeChunk(request.value("data"), data, false) || data.size() > s.size - s.offset) {
            s.fail(id, QStringLiteral("文件块无效或超过声明大小"), true); return;
        }
        if (s.output->write(data) != data.size()) { s.fail(id, QStringLiteral("上传写入失败，临时文件已清理"), true); return; }
        s.offset += data.size();
        auto response = message(id, "ack"); response["offset"] = double(s.offset); s.reply(response);
    } else if (op == "commit" && s.mode == Mode::Upload && s.offset == s.size) {
        if (!s.pinnedRoot->unchanged(s.root)) {
            s.fail(id, QStringLiteral("共享文件夹已移动或替换，上传已取消"), true); return;
        }
        if (!s.output->commit()) { s.fail(id, QStringLiteral("上传提交失败，临时文件已清理"), true); return; }
        s.output.reset();
        if (!s.pinnedRoot->publish(s.staging, s.target)) {
            s.fail(id, QStringLiteral("远端文件已存在或无法发布，已有文件保持不变"), true); return;
        }
        auto response = message(id, "done"); response["size"] = double(s.size);
        s.clear(); s.reply(response); emit status(QStringLiteral("上传完成"));
    } else if (op == "read" && s.mode == Mode::Download) {
        if (s.input->size() != s.size) { s.fail(id, QStringLiteral("远端文件在下载过程中发生变化"), true); return; }
        const qint64 requested = std::min(qint64(FileChunkSize), s.size - s.offset);
        const QByteArray data = s.input->read(requested);
        if (data.size() != requested) { s.fail(id, QStringLiteral("远端文件读取失败"), true); return; }
        auto response = message(id, "data"); response["offset"] = double(s.offset);
        response["data"] = QString::fromLatin1(data.toBase64());
        s.offset += data.size(); const bool eof = s.offset == s.size; response["eof"] = eof;
        const bool offered = s.outgoingOffer;
        const qint64 sent = s.offset, total = s.size;
        if (offered) {
            s.offerTimer.start();
            if (eof) { s.mode = Mode::AwaitReceipt; s.input.reset(); s.pinnedRoot.reset(); }
            emit progress(sent, total);
            if (s.id != id) return;
            if (eof) {
                emit status(QStringLiteral("文件已发出，等待对方保存确认"));
                if (s.id != id) return;
            }
        } else if (eof) s.clear();
        s.reply(response);
    } else s.fail(id, QStringLiteral("文件传输状态或操作无效"), true);
}

struct FileTransferClient::Impl {
    explicit Impl(FileTransferClient *owner) : q(owner), offerTimer(owner) {
        offerTimer.setObjectName(QStringLiteral("fileOfferTimeout"));
        offerTimer.setSingleShot(true); offerTimer.setInterval(OfferTimeoutMs);
        QObject::connect(&offerTimer, &QTimer::timeout, q, [this] {
            if (incomingOffer) fail(QStringLiteral("文件接收等待超时，已取消"));
        });
    }
    FileTransferClient *q;
    bool available = false, offerEnabled = false, incomingOffer = false;
    QTimer offerTimer;
    Mode mode = Mode::Idle;
    QString id, target;
    qint64 size = 0, offset = 0, expectedAck = 0;
    bool ready = false, finishing = false;
    std::unique_ptr<QFile> input;
    std::unique_ptr<QSaveFile> output;

    void clear() {
        const bool wasBusy = mode != Mode::Idle;
        offerTimer.stop(); incomingOffer = false;
        if (output) output->cancelWriting();
        output.reset(); input.reset(); id.clear(); target.clear(); mode = Mode::Idle;
        size = offset = expectedAck = 0; ready = finishing = false;
        if (wasBusy) emit q->busyChanged(false);
    }
    void send(QJsonObject object) { emit q->send(encode(object)); }
    bool canBegin() {
        if (!available) { emit q->status(QStringLiteral("当前连接未允许文件传输")); return false; }
        if (mode != Mode::Idle) { emit q->status(QStringLiteral("请等待当前传输完成，或先取消")); return false; }
        return true;
    }
    QString begin(Mode next) {
        id = newId(); mode = next; offset = expectedAck = 0; ready = finishing = false;
        const QString transaction = id;
        emit q->busyChanged(true);
        return transaction;
    }
    void fail(const QString &error, bool notifyHost = true) {
        const QString transaction = id;
        clear();
        if (notifyHost && available && !transaction.isEmpty()) send(message(transaction, "cancel"));
        emit q->status(error);
    }
    void next() {
        // Yield between chunks: with 32 KiB this caps request rate near 500/s,
        // leaving headroom for desktop ACKs and heartbeats in the same session.
        const QString transaction = id;
        QTimer::singleShot(2, q, [this, transaction] {
            if (!available || transaction != id || !ready) return;
            if (mode == Mode::Upload) {
                if (offset == size) {
                    finishing = true;
                    auto request = message(id, "commit"); request["offset"] = double(offset); send(request); return;
                }
                if (input->size() != size) { fail(QStringLiteral("本地文件在上传过程中发生变化")); return; }
                const qint64 requested = std::min(qint64(FileChunkSize), size - offset);
                const QByteArray data = input->read(requested);
                if (data.size() != requested) { fail(QStringLiteral("本地文件读取失败")); return; }
                auto request = message(id, "data"); request["offset"] = double(offset);
                request["data"] = QString::fromLatin1(data.toBase64());
                expectedAck = offset + data.size(); send(request);
            } else if (mode == Mode::Download) {
                auto request = message(id, "read"); request["offset"] = double(offset); send(request);
            }
        });
    }
};

FileTransferClient::FileTransferClient(QObject *parent) : QObject(parent), impl_(new Impl(this)) {}
FileTransferClient::~FileTransferClient() { impl_->clear(); }
bool FileTransferClient::available() const { return impl_->available; }
bool FileTransferClient::busy() const { return impl_->mode != Mode::Idle; }
bool FileTransferClient::offerAvailable() const { return impl_->available && impl_->offerEnabled; }
void FileTransferClient::setOfferAvailable(bool available) {
    impl_->offerEnabled = available;
    if (!available && impl_->incomingOffer) cancel();
}
void FileTransferClient::setAvailable(bool available) {
    if (impl_->available == available) return;
    impl_->available = available;
    if (!available) { impl_->offerEnabled = false; impl_->clear(); }
    emit availableChanged(available);
    if (!available) emit listing({});
}
void FileTransferClient::reset() {
    const bool wasAvailable = impl_->available;
    impl_->available = impl_->offerEnabled = false; impl_->clear();
    if (wasAvailable) emit availableChanged(false);
    emit listing({});
}
void FileTransferClient::acceptOffer(const QString &id, const QString &localPath) {
    auto &s = *impl_;
    if (!offerAvailable() || !s.incomingOffer || s.mode != Mode::Offer || s.id != id) return;
    const QFileInfo target(localPath);
    if (localPath.isEmpty() || target.isSymLink() || (target.exists() && !target.isFile())) {
        s.fail(QStringLiteral("保存目标不能是空路径、符号链接或目录")); return;
    }
    // Only this local acceptance selects a destination. The peer sends a plain
    // display name, never a path that can select where bytes will be written.
    s.target = localPath; s.mode = Mode::Download; s.offerTimer.start();
    emit status(QStringLiteral("已同意接收，正在准备文件"));
    if (s.id == id) s.send(message(id, "accept"));
}
void FileTransferClient::declineOffer(const QString &id) {
    if (impl_->incomingOffer && impl_->id == id && impl_->mode == Mode::Offer) cancel();
}
void FileTransferClient::refresh() {
    auto &s = *impl_;
    if (!s.canBegin()) return;
    const QString id = s.begin(Mode::List);
    if (s.id == id) s.send(message(id, "list"));
}
void FileTransferClient::upload(const QString &localPath) {
    auto &s = *impl_;
    if (!s.canBegin()) return;
    const QFileInfo info(localPath);
    if (!info.isFile() || !validName(info.fileName())) {
        emit status(QStringLiteral("请选择普通文件；文件名不能包含保留名称或路径字符")); return;
    }
    auto file = std::make_unique<QFile>(localPath);
    if (!file->open(QIODevice::ReadOnly) || file->size() < 0 || file->size() > MaxTransferFileSize) {
        emit status(QStringLiteral("文件不可读或超过单文件 8 GiB 上限")); return;
    }
    s.size = file->size(); s.input = std::move(file); const QString id = s.begin(Mode::Upload);
    if (s.id != id) return;
    auto request = message(id, "put"); request["name"] = info.fileName(); request["size"] = double(s.size);
    emit progress(0, s.size);
    if (s.id != id) return;
    emit status(QStringLiteral("正在上传 %1").arg(info.fileName()));
    if (s.id == id) s.send(request);
}
void FileTransferClient::download(const QString &remoteName, const QString &localPath) {
    auto &s = *impl_;
    if (!s.canBegin()) return;
    if (!validName(remoteName) || localPath.isEmpty()) { emit status(QStringLiteral("文件名或保存路径无效")); return; }
    const QFileInfo target(localPath);
    if (target.isSymLink() || (target.exists() && !target.isFile())) {
        emit status(QStringLiteral("保存目标不能是符号链接或目录")); return;
    }
    s.target = localPath; const QString id = s.begin(Mode::Download);
    if (s.id != id) return;
    auto request = message(id, "get"); request["name"] = remoteName;
    emit status(QStringLiteral("正在下载 %1").arg(remoteName));
    if (s.id == id) s.send(request);
}
void FileTransferClient::cancel() {
    auto &s = *impl_;
    if (s.mode == Mode::Idle) return;
    const QString transaction = s.id; s.clear();
    if (s.available) s.send(message(transaction, "cancel"));
    emit status(QStringLiteral("传输已取消，未完成文件已清理"));
}
void FileTransferClient::receive(const QByteArray &payload) {
    auto &s = *impl_;
    QJsonObject response;
    if (!parse(payload, response)) {
        if (s.available && s.mode != Mode::Idle) s.fail(QStringLiteral("文件传输响应无效或过大"));
        return;
    }
    const QString incomingId = response.value("id").toString();
    const QString op = response.value("op").toString();
    if (op == "offer") {
        qint64 size = 0;
        const QString name = response.value("name").toString();
        QString error;
        if (!offerAvailable()) error = QStringLiteral("当前连接未允许主动发送文件");
        else if (!validName(name) || !number(response.value("size"), size)) error = QStringLiteral("发送邀请的文件名或大小无效");
        else if (s.mode != Mode::Idle) error = QStringLiteral("另一个文件正在传输或等待确认");
        if (!error.isEmpty()) {
            auto rejection = message(incomingId, "error"); rejection["error"] = error;
            // A new offer must never abort an existing transaction, even when
            // its sender reuses the active id. Invalid offers have no file I/O.
            s.send(rejection); return;
        }
        s.id = incomingId; s.size = size; s.offset = 0; s.mode = Mode::Offer;
        s.incomingOffer = true; s.offerTimer.start();
        emit busyChanged(true);
        if (s.id != incomingId) return;
        emit status(QStringLiteral("对方请求发送 %1，请选择是否接收").arg(name));
        if (s.id == incomingId) emit offered(incomingId, name, size);
        return;
    }
    if (!s.available || s.mode == Mode::Idle) return;
    if (response.value("id").toString() != s.id) return;
    const QString transaction = s.id;
    if (op == "error") { s.fail(response.value("error").toString(QStringLiteral("远端文件传输失败")), false); return; }
    if (op == "canceled" || (op == "cancel" && s.incomingOffer)) { s.fail(QStringLiteral("远端已取消文件传输"), false); return; }
    if (s.mode == Mode::Offer) { s.fail(QStringLiteral("尚未同意接收文件，传输已拒绝")); return; }
    if (s.mode == Mode::List && op == "list" && response.value("files").isArray()) {
        const QJsonArray files = response.value("files").toArray();
        if (files.size() > 1000) { s.fail(QStringLiteral("远端文件列表过大")); return; }
        for (const auto &value : files) {
            qint64 size = 0;
            if (!value.isObject() || !validName(value.toObject().value("name").toString()) || !number(value.toObject().value("size"), size)) {
                s.fail(QStringLiteral("远端文件列表包含无效项")); return;
            }
        }
        const bool truncated = response.value("truncated").toBool();
        s.clear(); emit listing(files);
        emit status(truncated ? QStringLiteral("文件列表已截断，请在被控端减少该目录的文件数量") : QStringLiteral("文件列表已更新"));
        return;
    }
    if ((s.mode == Mode::Upload || s.mode == Mode::Download) && !s.ready && op == "ready") {
        qint64 size = 0, offset = 0;
        if (!number(response.value("size"), size) || !number(response.value("offset"), offset) || offset != 0 ||
            ((s.mode == Mode::Upload || s.incomingOffer) && size != s.size)) { s.fail(QStringLiteral("远端文件大小或初始偏移无效")); return; }
        s.size = size;
        if (s.mode == Mode::Download) {
            s.output = std::make_unique<QSaveFile>(s.target); s.output->setDirectWriteFallback(false);
            if (!s.output->open(QIODevice::WriteOnly)) { s.fail(QStringLiteral("无法创建本地下载临时文件")); return; }
        }
        s.ready = true;
        if (s.incomingOffer) s.offerTimer.start();
        emit progress(0, s.size);
        if (s.id == transaction) s.next();
        return;
    }
    if (s.mode == Mode::Upload && s.ready && !s.finishing && op == "ack") {
        qint64 offset = 0;
        if (!number(response.value("offset"), offset) || offset != s.expectedAck || offset <= s.offset || offset > s.size) {
            s.fail(QStringLiteral("上传确认偏移无效")); return;
        }
        s.offset = offset; emit progress(s.offset, s.size);
        if (s.id == transaction) s.next();
        return;
    }
    if (s.mode == Mode::Upload && s.finishing && op == "done") {
        qint64 size = 0;
        if (!number(response.value("size"), size) || size != s.size) { s.fail(QStringLiteral("上传完成大小不匹配")); return; }
        emit progress(s.size, s.size);
        if (s.id != transaction) return;
        s.clear(); emit status(QStringLiteral("上传完成")); refresh(); return;
    }
    if (s.mode == Mode::Download && s.ready && op == "data") {
        qint64 offset = 0; QByteArray data;
        if (!number(response.value("offset"), offset) || offset != s.offset ||
            !response.value("eof").isBool() || !decodeChunk(response.value("data"), data, true) || data.size() > s.size - s.offset ||
            response.value("eof").toBool() != (s.offset + data.size() == s.size) || (data.isEmpty() && s.offset != s.size)) {
            s.fail(QStringLiteral("下载文件块大小、偏移或结束标记无效")); return;
        }
        if (s.output->write(data) != data.size()) { s.fail(QStringLiteral("下载写入失败，原文件保持不变")); return; }
        if (s.incomingOffer) s.offerTimer.start();
        s.offset += data.size(); emit progress(s.offset, s.size);
        if (s.id != transaction) return;
        if (s.offset == s.size) {
            if (!s.output->commit()) { s.fail(QStringLiteral("下载提交失败，原文件保持不变")); return; }
            const bool offered = s.incomingOffer;
            auto receipt = message(transaction, "received"); receipt["size"] = double(s.size);
            s.output.reset();
            // The receipt follows a successful atomic commit, never just EOF.
            // Queue it before busy=false can start the next transaction.
            if (offered) s.send(receipt);
            if (s.id != transaction) return;
            s.clear(); emit status(offered ? QStringLiteral("文件已接收并保存") : QStringLiteral("下载完成"));
        } else s.next();
        return;
    }
    s.fail(QStringLiteral("文件传输响应状态不匹配"));
}
}
