#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <memory>

namespace ld {
constexpr int MaxClipboardBytes = 64 * 1024;

// Pure text only. The caller enforces authenticated session permissions.
// Enabling snapshots current content; it never exports an old clipboard.
class ClipboardSync : public QObject {
    Q_OBJECT
public:
    explicit ClipboardSync(QObject *parent = nullptr);
    ~ClipboardSync() override;
    void setEnabled(bool enabled);
    bool enabled() const;
    bool receive(const QByteArray &utf8);
    // App-owned secrets such as invitation codes must stay on this computer.
    static void setLocalOnlyText(const QString &text);
signals:
    void send(QByteArray utf8);
    void status(QString text);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
