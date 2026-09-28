#pragma once
#include <QJsonObject>
#include <QString>

namespace EventFormat {
QJsonObject details(const QString &event, const QJsonObject &input);
QString text(const QString &source, const QString &event, const QJsonObject &details);
} // namespace EventFormat
