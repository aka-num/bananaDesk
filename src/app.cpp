#include "app.h"
#include "video_codec.h"
#include <QApplication>
#include <QScreen>
#include <QBuffer>
#include <QImageReader>
#include <QNetworkInterface>
#include <QSslConfiguration>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QClipboard>
#include <QSaveFile>
#include <QMessageBox>
#include <QStatusBar>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>

namespace ld {
static QSslConfiguration tlsConfig() {
    auto config = QSslConfiguration::defaultConfiguration();
    config.setProtocol(QSsl::TlsV1_2OrLater);
    return config;
}
Host::Host(QObject *parent) : QObject(parent), server_(this) {
    connect(&server_, &Listener::accepted, this, &Host::accept);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(2);
    connect(&timer_, &QTimer::timeout, this, &Host::tick);
    worker_ = new CaptureWorker;
    worker_->moveToThread(&videoThread_);
    connect(&videoThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &Host::produceFrame, worker_, &CaptureWorker::produce);
    encoder_ = new EncodeWorker;
    encoder_->moveToThread(&encodeThread_);
    connect(&encodeThread_, &QThread::finished, encoder_, &QObject::deleteLater);
    connect(this, &Host::encodeFrame, encoder_, &EncodeWorker::encode);
    connect(encoder_, &EncodeWorker::produced, this, &Host::encoded);
    connect(worker_, &CaptureWorker::captured, this, [this](quint64 generation, const QImage &image, QRect bounds, double captureMs, const QString &backend) {
        if (generation != generation_ || !authenticated_) return;
        captureBusy_ = false; ++encodeQueued_;
        emit encodeFrame(generation, image, bounds, codec_, targetFps_, captureMs, backend);
        tick();
    });
    connect(worker_, &CaptureWorker::failed, this, [this](quint64 generation, const QString &error) {
        if (generation == generation_) drop(error);
    });
    connect(encoder_, &EncodeWorker::failed, this, [this](quint64 generation, const QString &error) {
        if (generation == generation_) drop(error);
    });
    videoThread_.start();
    encodeThread_.start();
}
Host::~Host() { stop(); videoThread_.quit(); videoThread_.wait(); encodeThread_.quit(); encodeThread_.wait(); }
bool Host::start(const QHostAddress &bind, quint16 port, bool control, QString &error) {
    stop();
#ifndef Q_OS_WIN
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" || QGuiApplication::platformName() != "xcb") {
        error = QStringLiteral("本版被控端需要 X11 会话，尚未实现 Wayland 桌面控制"); return false;
    }
#endif
    if (!QSslSocket::supportsSsl()) { error = QStringLiteral("Qt TLS 后端不可用"); return false; }
    if (!QGuiApplication::primaryScreen()) { error = QStringLiteral("没有可用桌面"); return false; }
    if (control && !input_.available()) { error = input_.error(); return false; }
    if (codecPreference_ == "h264" && !VideoEncoder::available()) { error = QStringLiteral("此构建没有可用的 H.264 编码器"); return false; }
    if (bind.protocol() != QAbstractSocket::IPv4Protocol || bind == QHostAddress::AnyIPv4 || bind.isMulticast()) {
        error = QStringLiteral("请选择具体的本机 IPv4 地址"); return false;
    }
    if (!identity_.create(error)) return false;
    if (!server_.listen(bind, port)) { error = server_.errorString(); return false; }
    address_ = bind; control_ = control;
    timer_.start(); emit status(QStringLiteral("正在共享 %1:%2 · 等待连接").arg(bind.toString()).arg(server_.serverPort()));
    return true;
}
QString Host::invitation() const {
    return Invitation{address_.toString(), server_.serverPort(), identity_.fingerprint, identity_.token}.encode();
}
void Host::stop() {
    timer_.stop(); server_.close(); drop(QString()); identity_ = Identity();
}
void Host::drop(const QString &reason) {
    ++generation_;
    worker_->activate(generation_); encoder_->activate(generation_);
    input_.releaseAll(); authenticated_ = false; captureBusy_ = false; inFlight_ = 0; encodeQueued_ = 0; screen_ = {};
    if (socket_) {
        auto *old = socket_; socket_ = nullptr; wire_ = nullptr;
        old->disconnect(this); old->abort(); old->deleteLater();
    }
    if (!reason.isEmpty()) emit status(reason);
}
void Host::accept(qintptr descriptor) {
    auto *socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
    if (socket_) { socket->abort(); socket->deleteLater(); return; }
    socket_ = socket; authenticated_ = false; inFlight_ = 0;
    socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket_->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 256 * 1024);
    auto config = tlsConfig(); config.setLocalCertificate(identity_.certificate); config.setPrivateKey(identity_.key);
    config.setPeerVerifyMode(QSslSocket::VerifyNone); socket_->setSslConfiguration(config);
    wire_ = new Wire(socket_, 4096, socket_);
    connect(wire_, &Wire::packet, this, &Host::receive);
    connect(wire_, &Wire::failure, this, [this](const QString &s) { drop(s); });
    connect(socket_, &QSslSocket::disconnected, this, [this] { drop(QStringLiteral("对方已断开 · 等待连接")); });
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QSslSocket::errorOccurred), this, [this](QAbstractSocket::SocketError) {
        if (socket_) drop(QStringLiteral("连接结束：") + socket_->errorString());
    });
    lastMessage_.start(); packetWindow_.start(); packets_ = 0;
    socket_->startServerEncryption();
}
void Host::receive(Packet type, const QByteArray &payload) {
    if (!socket_ || !socket_->isEncrypted()) { drop(QStringLiteral("TLS 未就绪")); return; }
    if (packetWindow_.elapsed() >= 1000) { packetWindow_.restart(); packets_ = 0; }
    if (++packets_ > 1000) { drop(QStringLiteral("输入消息速率超限")); return; }
    if (!authenticated_) {
        QJsonObject o;
        if (type != Packet::Auth || !object(payload, o) || o.value("v").toInt() != 1 ||
            !equalSecret(o.value("token").toString().toLatin1(), identity_.token.toLatin1())) {
            drop(QStringLiteral("连接验证失败 · 等待连接")); return;
        }
        const auto codecs = o.value("codecs").toArray();
        codec_ = codecPreference_ != "jpeg" && codecs.contains("h264") && VideoEncoder::available() ? "h264" : "jpeg";
        if (codecPreference_ == "h264" && codec_ != "h264") { drop(QStringLiteral("对端未提供 H.264 支持")); return; }
        frameWindow_ = qBound(1, o.value("window").toInt(1), 3);
        authenticated_ = true; lastMessage_.restart(); frameClock_.start(); nextFrameNs_ = 0;
        emit status(QStringLiteral("已连接：%1 · %2").arg(socket_->peerAddress().toString(), control_ ? QStringLiteral("允许键鼠控制") : QStringLiteral("仅观看")));
        capture(); return;
    }
    lastMessage_.restart();
    if (type == Packet::Ping && payload.isEmpty()) wire_->send(Packet::Pong);
    else if (type == Packet::Ack && payload.isEmpty() && inFlight_ > 0) { --inFlight_; tick(); }
    else if (type == Packet::Release && payload.isEmpty()) input_.releaseAll();
    else if (type == Packet::Input && control_) {
        QJsonObject o;
        if (!object(payload, o)) { drop(QStringLiteral("输入消息格式错误")); return; }
        if (!input_.apply(o, screen_)) emit status(QStringLiteral("部分输入无法执行：检查键盘映射或目标窗口权限"));
    } else if (type == Packet::Input && !control_) { drop(QStringLiteral("此会话只允许观看")); }
    else drop(QStringLiteral("未知或无效的会话消息"));
}
void Host::tick() {
    if (!socket_) return;
    if (lastMessage_.elapsed() > 8000) { drop(QStringLiteral("连接超时 · 已释放远程按键")); return; }
    if (authenticated_ && !captureBusy_ && inFlight_ + encodeQueued_ < frameWindow_ && encodeQueued_ < 2 && socket_->bytesToWrite() < 512 * 1024 && frameClock_.nsecsElapsed() >= nextFrameNs_) capture();
}
void Host::capture() {
    captureBusy_ = true;
    const qint64 period = 1000000000LL / targetFps_;
    nextFrameNs_ += period;
    if (nextFrameNs_ < frameClock_.nsecsElapsed() - period) nextFrameNs_ = frameClock_.nsecsElapsed() + period;
    emit produceFrame(generation_);
}
void Host::encoded(quint64 generation, QRect bounds, QByteArray bytes, QString codec,
                   double captureMs, double encodeMs, QString backend) {
    if (generation != generation_ || !authenticated_ || !socket_) return;
    --encodeQueued_;
    if (bounds != screen_) {
        input_.releaseAll(); screen_ = bounds;
        wire_->send(Packet::Welcome, json({{"v", 1}, {"width", screen_.width()}, {"height", screen_.height()}, {"control", control_}, {"codec", codec}, {"fps", targetFps_}, {"window", frameWindow_}}));
        qInfo().noquote() << QStringLiteral("视频：%1 · %2 · 目标 %3 fps · 首帧采集 %4 ms / 编码 %5 ms").arg(backend, codec).arg(targetFps_).arg(captureMs, 0, 'f', 1).arg(encodeMs, 0, 'f', 1);
    }
    if (!wire_->send(Packet::Image, bytes)) { drop(QStringLiteral("发送画面失败")); return; }
    ++inFlight_;
    tick();
}

