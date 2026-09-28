#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QList>

struct MarkerEvent {
	QString name;
	QJsonObject data;
	QDateTime timestamp;
	qint64 elapsedMs;
};
class MarkerTracker {
public:
	QList<MarkerEvent> observe(const QJsonObject &marker, qint64 ms, const QDateTime &wall);

	QJsonObject current() const { return active; }

private:
	QJsonObject active, pending;
	int count = 0;
	qint64 firstMs = 0, absentMs = -1;
	QDateTime firstWall, absentWall;
};
