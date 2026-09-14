#pragma once
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <memory>

namespace ld {
class NativeInput {
public:
    NativeInput();
    ~NativeInput();
    bool available() const;
    QString error() const;
    bool apply(const QJsonObject &event, const QRect &screen);
    void releaseAll();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
