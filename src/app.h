#pragma once
#include "protocol.h"
#include "identity_store.h"
#include "device_store.h"
#include "native_input.h"
#include "video_pipeline.h"
#include "file_transfer.h"
#include "clipboard_sync.h"
#include <QMainWindow>
#include <QTcpServer>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QSpinBox>
#include <QTabWidget>
#include <QSet>
#include <QThread>
#include <QVBoxLayout>
#include <QPointer>
#include <memory>

class QFileDialog;
class QProgressBar;

namespace ld {
class FileTransferDialog;
class Listener : public QTcpServer {
    Q_OBJECT
public: using QTcpServer::QTcpServer;
signals: void accepted(qintptr descriptor);
protected: void incomingConnection(qintptr descriptor) override { emit accepted(descriptor); }
};
class Host : public QObject {
    Q_OBJECT
public:
    explicit Host(QObject *parent, const QString &identityDirectory = {});
    ~Host() override;
    bool start(const QHostAddress &bind, quint16 port, bool control, QString &error);
    void stop();
    bool resetSharingCode(QString &error);
    QString invitation() const;
    bool running() const { return server_.isListening(); }
    QList<TrustedDevice> trustedDevices() const { return trustedDevices_; }
    bool reloadTrustedDevices(QString &error);
    bool forgetTrustedDevice(const QString &id, QString &error);
    void configureVideo(int fps, const QString &codec) { targetFps_ = fps; codecPreference_ = codec; }
    void setLockOnDisconnect(bool enabled) { lockOnDisconnect_ = enabled; }
    void configureFiles(const QString &root, bool enabled) { fileRoot_ = root; filesEnabled_ = enabled; }
    FileTransferHost *fileTransfer() { return &files_; }
signals: void status(QString text);
    void trustedDevicesChanged();
    void desktopLockRequested();
    void produceFrame(quint64 generation, bool useHelper);
    void encodeFrame(quint64 generation, QImage image, QRect bounds, QString codec, int fps, double captureMs, QString backend);
private:
    void accept(qintptr descriptor);
    void receive(Packet type, const QByteArray &payload);
    void tick();
    void capture();
    void encoded(quint64 generation, QRect bounds, QByteArray bytes, QString codec,
                 double captureMs, double encodeMs, QString backend);
    void drop(const QString &reason);
    void releaseInput();
    void completeAuthentication(QJsonObject options, const QString &deviceId = {}, const QString &deviceName = {});
    Listener server_;
    Identity identity_;
    std::unique_ptr<IdentityStore> identityStore_;
    std::unique_ptr<TrustedDeviceStore> trustedStore_;
    QList<TrustedDevice> trustedDevices_;
    QString activeDeviceId_;
    QString pairingError_;
    NativeInput input_;
    QHostAddress address_;
    QSslSocket *socket_ = nullptr;
    Wire *wire_ = nullptr;
    QTimer timer_;
    QThread videoThread_;
    QThread encodeThread_;
    CaptureWorker *worker_;
    EncodeWorker *encoder_;
    QElapsedTimer frameClock_;
    qint64 nextFrameNs_ = 0;
    quint64 generation_ = 0;
    int targetFps_ = 60, frameWindow_ = 1, inFlight_ = 0, encodeQueued_ = 0;
    QString codecPreference_ = "auto", codec_ = "jpeg";
    bool captureBusy_ = false;
    QElapsedTimer lastMessage_, packetWindow_;
    int packets_ = 0;
    bool authenticated_ = false, control_ = false;
    bool lockOnDisconnect_ = true;
    bool helperMode_ = false, loginScreen_ = false;
    QElapsedTimer wakeClock_;
    QRect screen_;
    FileTransferHost files_;
    ClipboardSync clipboard_;
    bool clipboardAllowed_ = false;
    QString fileRoot_;
    bool filesEnabled_ = false, filesAllowed_ = false, fileOffersAllowed_ = false;
    bool authChallengePending_ = false;
    QString pendingDeviceId_;
    QJsonObject pendingAuthOptions_;
    QByteArray pendingChallenge_;
};
class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject *parent, const QString &deviceDirectory = {});
    ~Client() override;
    void start(const Invitation &invitation);
    void stop();
    void sendInput(const QJsonObject &event);
    void release();
    void wakeDesktop();
    FileTransferClient *fileTransfer() { return &files_; }
    void recordPaint();
    void setStatsFile(const QString &path) { statsFile_ = path; }
signals:
    void status(QString text);
    void frame(QImage image);
    void capability(bool control);
    void loginScreenCapability(bool enabled);
    void disconnected();
    void statistics(QString text);
    void deviceAuthenticated(QString deviceId, QString deviceName, QString host, quint16 port, QString fingerprint);
    void decodeFrame(quint64 generation, QString codec, QByteArray bytes);
