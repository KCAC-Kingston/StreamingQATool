#pragma once
#include "service-manager-client.h"
#include <QJsonArray>
#include <QTimer>
#include <functional>
class EventLog;

// Durable recovery and upload queue. Contains no credentials or RTMP keys.
class LiveSession : public QObject {
	Q_OBJECT
public:
	LiveSession(EventLog &log, QString statePath, QObject *parent = nullptr);
	void configure(bool enabled, const QString &host, const QString &key);
	void observe(const QString &event, const QJsonObject &details);
	void recover();
	void dismissRecovery();
	void retry();
	std::function<QString(const QString &, const QJsonObject &)> restore;
signals:
	void recoveryPrompt(const QString &service);
	void hideRecovery();
	void notice(const QString &text);

private:
	bool save();
	void protect();
	void checkRecovery();
	void inspectRecovery(const QJsonObject &body, bool accepted);
	void completed(const QString &file);
	void upload();
	void uploaded(const QJsonObject &job);
	void uploadError(const ApiError &error);
	EventLog &log;
	QString statePath, host, key;
	QJsonObject active, candidate;
	QJsonArray pending;
	ServiceManagerClient client;
	QTimer timer;
	bool recoveredThisRun = false, recoveryDismissed = false;
	bool enabled = false, checking = false, uploading = false, checked = false, uploadBlocked = false;
	int generation = 0;
};
