#pragma once
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <QHash>
#include <QSet>

class EventLog : public QObject {
	Q_OBJECT
public:
	explicit EventLog(QString folder, QObject *parent = nullptr);
	~EventLog() override;
	QString runFilePath() const;
	QString streamFilename() const { return streamFile; }
	void finishStream();
	QString resumeStream(const QString &filename);
	void protectFiles(const QSet<QString> &files) { protectedFiles = files; }
	void configure(int retainDays);
	void append(const QString &source, const QString &event, const QJsonObject &details = {},
		    const QDateTime &timestamp = {});
	void prune();
	QString folder() const { return directory; }
	QStringList recent() const { return history; }
	QString error() const { return lastError; }
	static QJsonObject sanitize(const QJsonObject &details);
signals:
	void streamCompleted(const QString &filename);
	void entryAdded(const QString &line);
	void storageError(const QString &message);

private:
	QSet<QString> protectedFiles;
	QString directory;
	QString runFile, streamFile;
	bool streamObsEnded = false, streamRemoteExpected = false, streamRemoteEnded = false;
	QHash<QString, QString> connectionStates;
	QString lastError;
	QStringList history;
	int retention = 90;

	QTimer cleanup;
};
