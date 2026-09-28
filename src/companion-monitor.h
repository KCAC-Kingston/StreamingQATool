#pragma once
#include "local-websocket.h"
#include <QJsonObject>
#include <QSet>
#include <QTimer>

class CompanionMonitor : public QObject {
	Q_OBJECT
public:
	explicit CompanionMonitor(QObject *parent = nullptr);
	~CompanionMonitor() override;
	void configure(const QString &host, int port, bool enabled);
	static QJsonObject keyPress(const QJsonObject &line);
signals:
	void event(const QString &name, const QJsonObject &details);
	void statusChanged(const QString &status);

private:
	void connectStream();
	void consume(const QByteArray &bytes);
	void report(const QString &value);
	void retry();
	LocalWebSocket websocket;
	QTimer reconnect;
	QString hostname;
	QString state;
	int wsPort = 8000;
	bool enabled = false;
	bool initialHistory = true;
	QSet<QByteArray> seen;
};
