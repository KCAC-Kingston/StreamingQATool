#include "live-session.h"
#include "event-log.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why)
{
	if (!ok)
		throw std::runtime_error(why);
}
static void until(std::function<bool()> predicate, int timeout = 6000)
{
	QElapsedTimer timer;
	timer.start();
	while (!predicate() && timer.elapsed() < timeout) {
		QCoreApplication::processEvents();
		QThread::msleep(5);
	}
	check(predicate(), "Timed out");
}
struct Api : QTcpServer {
	QString state = "live", hash;
	QByteArray uploaded;
	bool deny = false, loseResponse = false, authenticated = true, contentType = true;
	int posts = 0, gets = 0;
	Api()
	{
		check(listen(QHostAddress::LocalHost), "listen");
		connect(this, &QTcpServer::newConnection, this, [this]() {
			while (hasPendingConnections()) {
				auto *socket = nextPendingConnection();
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
					auto bytes = socket->property("bytes").toByteArray() + socket->readAll();
					socket->setProperty("bytes", bytes);
					const int boundary = bytes.indexOf("\r\n\r\n");
					if (boundary < 0 || socket->property("done").toBool())
						return;
					int length = 0;
					for (const auto &line : bytes.left(boundary).split('\n'))
						if (line.toLower().startsWith("content-length:"))
							length = line.mid(15).trimmed().toInt();
					if (bytes.size() < boundary + 4 + length)
						return;
					socket->setProperty("done", true);
					authenticated &= bytes.left(boundary).toLower().contains("x-api-key: test-key");
					const auto path = bytes.split(' ').value(1);
					QJsonObject result;
					int code = deny ? 403 : 200;
					if (path.contains("live-logs")) {
						if (bytes.startsWith("POST")) {
							++posts;
							uploaded = bytes.mid(boundary + 4, length);
							contentType &=
								bytes.left(boundary).contains("application/x-ndjson");
							hash = QString::fromLatin1(
								QCryptographicHash::hash(uploaded,
											 QCryptographicHash::Sha256)
									.toHex());
							if (loseResponse) {
								loseResponse = false;
								socket->abort();
								return;
							}
							result = {{"item", QJsonObject{{"sha256", hash}}}};
							code = 201;
						} else
							result = {{"items", hash.isEmpty()
										    ? QJsonArray()
										    : QJsonArray{QJsonObject{
											      {"sha256", hash}}}}};
					} else {
						++gets;
						result = {{"item",
							   QJsonObject{{"service", QJsonObject{{"id", 1}}},
								       {"broadcast",
									QJsonObject{{"lifeCycleStatus", state}}}}}};
					}
					const auto body = QJsonDocument(result).toJson(QJsonDocument::Compact);
					socket->write(
						"HTTP/1.1 " + QByteArray::number(code) +
						" Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
						QByteArray::number(body.size()) + "\r\n\r\n" + body);
					socket->disconnectFromHost();
				});
			}
		});
	}
	QString host() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
};
static QJsonObject readState(const QString &path)
{
	QFile f(path);
	check(f.open(QIODevice::ReadOnly), "state file");
	return QJsonDocument::fromJson(f.readAll()).object();
}
static QString seed(const QString &dir, const QString &path, Api &api)
{
	EventLog log(dir);
	LiveSession session(log, path);
	session.configure(true, api.host(), "test-key");
	const QJsonObject service{{"serviceId", "1"}, {"serviceTitle", "Sunday"}};
	log.append("live-control", "prestart.requested", service);
	session.observe("prestart.requested", service);
	const auto file = log.streamFilename();
	check(!QFile::exists(path), "Prestart alone must not persist live recovery");
	log.append("obs", "streaming.started");
	log.append("live-control", "youtube.streaming.started", service);
	session.observe("youtube.streaming.started", service);
	check(readState(path)["active"].toObject()["file"] == file, "persist live service and log");
	return file;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	try {
		{
			Api api;
			QTemporaryDir temp;
			const auto path = temp.filePath("state.json");
			EventLog log(temp.filePath("logs"));
			LiveSession session(log, path);
			session.configure(true, api.host(), "test-key");
			const QJsonObject service{{"serviceId", "1"}, {"serviceTitle", "Already live"}};
			log.append("live-control", "youtube.streaming.started", service);
			session.observe("youtube.streaming.started", service);
			check(!log.streamFilename().isEmpty() &&
				      readState(path)["active"].toObject()["serviceId"] == "1",
			      "Observed live service arms recovery without local Prestart");
		}
		for (bool alreadyEnded : {false, true}) {
			Api api;
			QTemporaryDir temp;
			const auto dir = temp.filePath("logs"), path = temp.filePath("session.json");
			const QString filename = seed(dir, path, api);
			if (alreadyEnded)
				api.state = "complete";
			EventLog log(dir);
			LiveSession session(log, path);
			int prompts = 0, restores = 0;
			QString notice;
			QObject::connect(&session, &LiveSession::recoveryPrompt, &session,
					 [&](const QString &) { ++prompts; });
			QObject::connect(&session, &LiveSession::notice, &session,
					 [&](const QString &value) { notice = value; });
			session.restore = [&](const QString &id, const QJsonObject &) {
				check(id == "1", "correct service restored");
				++restores;
				return QString();
			};
			api.deny = true;
			session.configure(true, api.host(), "test-key");
			until([&]() { return notice.contains("failed"); });
			check(prompts == 0 && restores == 0 && !readState(path)["active"].toObject().isEmpty(),
			      "auth failure preserves recovery without starting OBS");
			api.deny = false;
			session.retry();
			if (!alreadyEnded) {
				until([&]() { return prompts == 1; });
				check(restores == 0 && log.streamFilename() == filename,
				      "same file, explicit recovery approval");
				session.recover();
				until([&]() { return restores == 1; });
				log.append("obs", "rtmp.disconnected", {{"reason", "Reconnecting"}});
				api.loseResponse = true;
				log.append("live-control", "youtube.streaming.ended");
				log.append("obs", "streaming.ended");
				until([&]() { return notice.startsWith("Log upload pending"); });
				check(api.posts == 1 && !readState(path)["pending"].toArray().isEmpty(),
				      "uncertain upload remains queued");
				session.retry();
			}
			until([&]() {
				return readState(path)["pending"].toArray().isEmpty() &&
				       readState(path)["active"].toObject().isEmpty();
			});
			check(api.posts == 1 && api.authenticated && api.contentType,
			      "one authenticated raw JSONL upload");
			check(api.uploaded.contains("prestart.requested") &&
				      api.uploaded.contains(alreadyEnded ? "already_ended" : "service.recovered"),
			      "whole original log with recovery marker");
			if (alreadyEnded)
				check(restores == 0 && prompts == 0,
				      "ended service is uploaded without recovery prompt");
			else
				check(api.uploaded.contains("rtmp.disconnected"), "disconnect is in stream log");
		}
		fprintf(stdout,
			"PASS: persisted live recovery, auth failure, ended service, original log, raw upload and dedup after lost response\n");
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
