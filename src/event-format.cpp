#include "event-format.h"
#include <QStringList>

namespace {
QString brief(QString text)
{
	text = text.simplified();
	return text.size() > 160 ? text.left(157) + "..." : text;
}
} // namespace

QJsonObject EventFormat::details(const QString &event, const QJsonObject &input)
{
	QJsonObject output;
	// Keep useful, stable fields for later database storage, without transport metadata.
	const QStringList fields{"code",      "section",      "serviceDate",  "serviceKind", "elapsedMs",
				 "serviceId", "serviceTitle", "serviceItem",  "slide",       "scene",
				 "page",      "row",          "column",       "reason",      "delaySeconds",
				 "command",   "retry",        "skippedDelay", "message"};
	for (const auto &key : fields) {
		if (!input.contains(key))
			continue;
		output.insert(key, input[key].isString() ? QJsonValue(brief(input[key].toString())) : input[key]);
	}
	if (event == "connection.state") {
		const auto state = input["state"].toString();
		output.insert("state", state.startsWith("Connected") ? "Connected"
				       : state == "Disabled"         ? "Disabled"
				       : state == "Connecting"       ? "Connecting"
								     : "Disconnected");
	}
	return output;
}

QString EventFormat::text(const QString &source, const QString &event, const QJsonObject &d)
{
	if (event == "service.recovered")
		return "Recovered service: " + d["serviceTitle"].toString() + " (#" + d["serviceId"].toString() + ")";
	if (source == "recovery")
		return "Recovery: " + event + " | Service " + d["serviceId"].toString();
	if (source == "upload")
		return "Log upload: " + event + " | Service " + d["serviceId"].toString();
	if (event == "rtmp.disconnected")
		return "RTMP disconnected: " + d["reason"].toString();
	if (event == "rtmp.reconnected")
		return "RTMP reconnected";
	if (source == "camera-assist")
		return "Camera Assist: " + event + " | " + d["scene"].toString();
	const QString label = source == "openlp"      ? "OpenLP"
			      : source == "companion" ? "Companion"
			      : source == "obs"       ? "OBS"
						      : "StreamingQATool";
	if (event == "marker.appeared" || event == "marker.disappeared") {
		QString data = d["section"].toString();
		if (d.contains("serviceDate"))
			data += " | " + d["serviceDate"].toString() + " " + d["serviceKind"].toString();
		return "Marker " + QString(event == "marker.appeared" ? "appeared: " : "disappeared: ") + data;
	}
	if (event == "connection.state")
		return label + ": " + d["state"].toString();
	if (event.startsWith("slide."))
		return "OpenLP: " + d["serviceItem"].toString() + " | Slide " + QString::number(d["slide"].toInt());
	if (event == "scene.changed")
		return "OBS: Scene " + d["scene"].toString();
	if (event == "key.pressed")
		return QString("Companion: Button %1/%2/%3 pressed")
			.arg(d["page"].toInt())
			.arg(d["row"].toInt())
			.arg(d["column"].toInt());
	if (event == "streaming.started")
		return "OBS: Streaming started";
	if (event == "streaming.ended")
		return "OBS: Streaming ended";
	if (event == "youtube.streaming.started")
		return "YouTube: Streaming started";
	if (event == "youtube.streaming.ended")
		return "YouTube: Streaming ended";
	if (event == "prestart.requested")
		return "Prestart: " + d["serviceTitle"].toString();
	if (event == "prestart.cancelled")
		return "Prestart cancelled: " + d["reason"].toString();
	if (event == "youtube.start.requested")
		return "YouTube: Start requested";
	if (event == "youtube.end.requested")
		return QString("YouTube: End requested (%1s delay)").arg(d["delaySeconds"].toInt());
	if (event == "youtube.end.command_requested")
		return d["skippedDelay"].toBool() ? "YouTube: Ending now (delay skipped)" : "YouTube: Ending now";
	if (event == "obs.stop_override.requested")
		return "OBS: Stop override requested";
	if (event == "youtube.command.retry")
		return QString("YouTube: Retry %1 (%2/3)").arg(d["command"].toString()).arg(d["retry"].toInt());
	if (event == "control.error")
		return "Error: " + d["message"].toString();
	if (event == "stream.interrupted")
		return "Stream log closed: " + d["reason"].toString();
	if (event == "ended")
		return "StreamingQATool closed";
	if (event == "started")
		return "StreamingQATool started";
	return label + ": " + event;
}
