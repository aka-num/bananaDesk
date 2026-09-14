#include "file_transfer_dialog.h"
#include "file_transfer.h"

#include <QAbstractItemView>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace ld {
namespace {
QString fileSize(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    const char *units[] = {"KiB", "MiB", "GiB", "TiB"};
    double size = double(bytes) / 1024;
    int unit = 0;
    while (size >= 1024 && unit < 3) { size /= 1024; ++unit; }
    return QStringLiteral("%1 %2").arg(size, 0, 'f', size >= 10 ? 1 : 2).arg(QString::fromLatin1(units[unit]));
}
bool plainName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
        && !name.contains('/') && !name.contains('\\') && !name.contains(QChar::Null);
}
}

FileTransferDialog::FileTransferDialog(FileTransferClient *client, QWidget *parent)
    : QDialog(parent), client_(client) {
    setWindowTitle(QStringLiteral("bananaDesk · 文件传输"));
    setObjectName(QStringLiteral("fileTransferDialog"));
    resize(640, 480); setMinimumSize(420, 300);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    auto *description = new QLabel(QStringLiteral("被控端共享文件夹中的文件。一次传一个文件，上传同名文件会被拒绝。"));
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true);
    description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(description);

    files_ = new QTreeWidget;
    files_->setObjectName(QStringLiteral("remoteFiles"));
    files_->setColumnCount(2); files_->setHeaderLabels({QStringLiteral("文件名"), QStringLiteral("大小")});
    files_->setRootIsDecorated(false); files_->setItemsExpandable(false);
    files_->setSelectionMode(QAbstractItemView::SingleSelection);
    files_->setSelectionBehavior(QAbstractItemView::SelectRows);
    files_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    files_->setUniformRowHeights(true);
    files_->header()->setStretchLastSection(false);
    files_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    files_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(files_, 1);

    auto *actions = new QHBoxLayout;
    refresh_ = new QPushButton(QStringLiteral("刷新")); refresh_->setObjectName(QStringLiteral("refreshFiles"));
    upload_ = new QPushButton(QStringLiteral("上传文件")); upload_->setObjectName(QStringLiteral("uploadFile"));
    download_ = new QPushButton(QStringLiteral("下载所选")); download_->setObjectName(QStringLiteral("downloadFile"));
    cancel_ = new QPushButton(QStringLiteral("取消传输")); cancel_->setObjectName(QStringLiteral("cancelFileTransfer"));
    for (auto *button : {refresh_, upload_, download_, cancel_}) {
        button->setAutoDefault(false); actions->addWidget(button);
    }
    actions->addStretch(); layout->addLayout(actions);
    progress_ = new QProgressBar;
    progress_->setObjectName(QStringLiteral("fileTransferProgress"));
    progress_->setRange(0, 1000); progress_->setValue(0);
    layout->addWidget(progress_);
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("fileTransferStatus"));
    status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    status_->setMinimumHeight(status_->fontMetrics().lineSpacing() * 2);
    status_->setMaximumHeight(status_->fontMetrics().lineSpacing() * 3);
    layout->addWidget(status_);

    localDirectory_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (localDirectory_.isEmpty()) localDirectory_ = QDir::homePath();
    connect(files_, &QTreeWidget::itemSelectionChanged, this, &FileTransferDialog::updateActions);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        if (client_ && available_ && !busy_) client_->refresh();
    });
    connect(upload_, &QPushButton::clicked, this, &FileTransferDialog::uploadFile);
    connect(download_, &QPushButton::clicked, this, &FileTransferDialog::downloadFile);
    connect(files_, &QTreeWidget::itemDoubleClicked, this, [this] { downloadFile(); });
    connect(cancel_, &QPushButton::clicked, this, [this] { if (client_) client_->cancel(); });
    if (client_) {
        connect(client_, &FileTransferClient::listing, this, &FileTransferDialog::showListing);
        connect(client_, &FileTransferClient::progress, this, &FileTransferDialog::showProgress);
        connect(client_, &FileTransferClient::status, this, &FileTransferDialog::setStatus);
        connect(client_, &FileTransferClient::busyChanged, this, &FileTransferDialog::setBusy);
        connect(client_, &FileTransferClient::availableChanged, this, &FileTransferDialog::setAvailable);
        connect(client_, &QObject::destroyed, this, [this] { setAvailable(false); });
        available_ = client_->available(); busy_ = client_->busy();
    }
    setStatus(available_ ? QStringLiteral("可上传文件，或选择远端文件下载。")
                         : QStringLiteral("文件传输不可用：请连接已允许文件传输的被控端。"));
    setBusy(busy_);
}

