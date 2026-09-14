#pragma once
#include "protocol.h"
#include "native_input.h"
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

namespace ld {
class Listener : public QTcpServer {
    Q_OBJECT
public: using QTcpServer::QTcpServer;
signals: void accepted(qintptr descriptor);
protected: void incomingConnection(qintptr descriptor) override { emit accepted(descriptor); }
};
class Host : public QObject {
    Q_OBJECT
public:
    explicit Host(QObject *parent);
    ~Host() override;
    bool start(const QHostAddress &bind, quint16 port, bool control, QString &error);
    void stop();
    QString invitation() const;
    bool running() const { return server_.isListening(); }
signals: void status(QString text);
private:
    void accept(qintptr descriptor);
    void receive(Packet type, const QByteArray &payload);
    void tick();
    void capture();
    void drop(const QString &reason);
    Listener server_;
    Identity identity_;
    NativeInput input_;
    QHostAddress address_;
    QSslSocket *socket_ = nullptr;
    Wire *wire_ = nullptr;
    QTimer timer_;
    QElapsedTimer lastMessage_, packetWindow_;
    int packets_ = 0;
    bool authenticated_ = false, control_ = false, framePending_ = false;
    QRect screen_;
};
class Client : public QObject {
    Q_OBJECT
public:
    explicit Client(QObject *parent);
    void start(const Invitation &invitation);
    void stop();
    void sendInput(const QJsonObject &event);
    void release();
signals:
    void status(QString text);
    void frame(QImage image);
    void capability(bool control);
    void disconnected();
private:
    void receive(Packet type, const QByteArray &payload);
    void fail(const QString &message);
    QSslSocket *socket_ = nullptr;
    Wire *wire_ = nullptr;
    Invitation invitation_;
    QTimer timer_;
    QElapsedTimer lastMessage_;
    bool ready_ = false;
};
class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(QWidget *parent = nullptr);
    void setFrame(QImage image);
    void setControl(bool enabled);
    void reset();
signals: void input(QJsonObject event); void releaseKeys();
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
};
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window();
    bool startHost(const QString &bind = {}, quint16 port = DefaultPort, const QString &inviteFile = {});
    void setViewOnly() { allowControl_->setChecked(false); }
    void connectFile(const QString &path);
protected: void closeEvent(QCloseEvent *) override;
private:
    void connectCode();
    void showStatus(const QString &text);
    Host host_;
    Client client_;
    QComboBox *addresses_;
    QSpinBox *port_;
    QCheckBox *allowControl_;
    QPushButton *share_;
    QPlainTextEdit *invitation_;
    QPlainTextEdit *connectCode_;
    QLabel *status_;
    QTabWidget *tabs_;
    Viewer *viewer_;
};
}
