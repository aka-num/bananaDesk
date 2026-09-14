#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QPointer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;

namespace ld {
class FileTransferClient;

class FileTransferDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileTransferDialog(FileTransferClient *client, QWidget *parent = nullptr);
    ~FileTransferDialog() override;
    void reject() override;
private:
    void updateActions();
    void setAvailable(bool available);
    void setBusy(bool busy);
    void setStatus(const QString &text);
    void showListing(const QJsonArray &files);
    void showProgress(qint64 done, qint64 total);
    void uploadFile();
    void downloadFile();
    QString selectedName() const;

    QPointer<FileTransferClient> client_;
    QTreeWidget *files_;
    QPushButton *refresh_, *upload_, *download_, *cancel_;
    QProgressBar *progress_;
    QLabel *status_;
    QString localDirectory_;
    bool available_ = false, busy_ = false;
};
}
