#pragma once
#include "marker-tracker.h"
#include <QObject>
#include <QThread>
#include <atomic>
#include <graphics/graphics.h>

// Samples the composed program canvas; decoding never runs on OBS's render thread.
class MarkerMonitor : public QObject {
	Q_OBJECT
public:
	explicit MarkerMonitor(QObject *parent = nullptr);
	~MarkerMonitor() override;
signals:
	void event(const QString &name, const QJsonObject &details, const QDateTime &timestamp);
	void statusChanged(const QString &status);
	void statisticsChanged(const QJsonObject &stats);

private:
	static void rendered(void *data);
	void capture();
	void report(const QString &message);
	QThread thread;
	QObject *worker;
	MarkerTracker tracker;
	qint64 samples = 0, validSamples = 0, appearances = 0, disappearances = 0, lastStatsMs = -1000;
	QString lastSeen;
	std::atomic_bool busy{false};
	gs_stagesurf_t *surface = nullptr;
	uint32_t width = 0, height = 0;
	gs_color_format format = GS_UNKNOWN;
	uint64_t lastFrame = 0, origin = 0;
	QString lastStatus;
};
