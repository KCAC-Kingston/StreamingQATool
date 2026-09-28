#include "live-session.h"
#include "event-log.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {
bool validJob(const QJsonObject &job)
{
	return QRegularExpression("^[1-9][0-9]*$").match(job["serviceId"].toString()).hasMatch() &&
	       QRegularExpression("^streamingqa-stream-[0-9-]+_[0-9]{9}-[0-9a-f]{8}\\.jsonl$")
		       .match(job["file"].toString())
		       .hasMatch() &&
	       ServiceManagerClient::validHost(job["host"].toString());
}
QString controlPath(const QJsonObject &job)
{
	return "/api/admin/v1/services/" + job["serviceId"].toString() + "/youtube/live-control";
}
} // namespace

LiveSession::LiveSession(EventLog &logger, QString path, QObject *parent)
	: QObject(parent),
	  log(logger),
	  statePath(std::move(path))
{
	QFile file(statePath);
	if (file.open(QIODevice::ReadOnly)) {
		const auto state = QJsonDocument::fromJson(file.readAll()).object();
		if (validJob(state["active"].toObject()))
			active = state["active"].toObject();
		for (const auto &value : state["pending"].toArray())
			if (validJob(value.toObject()))
				pending.append(value);
	}
	protect();
	connect(&log, &EventLog::streamCompleted, this, &LiveSession::completed);
	timer.setInterval(300000);
	connect(&timer, &QTimer::timeout, this, [this]() {
		checkRecovery();
		upload();
	});
	timer.start();
}

void LiveSession::protect()
{
	QSet<QString> files;
	if (!active.isEmpty())
		files.insert(active["file"].toString());
	for (const auto &value : pending)
		files.insert(value.toObject()["file"].toString());
	log.protectFiles(files);
}

bool LiveSession::save()
{
	protect();
	QDir().mkpath(QFileInfo(statePath).absolutePath());
	QSaveFile file(statePath);
	const auto bytes = QJsonDocument(QJsonObject{{"active", active}, {"pending", pending}}).toJson();
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
		emit notice("Could not save recovery/upload state: " + file.errorString());
		return false;
	}
	return true;
}

void LiveSession::configure(bool on, const QString &newHost, const QString &newKey)
{
	const auto normalized = ServiceManagerClient::normalizeHost(newHost);
	if (enabled == on && host == normalized && key == newKey)
		return;
	++generation;
	checking = uploading = checked = uploadBlocked = false;
	if (!on)
		recoveredThisRun = false;
	checked = recoveredThisRun;
	enabled = on;
	host = normalized;
	key = newKey;
	client.configure(host, key);
	emit hideRecovery();
	if (!enabled) {
		emit notice({});
		return;
	}
	QTimer::singleShot(0, this, [this]() {
		checkRecovery();
		upload();
	});
}

void LiveSession::observe(const QString &event, const QJsonObject &details)
{
	if (event == "service.completed" && candidate["serviceId"] == details["serviceId"])
		log.finishStream();
	if (event == "prestart.requested") {
		candidate = {{"host", host},
			     {"serviceId", details["serviceId"]},
			     {"serviceTitle", details["serviceTitle"]},
			     {"file", log.streamFilename()}};
	}
	if (event == "youtube.streaming.started" &&
	    (!validJob(candidate) || candidate["file"].toString() != log.streamFilename()))
		candidate = {{"host", host},
			     {"serviceId", details["serviceId"]},
			     {"serviceTitle", details["serviceTitle"]},
			     {"file", log.streamFilename()}};
	if (event == "youtube.streaming.started" && validJob(candidate)) {
		active = candidate;
		checked = true;
		recoveredThisRun = true;
		save();
	}
}

void LiveSession::checkRecovery()
{
	if (!enabled || checking || (checked && !recoveryDismissed) || active.isEmpty() ||
	    host != active["host"].toString() || key.isEmpty())
		return;
	checking = true;
	const int token = generation;
	client.request(
		controlPath(active), "GET",
		[this, token](const QJsonObject &body) {
			if (token != generation)
				return;
			checking = false;
			inspectRecovery(body, false);
		},
		[this, token](const ApiError &error) {
			if (token != generation)
				return;
			checking = false;
			emit notice("Recovery check failed: " + error.message);
		});
}

void LiveSession::inspectRecovery(const QJsonObject &body, bool accepted)
{
	const auto item = body["item"].toObject();
	const auto state = item["broadcast"].toObject()["lifeCycleStatus"].toString().toLower();
	if (QString::number(item["service"].toObject()["id"].toInteger()) != active["serviceId"].toString() ||
	    state.isEmpty()) {
		emit notice("Recovery returned an invalid service status.");
		return;
	}
	candidate = active;
	const auto error = log.streamFilename() == active["file"].toString()
				   ? QString()
				   : log.resumeStream(active["file"].toString());
	if (!error.isEmpty()) {
		emit notice(error);
		return;
	}
	if (state == "complete" || state == "revoked") {
		log.append("recovery", "already_ended", {{"serviceId", active["serviceId"]}});
		checked = true;
		emit hideRecovery();
		log.finishStream();
		return;
	}
	checked = true;
	if (!accepted) {
		if (recoveryDismissed)
			return;
		emit recoveryPrompt(active["serviceTitle"].toString("Service " + active["serviceId"].toString()));
		return;
	}
	const auto restoreError = restore ? restore(active["serviceId"].toString(), body) : "Recovery is unavailable.";
	if (!restoreError.isEmpty()) {
		emit notice(restoreError);
		emit recoveryPrompt(active["serviceTitle"].toString());
		return;
	}
	recoveryDismissed = false;
	recoveredThisRun = true;
	log.append("recovery", "service.recovered",
		   {{"serviceId", active["serviceId"]}, {"serviceTitle", active["serviceTitle"]}});
	emit notice("Service recovered; continuing the original stream log.");
	emit hideRecovery();
	save();
}

