#pragma once
#include <QString>

namespace ld {
// Requests display wake-up in the current interactive session. This never
// unlocks the operating system, changes its lock policy, or enters credentials.
// Success means a wake request was issued, not that a password was accepted.
bool wakeDesktop(QString &error);
}