FileTransferDialog::~FileTransferDialog() { if (client_) client_->cancel(); }

void FileTransferDialog::reject() {
    if (client_) client_->cancel();
    QDialog::reject();
}

QString FileTransferDialog::selectedName() const {
    auto *item = files_->currentItem();
    return item && item->isSelected() ? item->data(0, Qt::UserRole).toString() : QString();
}

void FileTransferDialog::updateActions() {
    const bool ready = client_ && available_ && !busy_;
    refresh_->setEnabled(ready); upload_->setEnabled(ready);
    download_->setEnabled(ready && !selectedName().isEmpty());
    cancel_->setEnabled(client_ && available_ && busy_);
    files_->setEnabled(client_ && available_ && !busy_);
}

void FileTransferDialog::setAvailable(bool available) {
    available_ = available;
    if (!available_) {
        busy_ = false; files_->clear(); progress_->setRange(0, 1000); progress_->setValue(0);
        progress_->setFormat(QStringLiteral("%p%"));
        setStatus(QStringLiteral("文件传输已停用或连接已断开。"));
    }
    updateActions();
    if (available_ && isVisible() && client_ && !busy_) client_->refresh();
}

void FileTransferDialog::setBusy(bool busy) {
    busy_ = available_ && busy;
    if (busy_) { progress_->setRange(0, 0); progress_->setFormat(QStringLiteral("正在处理…")); }
    else if (progress_->maximum() == 0) {
        progress_->setRange(0, 1000); progress_->setValue(0); progress_->setFormat(QStringLiteral("%p%"));
    }
    updateActions();
}

void FileTransferDialog::setStatus(const QString &text) {
    status_->setText(text); status_->setToolTip(text);
}

void FileTransferDialog::showListing(const QJsonArray &files) {
    if (!available_) return;
    const QString selected = selectedName();
    files_->clear();
    for (const auto &value : files) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value("name").toString();
        const double size = entry.value("size").toDouble(-1);
        if (!plainName(name) || !std::isfinite(size) || size < 0 || size > 9007199254740991.0) continue;
        auto *item = new QTreeWidgetItem(files_, {name, fileSize(qint64(size))});
        item->setData(0, Qt::UserRole, name); item->setToolTip(0, name);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        if (name == selected) { files_->setCurrentItem(item); item->setSelected(true); }
    }
    files_->sortItems(0, Qt::AscendingOrder);
    updateActions();
}

void FileTransferDialog::showProgress(qint64 done, qint64 total) {
    if (!available_) return;
    done = std::max<qint64>(0, done); total = std::max<qint64>(0, total);
    progress_->setRange(0, 1000);
    const int value = total > 0 ? int(std::min<long double>(1000, static_cast<long double>(done) * 1000 / total)) : 0;
    progress_->setValue(value);
    progress_->setFormat(QStringLiteral("%1 / %2 (%p%)").arg(fileSize(done), fileSize(total)));
}

void FileTransferDialog::uploadFile() {
    if (!client_ || !available_ || busy_) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择上传文件"), localDirectory_, QStringLiteral("所有文件 (*)"));
    if (path.isEmpty() || !client_ || !available_ || busy_) return;
    localDirectory_ = QFileInfo(path).absolutePath();
    client_->upload(path);
}

void FileTransferDialog::downloadFile() {
    if (!client_ || !available_ || busy_) return;
    const QString name = selectedName();
    if (!plainName(name)) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("下载文件"),
        QDir(localDirectory_).filePath(name), QStringLiteral("所有文件 (*)"));
    if (path.isEmpty() || !client_ || !available_ || busy_) return;
    localDirectory_ = QFileInfo(path).absolutePath();
    client_->download(name, path);
}
}
