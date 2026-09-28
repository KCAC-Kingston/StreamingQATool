#pragma once
#include <QObject>
#include <QTimer>
#include <QJsonObject>
#include <obs.h>

class RtmpMonitor : public QObject {
	Q_OBJECT
public:
	explicit RtmpMonitor(QObject *parent = nullptr);
	~RtmpMonitor() override;
	void start();
	void refresh();
signals:
	void event(const QString &name, const QJsonObject &details);

private:
	void detach();
	static void reconnect(void *data, calldata_t *params);
	static void reconnected(void *data, calldata_t *params);
	static void stopped(void *data, calldata_t *params);
	void post(const QString &name, const QJsonObject &details = {});
	obs_output_t *output = nullptr;
	QTimer timer;
};
