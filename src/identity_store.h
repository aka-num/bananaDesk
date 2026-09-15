#pragma once

#include "protocol.h"
#include <memory>

class QLockFile;

namespace ld {
// Each active sharing host holds the profile lock so a reset cannot leave
// another process accepting credentials that have just been revoked.
class IdentityStore {
public:
    explicit IdentityStore(QString directory);
    ~IdentityStore();
    bool loadOrCreate(Identity &out, QString &error);
    bool reset(Identity &out, QString &error);
    void release();
    QString filePath() const;
private:
    bool acquire(QString &error);
    bool prepareDirectory(QString &error);
    bool checkFile(QString &error) const;
    bool read(Identity &out, QString &error) const;
    bool save(const Identity &identity, QString &error) const;
    QString directory_;
    std::unique_ptr<QLockFile> lock_;
};
}
