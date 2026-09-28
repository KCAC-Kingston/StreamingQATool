#include "openlp-monitor.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QJsonArray>
#include <QNetworkReply>

OpenLpMonitor::OpenLpMonitor(QObject *parent) : QObject(parent)
{
	timer.setSingleShot(true);
	connect(&timer, &QTimer::timeout, this, &OpenLpMonitor::fetch);
	connect(&websocket, &LocalWebSocket::opened, this, [this]() { report("Connected (OpenLP 3.0)"); });
	connect(&websocket, &LocalWebSocket::message, this, [this](const QByteArray &bytes) {
		QJsonParseError error;
		const auto json = QJsonDocument::fromJson(bytes, &error);
		if (error.error != QJsonParseError::NoError || !json.isObject()) {
			report("Invalid OpenLP WebSocket response");
			return;
		}
		process(json.object());
	});
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

void OpenLpMonitor::report(const QString &value)
{
	if (value == state)
		return;
	state = value;
	emit statusChanged(value);
}

void OpenLpMonitor::retry()
{
	if (enabled && !timer.isActive())
		timer.start(5000);
}

void OpenLpMonitor::configure(const QString &host, int port, const QString &version, bool on)
{
	if (hostname == host && httpPort == port && protocol == version && enabled == on)
		return;
	enabled = false;
	timer.stop();
	websocket.close();
	++generation;
	busy = false;
	previous = {};
	titles.clear();
	pending.clear();
	titlesBusy = false;
	hostname = host.trimmed();
	httpPort = port;
	protocol = version;
	enabled = on && !hostname.isEmpty();
	if (!enabled) {
		report("Disabled");
		return;
	}
	report("Connecting");
	fetch();
}

QJsonObject OpenLpMonitor::slideState(const QJsonObject &payload)
{
	const auto results = payload["results"].toObject();
	if (!results["slide"].isDouble() || !results["item"].isString())
		return {};
	const double raw = results["slide"].toDouble();
	if (raw < -1 || raw > 100000 || raw != int(raw))
		return {};
	return {{"serviceItemId", results["item"]}, {"slide", int(raw) + 1}};
}

void OpenLpMonitor::process(const QJsonObject &payload)
{
	const auto current = slideState(payload);
	if (current.isEmpty()) {
		report("OpenLP response has no valid slide state");
		return;
	}
	report(protocol == "v3" ? "Connected (OpenLP 3.0)" : "Connected (OpenLP 2.4)");
	if (current == previous)
		return;
	auto details = current;
	details.insert("version", protocol);
	details.insert("observedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	if (!previous.isEmpty())
		details.insert("previousSlide", previous["slide"]);
	details.insert("event", previous.isEmpty() ? "slide.snapshot" : "slide.changed");
	pending.append(details);
	previous = current;
	if (!titlesBusy && titles.contains(current["serviceItemId"].toString()))
		flushPending();
	else
		fetchTitles();
}

void OpenLpMonitor::flushPending()
{
	for (auto details : pending) {
		const QString name = details.take("event").toString();
		const QString id = details["serviceItemId"].toString();
		details.insert("serviceItem", titles.value(id, "(unavailable)"));
		emit event(name, details);
	}
	pending.clear();
}

void OpenLpMonitor::fetchTitles()
{
	if (titlesBusy || !enabled)
		return;
	titlesBusy = true;
	QUrl url;
	url.setScheme("http");
	url.setHost(hostname);
	url.setPort(httpPort);
	url.setPath(protocol == "v3" ? "/api/v2/service/items" : "/api/service/list");
	QNetworkRequest request(url);
	request.setTransferTimeout(5000);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	auto *reply = network.get(request);
	const int token = generation;
	QTimer::singleShot(5000, reply, [reply]() {
		if (!reply->isFinished())
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, token]() {
		reply->deleteLater();
		if (token != generation)
			return;
		titlesBusy = false;
		QJsonParseError error;
		const auto json = QJsonDocument::fromJson(reply->readAll(), &error);
		const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() == QNetworkReply::NoError && code == 200 &&
		    error.error == QJsonParseError::NoError) {
			const auto items = protocol == "v3" ? json.array()
							    : json.object()["results"].toObject()["items"].toArray();
			for (const auto &value : items) {
				const auto item = value.toObject();
				const QString id = item["id"].toString();
				if (!id.isEmpty())
					titles.insert(id, item["title"].toString());
			}
		} else {
			report("OpenLP slide tracking active; service-item title lookup failed");
		}
		flushPending();
	});
}

void OpenLpMonitor::fetch()
{
	if (!enabled || busy)
		return;
	busy = true;
	QUrl url;
	url.setScheme("http");
	url.setHost(hostname);
	url.setPort(httpPort);
	url.setPath(protocol == "v3" ? "/api/v2/core/system" : "/api/poll");
	QNetworkRequest request(url);
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(5000);
	auto *reply = network.get(request);
	const int token = generation;
	QTimer::singleShot(5000, reply, [reply]() {
		if (!reply->isFinished())
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, token]() {
		reply->deleteLater();
		if (token != generation)
			return;
		busy = false;
		const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		QJsonParseError error;
		const auto json = QJsonDocument::fromJson(reply->readAll(), &error);
		if (reply->error() != QNetworkReply::NoError || code != 200 ||
		    error.error != QJsonParseError::NoError || !json.isObject()) {
			report(QString("OpenLP unavailable or invalid response (HTTP %1)").arg(code));
			retry();
			return;
		}
		if (protocol == "v3") {
			const int port = json.object()["websocket_port"].toInt();
			if (port < 1 || port > 65535) {
				report("OpenLP did not advertise a WebSocket port");
				retry();
				return;
			}
			QUrl ws;
			ws.setScheme("ws");
			ws.setHost(hostname);
			ws.setPort(port);
			ws.setPath("/");
			websocket.open(ws);
		} else {
			process(json.object());
			timer.start(500);
		}
	});
}

OpenLpMonitor::~OpenLpMonitor()
{
	enabled = false;
	websocket.disconnect(this);
	websocket.close();
}
