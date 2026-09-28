#include "event-log.h"
#include "openlp-monitor.h"
#include "companion-monitor.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QEventLoop>
#include <QDebug>
#include <stdexcept>
#include <cstdio>
#include <QTcpServer>
#include <QCryptographicHash>

static void websocketFrames()
{
	QTcpServer server;
	if (!server.listen(QHostAddress::LocalHost))
		throw std::runtime_error("mock listener");
	QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
		auto *socket = server.nextPendingConnection();
		QObject::connect(
			socket, &QTcpSocket::readyRead, socket,
			[socket, input = QByteArray(), upgraded = false]() mutable {
				input += socket->readAll();
				if (upgraded || !input.contains("\r\n\r\n"))
					return;
				QByteArray nonce;
				for (const auto &line : input.split('\n'))
					if (line.startsWith("Sec-WebSocket-Key:"))
						nonce = line.mid(18).trimmed();
				const auto accept =
					QCryptographicHash::hash(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
								 QCryptographicHash::Sha1)
						.toBase64();
				socket->write(
					"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
					accept + "\r\n\r\n");
				socket->write(
					QByteArray::fromHex("01017b80017d82027b7d")); // fragmented text {}, binary {}
				upgraded = true;
			});
	});
	LocalWebSocket client;
	int received = 0;
	QEventLoop loop;
	QObject::connect(&client, &LocalWebSocket::message, &loop, [&](const QByteArray &data) {
		if (data == "{}")
			++received;
		if (received == 2)
			loop.quit();
	});
	QTimer::singleShot(2000, &loop, &QEventLoop::quit);
	client.open(QUrl(QString("ws://127.0.0.1:%1/").arg(server.serverPort())));
	loop.exec();
	if (received != 2)
		throw std::runtime_error("WebSocket fragmented text and OpenLP binary JSON");
}

static void check(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	try {
		websocketFrames();
		QTemporaryDir dir;
		check(dir.isValid(), "temporary directory");
		EventLog log(dir.path());
		const auto old = dir.filePath("streamingqa-2000-01-01.jsonl");
		const auto unrelated = dir.filePath("other-2000-01-01.jsonl");
		for (const auto &path : {old, unrelated}) {
			QFile f(path);
			check(f.open(QIODevice::WriteOnly), "fixture");
			f.write("test");
		}
		log.configure(90);
		check(!QFile::exists(old) && QFile::exists(unrelated), "retention must only delete owned expired logs");
		log.append("openlp", "slide.changed",
			   {{"serviceItem", "Test song"}, {"slide", 8}, {"apiKey", "secret"}});
		QFile file(dir.filePath("streamingqa-" + QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate) +
					".jsonl"));
		check(file.open(QIODevice::ReadOnly), "persisted log");
		const auto bytes = file.readAll();
		const auto record = QJsonDocument::fromJson(bytes).object();
		check(!bytes.contains("secret") && record["details"].toObject()["serviceItem"] == "Test song" &&
			      record["details"].toObject()["slide"] == 8,
		      "title and slide persistence/redaction");
		check(log.recent().last().endsWith("OpenLP: Test song | Slide 8") && !log.recent().last().contains('{'),
		      "brief readable viewer line");
		check(!record.contains("session") && !record.contains("sequence") &&
			      !record["details"].toObject().contains("apiKey"),
		      "minimal saved record");
		const int before = log.recent().size();
		log.append("live-control", "youtube.status", {{"status", "ready"}});
		log.append("openlp", "connection.state", {{"state", "Connecting"}});
		check(log.recent().size() == before, "internal chatter omitted");
		log.append("openlp", "connection.state", {{"state", "Disconnected; reconnecting"}});
		log.append("openlp", "connection.state", {{"state", "Disconnected: socket closed"}});
		check(log.recent().size() == before + 1, "duplicate disconnection omitted");
		log.append("openlp", "connection.state", {{"state", "Connected (OpenLP 3.0)"}});
		check(log.recent().last().endsWith("OpenLP: Connected"), "recovery retained");
		check(OpenLpMonitor::slideState({{"results", QJsonObject{{"item", "id"}, {"slide", 7}}}})["slide"] == 8,
		      "OpenLP one-based slide");
		check(OpenLpMonitor::slideState({{"results", QJsonObject{{"slide", "bad"}}}}).isEmpty(),
		      "invalid OpenLP state");
		check(CompanionMonitor::keyPress({{"message", "Button 1/2/3 pressed"}})["column"] == 3,
		      "Companion press");
		check(CompanionMonitor::keyPress({{"message", "Button 1/2/3 released"}}).isEmpty(), "ignore release");
		check(CompanionMonitor::keyPress({{"message", "Got HTTP control press 1/2/3 - bank"}})["row"] == 2,
		      "Companion HTTP press");
		if (app.arguments().contains("--live")) {
			OpenLpMonitor openlp;
			QObject::connect(&openlp, &OpenLpMonitor::statusChanged, &app,
					 [](const QString &s) { fprintf(stderr, "OpenLP: %s\n", qPrintable(s)); });
			CompanionMonitor companion;
			bool slide = false, connected = false;
			QObject::connect(&openlp, &OpenLpMonitor::event, &app,
					 [&](const QString &, const QJsonObject &details) {
						 slide = details["serviceItem"].toString() != "(unavailable)" &&
							 details["slide"].isDouble();
					 });
			QObject::connect(&companion, &CompanionMonitor::statusChanged, &app, [&](const QString &state) {
				connected = state.startsWith("Connected");
				fprintf(stderr, "Companion: %s\n", qPrintable(state));
			});
			openlp.configure("localhost", 4316, "v3", true);
			companion.configure("localhost", 8000, true);
			QEventLoop wait;
			QTimer::singleShot(6000, &wait, &QEventLoop::quit);
			wait.exec();
			check(slide, "live OpenLP title/slide event");
			check(connected, "live Companion logs subscription");
		}
		qInfo() << "PASS: event persistence, retention, redaction, OpenLP slide numbering and Companion press parsing";
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
