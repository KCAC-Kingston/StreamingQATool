#pragma once
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QHash>

class EventLog : public QObject {
	Q_OBJECT
public:
	explicit EventLog(QString folder, QObject *parent = nullptr);
	~EventLog() override;
	QString runFilePath() const;
	void configure(int retainDays);
	void append(const QString &source, const QString &event, const QJsonObject &details = {},
		    const QDateTime &timestamp = {});
	void prune();
	QString folder() const { return directory; }
	QStringList recent() const { return history; }
	QString error() const { return lastError; }
	static QJsonObject sanitize(const QJsonObject &details);
signals:
	void entryAdded(const QString &line);
	void storageError(const QString &message);

private:
	QString directory;
	QString runFile, streamFile;
	bool streamObsEnded = false, streamRemoteExpected = false, streamRemoteEnded = false;
	QHash<QString, QString> connectionStates;
	QString lastError;
	QStringList history;
	int retention = 90;

	QTimer cleanup;
};
