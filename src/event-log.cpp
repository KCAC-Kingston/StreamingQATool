#include "event-log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include "event-format.h"

namespace {
QString newLogName(const QString &kind)
{
	return "streamingqa-" + kind + "-" + QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd_HHmmsszzz") + "-" +
	       QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + ".jsonl";
}

QJsonValue cleanValue(const QJsonValue &value)
{
	if (value.isObject())
		return EventLog::sanitize(value.toObject());
	if (value.isArray()) {
		QJsonArray cleaned;
		for (const auto &item : value.toArray())
			cleaned.append(cleanValue(item));
		return cleaned;
	}
	if (value.isString())
		return value.toString().left(4096);
	return value;
}
} // namespace

EventLog::EventLog(QString folder, QObject *parent) : QObject(parent), directory(std::move(folder))
{
	runFile = newLogName("run");
	cleanup.setInterval(60 * 60 * 1000);
	connect(&cleanup, &QTimer::timeout, this, &EventLog::prune);
	cleanup.start();
}

EventLog::~EventLog()
{
	if (!streamFile.isEmpty())
		append("application", "stream.interrupted", {{"reason", "OBS closed before stream completion"}});
	append("application", "ended");
}

QString EventLog::runFilePath() const
{
	return QDir(directory).filePath(runFile);
}

QJsonObject EventLog::sanitize(const QJsonObject &details)
{
	static const QRegularExpression secret("api.?key|stream.?key|stream.?name|password|authorization|token",
					       QRegularExpression::CaseInsensitiveOption);
	QJsonObject cleaned;
	for (auto it = details.begin(); it != details.end(); ++it)
		cleaned.insert(it.key(),
			       secret.match(it.key()).hasMatch() ? QJsonValue("[redacted]") : cleanValue(it.value()));
	return cleaned;
}

void EventLog::configure(int retainDays)
{
	retention = qBound(1, retainDays, 3650);
	prune();
}

void EventLog::append(const QString &source, const QString &event, const QJsonObject &details,
		      const QDateTime &timestamp)
{
	if (event == "prestart.requested") {
		if (!streamFile.isEmpty())
			append("application", "stream.interrupted", {{"reason", "New Prestart attempt"}});
		streamFile = newLogName("stream");
		streamObsEnded = streamRemoteExpected = streamRemoteEnded = false;
	}
	if (!streamFile.isEmpty()) {
		if (source == "obs" && event == "streaming.ended")
			streamObsEnded = true;
		if (source == "obs" && event == "streaming.started")
			streamObsEnded = false;
		if (event == "youtube.start.requested" || event == "youtube.streaming.started")
			streamRemoteExpected = true;
		if (event == "youtube.streaming.ended")
			streamRemoteEnded = true;
	}
	// Internal workflow chatter is already represented by requests and confirmations.
	if (event == "youtube.status" || event == "prestart.encoder_start_requested")
		return;
	auto concise = EventFormat::details(event, sanitize(details));
	if (event == "connection.state") {
		const QString state = concise["state"].toString();
		if (state == "Connecting" || connectionStates.value(source) == state)
			return;
		connectionStates.insert(source, state);
	}
	const auto now = timestamp.isValid() ? timestamp.toUTC() : QDateTime::currentDateTimeUtc();
	QJsonObject record{{"timestamp", now.toString(Qt::ISODateWithMs)}, {"source", source}, {"event", event}};
	if (!concise.isEmpty())
		record.insert("details", concise);
	const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
	const QString line = now.toLocalTime().toString("HH:mm:ss") + "  " + EventFormat::text(source, event, concise);
	history.append(line);
	while (history.size() > 2000)
		history.removeFirst();
	QString error;
	if (!QDir().mkpath(directory)) {
		error = "Cannot create the event log folder.";
	} else {
		QStringList files{runFile};
		if (!streamFile.isEmpty())
			files.append(streamFile);
		for (const auto &name : files) {
			QFile file(QDir(directory).filePath(name));
			if (!file.open(QIODevice::WriteOnly | QIODevice::Append) ||
			    file.write(bytes + '\n') != bytes.size() + 1 || !file.flush())
				error += "Cannot save " + name + ": " + file.errorString() + " ";
		}
	}
	if (error != lastError) {
		lastError = error;
		emit storageError(lastError);
	}
	emit entryAdded(line);
	if (event == "stream.interrupted" || (streamObsEnded && (!streamRemoteExpected || streamRemoteEnded)))
		streamFile.clear();
}

void EventLog::prune()
{
	QDir dir(directory);
	const auto cutoff = QDateTime::currentDateTimeUtc().date().addDays(1 - retention);
	const QRegularExpression pattern(
		"^streamingqa-(?:(?:run|stream)-)?(\\d{4}-\\d{2}-\\d{2})(?:_\\d{9}-[0-9a-f]{8})?\\.jsonl$");
	for (const auto &file : dir.entryInfoList({"streamingqa-*.jsonl"}, QDir::Files | QDir::NoSymLinks)) {
		if (file.fileName() == runFile || file.fileName() == streamFile)
			continue;
		const auto match = pattern.match(file.fileName());
		const auto date = QDate::fromString(match.captured(1), Qt::ISODate);
		if (match.hasMatch() && date.isValid() && date < cutoff &&
		    ((!file.fileName().startsWith("streamingqa-run-") &&
		      !file.fileName().startsWith("streamingqa-stream-")) ||
		     file.lastModified().toUTC().date() < cutoff) &&
		    file.canonicalPath() == dir.canonicalPath()) {
			if (!QFile::remove(file.absoluteFilePath()))
				emit storageError("Could not remove an expired event log: " + file.fileName());
		}
	}
}
