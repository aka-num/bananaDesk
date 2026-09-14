#pragma once
#include <QImage>
#include <QJsonObject>
#include <QRect>
#include <QString>

namespace ld {
// These calls never install/elevate anything. Installation is an explicit local action.
bool windowsHelperInstalled();
void windowsHelperSetEnabled(bool enabled);
bool windowsHelperCapture(QImage &image, QRect &bounds, QString &error);
enum class WindowsInputResult { Applied, Unsupported, Failed };
WindowsInputResult windowsHelperInput(const QJsonObject &event, QString &error);
void windowsHelperRelease();
bool windowsHelperWake(QString &error);
void windowsHelperDisconnect();
}