Client::Client(QObject *parent) : QObject(parent) {
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (!socket_) return;
        if (lastMessage_.elapsed() > 10000) { fail(QStringLiteral("连接超时")); return; }
        if (ready_) wire_->send(Packet::Ping);
    });
    decoder_ = new DecodeWorker;
    decoder_->moveToThread(&decodeThread_);
    connect(&decodeThread_, &QThread::finished, decoder_, &QObject::deleteLater);
    connect(this, &Client::decodeFrame, decoder_, &DecodeWorker::decode);
    connect(decoder_, &DecodeWorker::failed, this, [this](quint64 generation, const QString &error) {
        if (generation == generation_) fail(error);
    });
    connect(decoder_, &DecodeWorker::decoded, this, [this](quint64 generation, const QImage &image, quint64 fingerprint, double decodeMs) {
        if (generation != generation_ || !ready_) return;
        --queuedDecode_; ++decodedFrames_; decodeTotalMs_ += decodeMs;
        if (decodedFrames_ == 1 || fingerprint != lastFingerprint_) ++distinctFrames_;
        lastFingerprint_ = fingerprint;
        emit frame(image);
        if (wire_) wire_->send(Packet::Ack);
    });
    statsTimer_.setInterval(1000);
    connect(&statsTimer_, &QTimer::timeout, this, &Client::reportStats);
    decodeThread_.start();
}
Client::~Client() { stop(); decodeThread_.quit(); decodeThread_.wait(); }
void Client::stop() {
    if (ready_) reportStats();
    release(); ready_ = false; timer_.stop();
    statsTimer_.stop(); ++generation_; queuedDecode_ = 0;
    decoder_->activate(generation_);
    if (socket_) { auto *old = socket_; socket_ = nullptr; wire_ = nullptr; old->disconnect(this); old->abort(); old->deleteLater(); }
    emit disconnected();
}
void Client::fail(const QString &message) { stop(); emit status(message); }
void Client::start(const Invitation &invitation) {
    stop(); invitation_ = invitation;
    receivedFrames_ = decodedFrames_ = paintedFrames_ = distinctFrames_ = receivedBytes_ = lastFingerprint_ = 0;
    intervalDecoded_ = intervalPainted_ = intervalBytes_ = 0; decodeTotalMs_ = 0; codec_.clear();
    socket_ = new QSslSocket(this);
    socket_->setSslConfiguration(tlsConfig());
    socket_->setPeerVerifyMode(QSslSocket::VerifyPeer);
    wire_ = new Wire(socket_, MaxPacket, socket_);
    connect(wire_, &Wire::packet, this, &Client::receive);
    connect(wire_, &Wire::failure, this, [this](const QString &s) { fail(s); });
    connect(socket_, QOverload<const QList<QSslError> &>::of(&QSslSocket::sslErrors), this, [this](const QList<QSslError> &errors) {
        if (!socket_ || socket_->peerCertificate().digest(QCryptographicHash::Sha256).toHex() != invitation_.fingerprint.toLatin1()) {
            fail(QStringLiteral("证书指纹不匹配，连接已拒绝")); return;
        }
        for (const auto &error : errors) {
            if (error.error() != QSslError::SelfSignedCertificate && error.error() != QSslError::HostNameMismatch) {
                fail(QStringLiteral("TLS 证书错误：") + error.errorString()); return;
            }
        }
        socket_->ignoreSslErrors(errors); // Only the explicitly pinned certificate, with these expected errors.
    });
    connect(socket_, &QSslSocket::encrypted, this, [this] {
        if (!socket_ || socket_->peerCertificate().digest(QCryptographicHash::Sha256).toHex() != invitation_.fingerprint.toLatin1()) {
            fail(QStringLiteral("证书指纹不匹配，连接已拒绝")); return;
        }
        socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        QJsonArray codecs; codecs.append("jpeg");
        if (VideoDecoder::available()) codecs.append("h264");
        wire_->send(Packet::Auth, json({{"v", 1}, {"token", invitation_.token}, {"codecs", codecs}, {"window", 3}}));
        emit status(QStringLiteral("TLS 身份已核验，正在验证连接码"));
    });
    connect(socket_, &QSslSocket::disconnected, this, [this] { fail(QStringLiteral("连接已结束")); });
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QSslSocket::errorOccurred), this, [this](QAbstractSocket::SocketError) {
        if (socket_) fail(QStringLiteral("连接失败：") + socket_->errorString());
    });
    lastMessage_.start(); timer_.start();
    emit status(QStringLiteral("正在连接 %1:%2").arg(invitation.host).arg(invitation.port));
    socket_->connectToHostEncrypted(invitation.host, invitation.port);
}
void Client::receive(Packet type, const QByteArray &payload) {
    if (!socket_ || !socket_->isEncrypted()) { fail(QStringLiteral("TLS 未就绪")); return; }
    lastMessage_.restart();
    if (type == Packet::Welcome) {
        QJsonObject o;
        if (!object(payload, o) || o.value("v").toInt() != 1 || (o.value("codec").toString() != "jpeg" && o.value("codec").toString() != "h264") ||
            o.value("width").toInt() < 1 || o.value("height").toInt() < 1 ||
            o.value("width").toInt() > 32768 || o.value("height").toInt() > 32768 || !o.value("control").isBool()) {
            fail(QStringLiteral("远端桌面信息无效")); return;
        }
        const QString codec = o.value("codec").toString();
        if ((ready_ && codec != codec_) || (codec == "h264" && !VideoDecoder::available())) { fail(QStringLiteral("不支持远端视频编码")); return; }
        codec_ = codec; targetFps_ = qBound(1, o.value("fps").toInt(10), 60);
        frameWindow_ = qBound(1, o.value("window").toInt(1), 3);
        if (!ready_) { statsClock_.start(); intervalClock_.start(); statsTimer_.start(); }
        ready_ = true; emit capability(o.value("control").toBool());
        emit status(QStringLiteral("已连接 · %1 × %2 · %3").arg(o.value("width").toInt()).arg(o.value("height").toInt()).arg(o.value("control").toBool() ? QStringLiteral("可控制") : QStringLiteral("仅观看")));
    } else if (type == Packet::Image && ready_) {
        if (++queuedDecode_ > frameWindow_) { fail(QStringLiteral("远端视频超出接收窗口")); return; }
        ++receivedFrames_; receivedBytes_ += quint64(payload.size());
        emit decodeFrame(generation_, codec_, payload);
    } else if (type == Packet::Pong && ready_ && payload.isEmpty()) { }
    else { fail(QStringLiteral("远端消息无效")); }
}
void Client::sendInput(const QJsonObject &event) {
    if (ready_ && wire_ && socket_->bytesToWrite() < 64 * 1024) wire_->send(Packet::Input, json(event));
    else if (ready_) fail(QStringLiteral("输入队列拥塞，已断开以释放按键"));
}
void Client::release() { if (ready_ && wire_) wire_->send(Packet::Release); }
void Client::recordPaint() { if (ready_) ++paintedFrames_; }
void Client::reportStats() {
    if (!ready_ || !statsClock_.isValid()) return;
    const double elapsed = statsClock_.nsecsElapsed() / 1e9;
    const double interval = qMax(0.001, intervalClock_.nsecsElapsed() / 1e9);
    const double decodeFps = (decodedFrames_ - intervalDecoded_) / interval;
    const double paintFps = (paintedFrames_ - intervalPainted_) / interval;
    const double mbps = (receivedBytes_ - intervalBytes_) * 8 / interval / 1e6;
    emit statistics(QStringLiteral("%1 · 目标 %2 fps · 解码 %3 fps · 绘制 %4 fps · %5 Mbps")
        .arg(codec_.toUpper()).arg(targetFps_).arg(decodeFps, 0, 'f', 1).arg(paintFps, 0, 'f', 1).arg(mbps, 0, 'f', 1));
    if (!statsFile_.isEmpty()) {
        QSaveFile file(statsFile_);
        if (file.open(QIODevice::WriteOnly)) {
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
            file.write(QJsonDocument(QJsonObject{{"received_frames", double(receivedFrames_)}, {"decoded_frames", double(decodedFrames_)},
                {"painted_frames", double(paintedFrames_)}, {"distinct_decoded_frames", double(distinctFrames_)},
                {"elapsed_seconds", elapsed}, {"target_fps", targetFps_}, {"codec", codec_}, {"bytes_received", double(receivedBytes_)},
                {"mean_decode_ms", decodedFrames_ ? decodeTotalMs_ / decodedFrames_ : 0.0}, {"queued_decode", queuedDecode_}}).toJson());
            file.commit();
        }
    }
    intervalDecoded_ = decodedFrames_; intervalPainted_ = paintedFrames_; intervalBytes_ = receivedBytes_; intervalClock_.restart();
}

