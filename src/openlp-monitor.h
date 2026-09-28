#pragma once
#include "local-websocket.h"
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QHash>

class OpenLpMonitor : public QObject {
	Q_OBJECT
public:
	explicit OpenLpMonitor(QObject *parent = nullptr);
	~OpenLpMonitor() override;
	void configure(const QString &host, int port, const QString &version, bool enabled);
	static QJsonObject slideState(const QJsonObject &payload);
signals:
	void event(const QString &name, const QJsonObject &details);
	void statusChanged(const QString &status);

private:
	void fetch();
	void process(const QJsonObject &payload);
	void report(const QString &status);
	void retry();
	void fetchTitles();
	void flushPending();
	QNetworkAccessManager network;
	LocalWebSocket websocket;
	QTimer timer;
	QString hostname;
	QString protocol;
	QString state;
	int httpPort = 4316;
	int generation = 0;
	bool enabled = false;
	bool busy = false;
	QJsonObject previous;
	QHash<QString, QString> titles;
	QList<QJsonObject> pending;
	bool titlesBusy = false;
};
