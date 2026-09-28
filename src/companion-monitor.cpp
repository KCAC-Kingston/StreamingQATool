#include "companion-monitor.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

CompanionMonitor::CompanionMonitor(QObject *parent) : QObject(parent)
{
	reconnect.setSingleShot(true);
	connect(&reconnect, &QTimer::timeout, this, &CompanionMonitor::connectStream);
	connect(&websocket, &LocalWebSocket::opened, this, [this]() {
		initialHistory = true;
		websocket.sendText(R"({"id":1,"method":"subscription","params":{"path":"logs.watch"}})");
	});
	connect(&websocket, &LocalWebSocket::message, this, &CompanionMonitor::consume);
	connect(&websocket, &LocalWebSocket::error, this, [this](const QString &why) {
		report("Disconnected: " + why);
		retry();
	});
	connect(&websocket, &LocalWebSocket::disconnected, this, [this]() {
		if (enabled) {
			report("Disconnected; reconnecting");
			retry();
		}
	});
}

void CompanionMonitor::report(const QString &value)
{
	if (state == value)
		return;
	state = value;
	emit statusChanged(value);
}

void CompanionMonitor::retry()
{
	if (enabled && !reconnect.isActive())
		reconnect.start(5000);
}

void CompanionMonitor::configure(const QString &host, int port, bool on)
{
	if (hostname == host && wsPort == port && enabled == on)
		return;
	enabled = false;
	reconnect.stop();
	websocket.close();
	hostname = host.trimmed();
	wsPort = port;
	seen.clear();
	enabled = on && !hostname.isEmpty();
	if (!enabled) {
		report("Disabled");
		return;
	}
	report("Connecting");
	connectStream();
}

void CompanionMonitor::connectStream()
{
	if (!enabled)
		return;
	QUrl url;
	url.setScheme("ws");
	url.setHost(hostname);
	url.setPort(wsPort);
	url.setPath("/trpc");
	websocket.open(url);
}

QJsonObject CompanionMonitor::keyPress(const QJsonObject &line)
{
	const QString text = line["message"].toString();
	const QString source = line["source"].toString();
	static const QRegularExpression surface("^Button (\\d+)/(\\d+)/(\\d+) pressed$");
	static const QRegularExpression api("^Got (?:HTTP|OSC) control press (\\d+)/(\\d+)/(\\d+)(?: - .*|)$");
	auto match = surface.match(text);
	if (!match.hasMatch())
		match = api.match(text);
	if (!match.hasMatch())
		return {};
	return {{"companionTimestamp", line["time"]},
		{"source", source},
		{"page", match.captured(1).toInt()},
		{"row", match.captured(2).toInt()},
		{"column", match.captured(3).toInt()}};
}

void CompanionMonitor::consume(const QByteArray &bytes)
{
	if (bytes == "PING") {
		websocket.sendText("PONG");
		return;
	}
	QJsonParseError error;
	const auto json = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError) {
		report("Invalid Companion log response");
		return;
	}
	const QJsonArray messages = json.isArray() ? json.array() : QJsonArray{json.object()};
	for (const auto &value : messages) {
		const auto envelope = value.toObject();
		if (envelope["method"].toString() == "reconnect") {
			websocket.close();
			retry();
			continue;
		}
		if (envelope["error"].isObject()) {
			report("Companion denied or does not support logs.watch");
			continue;
		}
		if (envelope["id"].toInt() != 1)
			continue;
		const auto result = envelope["result"].toObject();
		if (result["type"].toString() == "started") {
			report("Connected (Companion logs)");
			continue;
		}
		if (result["type"].toString() != "data")
			continue;
		const auto data = result["data"].toObject();
		if (data["type"].toString() == "clear") {
			seen.clear();
			continue;
		}
		if (data["type"].toString() != "lines" || !data["lines"].isArray())
			continue;
		// The first batch is stored history, not newly pressed buttons.
		if (initialHistory) {
			initialHistory = false;
			continue;
		}
		for (const auto &item : data["lines"].toArray()) {
			const auto details = keyPress(item.toObject());
			if (details.isEmpty())
				continue;
			const auto key = QJsonDocument(details).toJson(QJsonDocument::Compact);
			if (seen.contains(key))
				continue;
			if (seen.size() >= 2000)
				seen.clear();
			seen.insert(key);
			emit event("key.pressed", details);
		}
	}
}

CompanionMonitor::~CompanionMonitor()
{
	enabled = false;
	websocket.disconnect(this);
	websocket.close();
}
