#pragma once
#include <QString>

namespace ld {
// Requests a lock of the current interactive desktop. Success means that the
// operating system accepted the request, not that a locked screen was observed.
bool lockDesktop(QString &error);
}
