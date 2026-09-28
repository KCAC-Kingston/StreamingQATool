#pragma once
#include <QImage>
#include <QJsonObject>
#include <QVector>

namespace MarkerDecoder {
QJsonObject bits(const QVector<int> &bits);
QJsonObject pixels(const QImage &image, const QRect &box);
QJsonObject find(const QImage &image);
QJsonObject find(const QImage &image, const QVector<QRect> &regions);
} // namespace MarkerDecoder
