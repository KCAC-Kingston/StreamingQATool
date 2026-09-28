#include "event-log.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include "event-format.h"

namespace {
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
	cleanup.setInterval(60 * 60 * 1000);
	connect(&cleanup, &QTimer::timeout, this, &EventLog::prune);
	cleanup.start();
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

void EventLog::append(const QString &source, const QString &event, const QJsonObject &details)
{
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
	const auto now = QDateTime::currentDateTimeUtc();
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
		QFile file(QDir(directory).filePath("streamingqa-" + now.date().toString(Qt::ISODate) + ".jsonl"));
		if (!file.open(QIODevice::WriteOnly | QIODevice::Append) ||
		    file.write(bytes + '\n') != bytes.size() + 1 || !file.flush())
			error = "Cannot save the event log: " + file.errorString();
	}
	if (error != lastError) {
		lastError = error;
		emit storageError(lastError);
	}
	emit entryAdded(line);
}

void EventLog::prune()
{
	QDir dir(directory);
	const auto cutoff = QDateTime::currentDateTimeUtc().date().addDays(1 - retention);
	const QRegularExpression pattern("^streamingqa-(\\d{4}-\\d{2}-\\d{2})\\.jsonl$");
	for (const auto &file : dir.entryInfoList({"streamingqa-*.jsonl"}, QDir::Files | QDir::NoSymLinks)) {
		const auto match = pattern.match(file.fileName());
		const auto date = QDate::fromString(match.captured(1), Qt::ISODate);
		if (match.hasMatch() && date.isValid() && date < cutoff &&
		    file.canonicalPath() == dir.canonicalPath()) {
			if (!QFile::remove(file.absoluteFilePath()))
				emit storageError("Could not remove an expired event log: " + file.fileName());
		}
	}
}