private:
    void receive(Packet type, const QByteArray &payload);
    void fail(const QString &message);
    void reportStats();
    void sendAuth();
    QSslSocket *socket_ = nullptr;
    Wire *wire_ = nullptr;
    Invitation invitation_;
    QTimer timer_;
    QThread decodeThread_;
    DecodeWorker *decoder_;
    QTimer statsTimer_;
    QElapsedTimer statsClock_, intervalClock_;
    QString statsFile_, codec_;
    quint64 generation_ = 0, receivedFrames_ = 0, decodedFrames_ = 0, paintedFrames_ = 0,
            distinctFrames_ = 0, receivedBytes_ = 0, lastFingerprint_ = 0;
    quint64 intervalDecoded_ = 0, intervalPainted_ = 0, intervalBytes_ = 0;
    int targetFps_ = 0, frameWindow_ = 1, queuedDecode_ = 0;
    double decodeTotalMs_ = 0;
    QElapsedTimer lastMessage_;
    bool ready_ = false;
    bool control_ = false, loginScreen_ = false, wakePending_ = false;
    bool resumeAuth_ = false;
    DeviceIdentity deviceIdentity_;
    std::unique_ptr<DeviceIdentityStore> deviceStore_;
    QString deviceName_;
    QString deviceStorageError_;
    FileTransferClient files_;
    ClipboardSync clipboard_;
};
class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(QWidget *parent = nullptr);
    void setFrame(QImage image);
    void setControl(bool enabled);
    void reset();
signals: void input(QJsonObject event); void releaseKeys(); void presented();
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    bool focusNextPrevChild(bool) override { return false; }
private:
    QRectF imageRect() const;
    QJsonObject position(const QPointF &point) const;
    void mouseButton(QMouseEvent *event, bool down);
    QImage image_;
    bool control_ = false;
    QSet<int> pressedButtons_;
    quint64 frameSerial_ = 0, paintedSerial_ = 0;
};
class ControlWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ControlWindow();
    Viewer *viewer() const { return viewer_; }
    void setStatus(const QString &text);
    void setStatistics(const QString &text);
    void setLoginScreenCapability(bool enabled);
    void setFileCapability(bool enabled);
    void setCleanView(bool enabled);
    bool cleanView() const { return cleanView_; }
    void sessionEnded();
signals:
    void disconnectRequested();
    void releaseRequested();
    void wakeRequested();
    void filesRequested();
    void cleanViewChanged(bool enabled);
protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    Viewer *viewer_;
    QLabel *status_;
    QLabel *performance_;
    QPushButton *wakeButton_;
    QPushButton *filesButton_;
    QWidget *toolbar_;
    QWidget *information_;
    QCheckBox *cleanViewToggle_;
    QVBoxLayout *layout_;
    QMargins normalMargins_;
    int normalSpacing_ = 0;
    bool cleanView_ = false;
    QRect windowedGeometry_;
    Qt::WindowStates windowedState_;
    QSet<int> localShortcutKeys_;
};
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(bool headlessHost = false);
    ~Window() override;
    bool startHost(const QString &bind = {}, quint16 port = 0, const QString &inviteFile = {});
    void setViewOnly() { allowControl_->setChecked(false); }
    void setLockOnDisconnect(bool enabled) { lockOnDisconnect_->setChecked(enabled); }
    void configureFiles(const QString &directory, bool enabled);
    void connectFile(const QString &path);
    void configureVideo(int fps, const QString &codec);
    void setStatsFile(const QString &path) { client_.setStatsFile(path); }
protected: void closeEvent(QCloseEvent *) override;
private:
    void connectCode();
    void connectSavedDevice();
    void refreshSavedDevices();
    void refreshTrustedDevices();
    bool setAutostart(bool enabled, QString &error);
    void showStatus(const QString &text);
    void updateHostFileActions();
    void chooseHostFile();
    void receiveFileOffer(const QString &id, const QString &name, qint64 size);
    Host host_;
    Client client_;
    std::unique_ptr<DeviceProfileStore> profileStore_;
    QList<SavedDevice> savedDeviceRecords_;
    QComboBox *addresses_;
    QSpinBox *port_;
    QComboBox *fps_, *codec_;
    QCheckBox *allowControl_;
    QCheckBox *lockOnDisconnect_;
    QPushButton *share_;
    QPushButton *resetCode_;
    QCheckBox *autostart_;
    QComboBox *trustedDevices_;
    QPushButton *forgetTrusted_;
    QString inviteFile_;
    bool preloginServiceEnabled_ = false;
    QPlainTextEdit *invitation_;
    QPlainTextEdit *connectCode_;
    QComboBox *savedDevices_;
    QPushButton *connectSaved_, *forgetSaved_;
    QLabel *status_;
    QTabWidget *tabs_;
    std::unique_ptr<ControlWindow> controlWindow_;
    std::unique_ptr<FileTransferDialog> fileWindow_;
    QCheckBox *allowFiles_;
    QLineEdit *fileDirectory_;
    QPushButton *chooseFileDirectory_;
    QPushButton *sendHostFile_, *cancelHostFile_;
    QLabel *hostFileStatus_;
    QProgressBar *hostFileProgress_;
    QPointer<QFileDialog> hostFileDialog_, incomingFileDialog_;
    bool headlessHost_ = false;
    bool autostartChanging_ = false;
};
}
