#include "rtmp-monitor.h"
#include <obs-frontend-api.h>

RtmpMonitor::RtmpMonitor(QObject *parent) : QObject(parent)
{
	timer.setInterval(500);
	connect(&timer, &QTimer::timeout, this, &RtmpMonitor::refresh);
}
void RtmpMonitor::start()
{
	// The frontend output handler does not exist during module post-load.
	// Only begin inspecting it after OBS_FRONTEND_EVENT_FINISHED_LOADING.
	timer.start();
	refresh();
}
RtmpMonitor::~RtmpMonitor()
{
	detach();
}
void RtmpMonitor::detach()
{
	if (!output)
		return;
	auto *handler = obs_output_get_signal_handler(output);
	signal_handler_disconnect(handler, "reconnect", reconnect, this);
	signal_handler_disconnect(handler, "reconnect_success", reconnected, this);
	signal_handler_disconnect(handler, "stop", stopped, this);
	obs_output_release(output);
	output = nullptr;
}
void RtmpMonitor::refresh()
{
	if (!timer.isActive())
		return;
	auto *current = obs_frontend_get_streaming_output();
	if (current == output) {
		if (current)
			obs_output_release(current);
		return;
	}
	detach();
	output = current;
	if (!output)
		return;
	auto *handler = obs_output_get_signal_handler(output);
	signal_handler_connect(handler, "reconnect", reconnect, this);
	signal_handler_connect(handler, "reconnect_success", reconnected, this);
	signal_handler_connect(handler, "stop", stopped, this);
}
void RtmpMonitor::post(const QString &name, const QJsonObject &details)
{
	QMetaObject::invokeMethod(this, [this, name, details]() { emit event(name, details); }, Qt::QueuedConnection);
}
void RtmpMonitor::reconnect(void *data, calldata_t *)
{
	static_cast<RtmpMonitor *>(data)->post("rtmp.disconnected", {{"reason", "Reconnecting"}});
}
void RtmpMonitor::reconnected(void *data, calldata_t *)
{
	static_cast<RtmpMonitor *>(data)->post("rtmp.reconnected");
}
void RtmpMonitor::stopped(void *data, calldata_t *params)
{
	const auto code = calldata_int(params, "code");
	if (code != OBS_OUTPUT_SUCCESS)
		static_cast<RtmpMonitor *>(data)->post("rtmp.disconnected", {{"code", qint64(code)},
									     {"reason", "Output stopped with error"}});
}
