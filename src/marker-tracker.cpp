#include "marker-tracker.h"

QList<MarkerEvent> MarkerTracker::observe(const QJsonObject &marker, qint64 ms, const QDateTime &wall)
{
	QList<MarkerEvent> events;
	if (marker.isEmpty()) {
		pending = {};
		count = 0;
		if (absentMs < 0) {
			absentMs = ms;
			absentWall = wall;
		}
		if (!active.isEmpty() && ms - absentMs >= 1000) {
			events.append({"marker.disappeared", active, absentWall, absentMs});
			active = {};
		}
		return events;
	}
	absentMs = -1;
	if (marker != pending) {
		pending = marker;
		count = 1;
		firstMs = ms;
		firstWall = wall;
	} else if (count < 3)
		++count;
	if (count >= 3 && marker != active) {
		if (!active.isEmpty())
			events.append({"marker.disappeared", active, firstWall, firstMs});
		active = marker;
		events.append({"marker.appeared", marker, firstWall, firstMs});
	}
	return events;
}