Viewer::Viewer(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setMinimumSize(480, 270);
}
void Viewer::setFrame(QImage image) { image_ = std::move(image); ++frameSerial_; update(); }
void Viewer::setControl(bool enabled) { control_ = enabled; if (!enabled) { pressedButtons_.clear(); emit releaseKeys(); } }
void Viewer::reset() { control_ = false; pressedButtons_.clear(); image_ = {}; update(); }
QRectF Viewer::imageRect() const {
    if (image_.isNull()) return {};
    QSizeF size = image_.size(); size.scale(this->size(), Qt::KeepAspectRatio);
    return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height());
}
void Viewer::paintEvent(QPaintEvent *) {
    QPainter p(this); p.fillRect(rect(), QColor("#111827"));
    if (image_.isNull()) { p.setPen(QColor("#a9b4c7")); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("远程桌面\n连接后在此显示")); return; }
    p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(imageRect(), image_);
    if (paintedSerial_ != frameSerial_) { paintedSerial_ = frameSerial_; emit presented(); }
}
QJsonObject Viewer::position(const QPointF &point) const {
    const QRectF rect = imageRect();
    return {{"x", qBound(0.0, (point.x() - rect.left()) / qMax(1.0, rect.width() - 1), 1.0)},
            {"y", qBound(0.0, (point.y() - rect.top()) / qMax(1.0, rect.height() - 1), 1.0)}};
}
static QPointF mousePos(QMouseEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}
void Viewer::mouseMoveEvent(QMouseEvent *e) {
    if (!control_ || image_.isNull() || !hasFocus() || (!imageRect().contains(mousePos(e)) && !e->buttons())) return;
    auto o = position(mousePos(e)); o.insert("kind", "move"); emit input(o);
}
void Viewer::mouseButton(QMouseEvent *e, bool down) {
    if (!control_ || image_.isNull() || (down && !imageRect().contains(mousePos(e)))) return;
    int button = e->button() == Qt::LeftButton ? 1 : e->button() == Qt::MiddleButton ? 2 : e->button() == Qt::RightButton ? 3 : 0;
    if (!button) return;
    if (down) pressedButtons_.insert(button);
    else if (!pressedButtons_.remove(button)) return;
    if (down) setFocus(Qt::MouseFocusReason);
    auto o = position(mousePos(e)); o.insert("kind", "button"); o.insert("button", button); o.insert("down", down); emit input(o); e->accept();
}
void Viewer::mousePressEvent(QMouseEvent *e) { mouseButton(e, true); }
void Viewer::mouseReleaseEvent(QMouseEvent *e) { mouseButton(e, false); }
void Viewer::wheelEvent(QWheelEvent *e) {
    if (!control_ || image_.isNull() || !hasFocus() || !imageRect().contains(e->position())) return;
    int steps = e->angleDelta().y() / 120;
    if (!steps) return;
    auto o = position(e->position()); o.insert("kind", "wheel"); o.insert("steps", qBound(-10, steps, 10)); emit input(o); e->accept();
}
void Viewer::keyPressEvent(QKeyEvent *e) {
    if (!control_ || image_.isNull()) return;
    if (e->key() == Qt::Key_Escape && (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) == (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) {
        emit releaseKeys(); clearFocus(); e->accept(); return;
    }
    emit input({{"kind", "key"}, {"key", e->key()}, {"down", true}}); e->accept();
}
void Viewer::keyReleaseEvent(QKeyEvent *e) {
    if (control_ && !image_.isNull() && !e->isAutoRepeat()) emit input({{"kind", "key"}, {"key", e->key()}, {"down", false}});
    e->accept();
}
void Viewer::focusOutEvent(QFocusEvent *e) { pressedButtons_.clear(); emit releaseKeys(); QWidget::focusOutEvent(e); }

Window::Window() : host_(this), client_(this) {
    setWindowTitle(QStringLiteral("LanDesk · 局域网远程桌面")); resize(1120, 760);
    auto *root = new QWidget; auto *layout = new QVBoxLayout(root); layout->setContentsMargins(22, 18, 22, 12);
    auto *title = new QLabel(QStringLiteral("LanDesk")); title->setStyleSheet("font-size:26px;font-weight:600;color:#16324f");
    auto *subtitle = new QLabel(QStringLiteral("局域网直连 · 加密连接 · Windows / Linux")); subtitle->setStyleSheet("color:#52677d;margin-bottom:10px");
    layout->addWidget(title); layout->addWidget(subtitle);
    tabs_ = new QTabWidget; layout->addWidget(tabs_, 1);
    auto *sharePage = new QWidget; auto *shareLayout = new QVBoxLayout(sharePage); shareLayout->setContentsMargins(20, 20, 20, 20);
    auto *info = new QLabel(QStringLiteral("共享本机的主显示器。持有连接码的人可在共享期间连接；停止共享会立即撤销连接码。")); info->setWordWrap(true); shareLayout->addWidget(info);
    addresses_ = new QComboBox;
    QStringList addresses;
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || (iface.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const auto &entry : iface.addressEntries()) if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) addresses << entry.ip().toString();
    }
    addresses.removeDuplicates();
    std::stable_sort(addresses.begin(), addresses.end(), [](const QString &a, const QString &b) {
        auto privateAddress = [](const QString &s) { QHostAddress h(s); return h.isInSubnet(QHostAddress("10.0.0.0"), 8) || h.isInSubnet(QHostAddress("172.16.0.0"), 12) || h.isInSubnet(QHostAddress("192.168.0.0"), 16); };
        return privateAddress(a) && !privateAddress(b);
    });
    addresses << "127.0.0.1"; addresses_->addItems(addresses);
    port_ = new QSpinBox; port_->setRange(1024, 65535); port_->setValue(DefaultPort);
    auto *form = new QFormLayout; form->addRow(QStringLiteral("监听地址"), addresses_); form->addRow(QStringLiteral("端口"), port_); shareLayout->addLayout(form);
    fps_ = new QComboBox;
    for (int fps : {60, 30, 15}) fps_->addItem(QStringLiteral("%1 fps").arg(fps), fps);
    codec_ = new QComboBox;
    codec_->addItem(QStringLiteral("自动（优先 H.264）"), "auto");
    if (VideoEncoder::available()) codec_->addItem(QStringLiteral("H.264"), "h264");
    codec_->addItem(QStringLiteral("JPEG（兼容模式）"), "jpeg");
    form->addRow(QStringLiteral("目标帧率"), fps_); form->addRow(QStringLiteral("视频编码"), codec_);
    allowControl_ = new QCheckBox(QStringLiteral("允许对方操作键盘和鼠标")); allowControl_->setChecked(true); shareLayout->addWidget(allowControl_);
    share_ = new QPushButton(QStringLiteral("开始共享")); share_->setMinimumHeight(38); shareLayout->addWidget(share_);
    shareLayout->addWidget(new QLabel(QStringLiteral("连接码（包含访问密钥，请只交给可信设备）")));
    invitation_ = new QPlainTextEdit; invitation_->setReadOnly(true); invitation_->setMaximumHeight(125); invitation_->setPlaceholderText(QStringLiteral("开始共享后生成连接码")); shareLayout->addWidget(invitation_);
    auto *copy = new QPushButton(QStringLiteral("复制连接码")); shareLayout->addWidget(copy); shareLayout->addStretch();
    tabs_->addTab(sharePage, QStringLiteral("共享本机"));
    auto *viewPage = new QWidget; auto *viewLayout = new QVBoxLayout(viewPage); viewLayout->setContentsMargins(12, 12, 12, 12);
    connectCode_ = new QPlainTextEdit; connectCode_->setPlaceholderText(QStringLiteral("粘贴另一台电脑生成的 landesk1: 连接码")); connectCode_->setMaximumHeight(72); viewLayout->addWidget(connectCode_);
    auto *actions = new QHBoxLayout; auto *connectButton = new QPushButton(QStringLiteral("连接")); auto *disconnectButton = new QPushButton(QStringLiteral("断开")); auto *releaseButton = new QPushButton(QStringLiteral("释放远程按键"));
    actions->addWidget(connectButton); actions->addWidget(disconnectButton); actions->addWidget(releaseButton); actions->addStretch(); viewLayout->addLayout(actions);
    viewer_ = new Viewer; viewLayout->addWidget(viewer_, 1);
    auto *hint = new QLabel(QStringLiteral("点击画面开始操作；Ctrl + Alt + Shift + Esc 释放键盘。系统保留快捷键、中文输入法需另行验证。")); hint->setWordWrap(true); viewLayout->addWidget(hint);
    tabs_->addTab(viewPage, QStringLiteral("连接远程桌面"));
    status_ = new QLabel(QStringLiteral("尚未共享或连接")); status_->setWordWrap(true); status_->setStyleSheet("padding:9px;background:#edf3f8;color:#16324f;border-radius:5px"); layout->addWidget(status_);
    performance_ = new QLabel(QStringLiteral("目标 60 fps · 连接后显示实际解码与绘制帧率"));
    performance_->setStyleSheet("color:#52677d;padding:3px"); layout->addWidget(performance_);
    setCentralWidget(root);
    connect(share_, &QPushButton::clicked, this, [this] {
        if (host_.running()) { host_.stop(); invitation_->clear(); share_->setText(QStringLiteral("开始共享")); addresses_->setEnabled(true); port_->setEnabled(true); allowControl_->setEnabled(true); fps_->setEnabled(true); codec_->setEnabled(true); showStatus(QStringLiteral("共享已停止，连接码已撤销")); }
        else startHost(addresses_->currentText(), quint16(port_->value()));
    });
    connect(copy, &QPushButton::clicked, this, [this] { if (!invitation_->toPlainText().isEmpty()) QApplication::clipboard()->setText(invitation_->toPlainText()); });
    connect(connectButton, &QPushButton::clicked, this, &Window::connectCode);
    connect(disconnectButton, &QPushButton::clicked, this, [this] { client_.stop(); showStatus(QStringLiteral("已断开")); });
    connect(releaseButton, &QPushButton::clicked, &client_, &Client::release);
    connect(&host_, &Host::status, this, &Window::showStatus); connect(&client_, &Client::status, this, &Window::showStatus);
    connect(&client_, &Client::frame, viewer_, &Viewer::setFrame); connect(&client_, &Client::capability, viewer_, &Viewer::setControl);
    connect(&client_, &Client::disconnected, viewer_, &Viewer::reset);
    connect(&client_, &Client::disconnected, this, [this] { performance_->setText(QStringLiteral("未连接 · 连接后显示实际帧率")); });
    connect(viewer_, &Viewer::input, &client_, &Client::sendInput); connect(viewer_, &Viewer::releaseKeys, &client_, &Client::release);
    connect(viewer_, &Viewer::presented, &client_, &Client::recordPaint);
    connect(&client_, &Client::statistics, performance_, &QLabel::setText);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) { if (state != Qt::ApplicationActive) client_.release(); });
}
void Window::showStatus(const QString &text) { status_->setText(text); qInfo().noquote() << text; }
void Window::configureVideo(int fps, const QString &codec) {
    fps_->setCurrentIndex(qMax(0, fps_->findData(fps)));
    codec_->setCurrentIndex(qMax(0, codec_->findData(codec)));
}
bool Window::startHost(const QString &bind, quint16 port, const QString &inviteFile) {
    const QString address = bind.isEmpty() ? addresses_->currentText() : bind;
    QString error;
    host_.configureVideo(fps_->currentData().toInt(), codec_->currentData().toString());
    if (!host_.start(QHostAddress(address), port, allowControl_->isChecked(), error)) { showStatus(error); return false; }
    if (!inviteFile.isEmpty()) {
        QSaveFile file(inviteFile);
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(host_.invitation().toUtf8()) < 0 || !file.commit()) { host_.stop(); showStatus(QStringLiteral("无法安全保存连接码文件")); return false; }
    }
    invitation_->setPlainText(host_.invitation()); share_->setText(QStringLiteral("停止共享并撤销连接码"));
    if (addresses_->findText(address) < 0) addresses_->addItem(address);
    addresses_->setCurrentText(address); port_->setValue(port);
    addresses_->setEnabled(false); port_->setEnabled(false); allowControl_->setEnabled(false); fps_->setEnabled(false); codec_->setEnabled(false); return true;
}
void Window::connectCode() {
    Invitation invitation; QString error;
    if (!Invitation::decode(connectCode_->toPlainText(), invitation, error)) { showStatus(error); return; }
    client_.start(invitation);
}
void Window::connectFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2048) { showStatus(QStringLiteral("无法读取连接码文件")); return; }
    connectCode_->setPlainText(QString::fromUtf8(file.readAll())); tabs_->setCurrentIndex(1); connectCode();
}
void Window::closeEvent(QCloseEvent *event) { client_.stop(); host_.stop(); event->accept(); }
}