void LiveSession::recover()
{
	if (!enabled || checking || active.isEmpty() || active["host"].toString() != host)
		return;
	checking = true;
	const int token = generation;
	// Recheck after the operator accepts; a broadcast may have ended while the card was open.
	client.request(
		controlPath(active), "GET",
		[this, token](const QJsonObject &body) {
			if (token != generation)
				return;
			checking = false;
			inspectRecovery(body, true);
		},
		[this, token](const ApiError &error) {
			if (token != generation)
				return;
			checking = false;
			emit notice(error.message);
		});
}

void LiveSession::dismissRecovery()
{
	recoveryDismissed = true;
	checked = true;
	emit hideRecovery();
}
void LiveSession::retry()
{
	recoveryDismissed = false;
	checked = recoveredThisRun;
	uploadBlocked = false;
	checkRecovery();
	upload();
}

void LiveSession::completed(const QString &filename)
{
	QJsonObject job = candidate;
	if (active["file"].toString() == filename)
		job = active;
	if (!validJob(job) || job["file"].toString() != filename)
		return;
	bool duplicate = false;
	for (const auto &value : pending)
		if (value.toObject() == job)
			duplicate = true;
	if (!duplicate)
		pending.append(job);
	if (active["file"] == job["file"])
		active = {};
	candidate = {};
	recoveredThisRun = false;
	save();
	QTimer::singleShot(2000, this, [this]() { upload(); });
}

void LiveSession::uploadError(const ApiError &error)
{
	uploading = false;
	uploadBlocked = !error.retryable;
	emit notice(
		"Log upload pending: " +
		(error.status == 409
			 ? QString("Log versions changed or all three slots are full. Review Service Manager, then retry.")
			 : error.message));
}

void LiveSession::uploaded(const QJsonObject &job)
{
	for (int i = 0; i < pending.size(); ++i)
		if (pending[i].toObject() == job) {
			pending.removeAt(i);
			break;
		}
	uploading = false;
	save();
	log.append("upload", "completed", {{"serviceId", job["serviceId"]}});
	emit notice("Service log uploaded.");
	QTimer::singleShot(0, this, [this]() { upload(); });
}

void LiveSession::upload()
{
	if (!enabled || uploading || uploadBlocked || pending.isEmpty() || key.isEmpty())
		return;
	QJsonObject job;
	for (const auto &value : pending)
		if (value.toObject()["host"].toString() == host) {
			job = value.toObject();
			break;
		}
	if (job.isEmpty())
		return;
	QFile file(QDir(log.folder()).filePath(job["file"].toString()));
	if (QFileInfo(file).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 10 * 1024 * 1024 ||
	    file.size() == 0) {
		uploadError({"Log is missing, unreadable, or exceeds the API's 10 MiB limit.", false});
		return;
	}
	const auto bytes = file.readAll();
	const QString hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	uploading = true;
	const int token = generation;
	const QString path = "/api/admin/v1/services/" + job["serviceId"].toString() + "/live-logs";
	// A lost response may already have committed the upload. Compare hashes before every attempt.
	client.request(
		path, "GET",
		[this, job, bytes, hash, path, token](const QJsonObject &body) {
			if (token != generation)
				return;
			if (!body["items"].isArray()) {
				uploadError({"Invalid log-list response.", false});
				return;
			}
			const auto items = body["items"].toArray();
			for (const auto &value : items)
				if (value.toObject()["sha256"].toString() == hash) {
					uploaded(job);
					return;
				}
			if (items.size() >= 3) {
				uploadError(
					{"Three log versions already exist. Manage versions in Service Manager, then retry.",
					 false});
				return;
			}
			QUrlQuery query;
			query.addQueryItem("filename", job["file"].toString());
			client.request(
				path + "?" + query.toString(QUrl::FullyEncoded), "POST",
				[this, job, hash, token](const QJsonObject &result) {
					if (token != generation)
						return;
					if (result["item"].toObject()["sha256"].toString() != hash) {
						uploadError({"Upload acknowledgement did not match the log."});
						return;
					}
					uploaded(job);
				},
				[this, token](const ApiError &error) {
					if (token == generation)
						uploadError(error);
				},
				bytes, "application/x-ndjson");
		},
		[this, token](const ApiError &error) {
			if (token == generation)
				uploadError(error);
		});
}
