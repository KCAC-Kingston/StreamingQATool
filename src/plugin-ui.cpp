#include "plugin-ui.h"
#include "live-control.h"
#include "settings.h"
#include "event-log.h"
#include "log-window.h"
#include "marker-monitor.h"
#include "camera-assist.h"
#include "openlp-monitor.h"
#include "companion-monitor.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>
#include "plugin-support.h"
#include <QDockWidget>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <type_traits>

namespace {
constexpr const char *dock_id = "StreamingQATool.Panel";
SettingsStore settings;
QPointer<QWidget> panel;
QPointer<SettingsDialog> dialog;
QPointer<LiveControl> live_control;
QPointer<EventLog> event_log;
QPointer<LogWindow> log_window;
QPointer<MarkerMonitor> marker_monitor;
QPointer<CameraAssist> camera_assist;
QPointer<OpenLpMonitor> openlp;
QPointer<CompanionMonitor> companion;
QJsonObject applied;
bool callback_registered = false;

QString current_scene_name()
{
	auto *scene = obs_frontend_get_current_scene();
	const QString name = scene ? QString::fromUtf8(obs_source_get_name(scene)) : QString();
	if (scene)
		obs_source_release(scene);
	return name;
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		delete marker_monitor.data();
		return;
	}
	QString name;
	QJsonObject details;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED)
		name = "streaming.started";
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		name = "streaming.ended";
	else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		name = "scene.changed";
		if (camera_assist)
			QMetaObject::invokeMethod(
				camera_assist,
				[]() {
					if (camera_assist)
						camera_assist->sceneChanged(current_scene_name());
				},
				Qt::QueuedConnection);
		auto *scene = obs_frontend_get_current_scene();
		if (scene) {
			details.insert("scene", QString::fromUtf8(obs_source_get_name(scene)));
			obs_source_release(scene);
		}
	} else
		return;
	const QPointer<EventLog> target = event_log;
	if (target)
		QMetaObject::invokeMethod(
			target,
			[target, name, details]() {
				if (target)
					target->append("obs", name, details);
			},
			Qt::QueuedConnection);
}

void apply_settings()
{
	if (!live_control)
		return;
	const auto &values = settings.values();
	live_control->setServiceManagerEnabled(values["serviceManagerEnabled"].toBool(true));
	if (values["serviceManagerEnabled"].toBool(true) &&
	    (applied.isEmpty() || applied["serviceManagerEnabled"] != values["serviceManagerEnabled"] ||
	     applied["serviceManagerHost"] != values["serviceManagerHost"] ||
	     applied["serviceManagerApiKey"] != values["serviceManagerApiKey"] ||
	     applied["endStreamingDelay"] != values["endStreamingDelay"]))
		live_control->configure(values["serviceManagerHost"].toString(),
					values["serviceManagerApiKey"].toString(),
					values["endStreamingDelay"].toString().toInt());
	event_log->configure(values["logRetentionDays"].toString().toInt());
	openlp->configure(values["openlpHost"].toString(), values["openlpPort"].toString().toInt(),
			  values["openlpVersion"].toString(), values["openlpEnabled"].toBool(true));
	companion->configure(values["companionHost"].toString(), values["companionPort"].toString().toInt(),
			     values["companionEnabled"].toBool(true));
	if (camera_assist)
		camera_assist->configure(values["cameraAssistEnabled"].toBool(),
					 values["cameraAssistSlides"].toString(),
					 values["cameraAssistCamera"].toString(),
					 values["cameraAssistDelay"].toInt(15));
	if (camera_assist)
		camera_assist->sceneChanged(current_scene_name());
	applied = values;
}

void show_settings()
{
	if (dialog) {
		dialog->showNormal();
		dialog->raise();
		dialog->activateWindow();
		return;
	}
	QStringList scenes;
	obs_frontend_source_list list{};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i)
		scenes.append(QString::fromUtf8(obs_source_get_name(list.sources.array[i])));
	obs_frontend_source_list_free(&list);
	dialog = new SettingsDialog(
		settings,
		[](const QJsonObject &updated) -> QString {
			if (updated["serviceManagerEnabled"].toBool(true) &&
			    settings.values()["serviceManagerEnabled"].toBool(true) && live_control &&
			    live_control->sessionActive()) {
				if (ServiceManagerClient::normalizeHost(updated["serviceManagerHost"].toString()) !=
				    ServiceManagerClient::normalizeHost(
					    settings.values()["serviceManagerHost"].toString()))
					return "Finish the active session before changing the Service Manager host. You can update the API key now.";
				if (updated["serviceManagerApiKey"].toString().isEmpty())
					return "An API key is required to recover the active session.";
			}
			return {};
		},
		[]() {
			apply_settings();
			obs_log(LOG_INFO, "settings saved");
		},
		static_cast<QWidget *>(obs_frontend_get_main_window()), scenes);
	dialog->show();
}
} // namespace

void streaming_qa_load_settings(void)
{
	char *path = obs_module_config_path("settings.json");
	const QString error = settings.load(QString::fromUtf8(path ? path : ""));
	bfree(path);
	if (!error.isEmpty())
		obs_log(LOG_WARNING, "%s", error.toUtf8().constData());
	for (const auto &line : settings.startupLogLines())
		obs_log(LOG_INFO, "%s", line.toUtf8().constData());
}

void streaming_qa_create_panel(void)
{
	if (panel)
		return;
	auto *widget = new QWidget;
	panel = widget;
	auto *layout = new QVBoxLayout(widget);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);
	auto *header = new QHBoxLayout;
	auto *title = new QLabel("StreamingQATool", widget);
	auto font = title->font();
	font.setBold(true);
	title->setFont(font);
	header->addWidget(title, 1);
	layout->addLayout(header);
	EncoderControl encoder;
	encoder.active = []() {
		return obs_frontend_streaming_active();
	};
	encoder.matches = [](const QString &server, const QString &key) {
		auto *service = obs_frontend_get_streaming_service();
		if (!service || server.isEmpty() || key.isEmpty())
			return false;
		auto *data = obs_service_get_settings(service);
		const bool matches = server == QString::fromUtf8(obs_data_get_string(data, "server")) &&
				     key == QString::fromUtf8(obs_data_get_string(data, "key"));
		obs_data_release(data);
		return matches;
	};
	encoder.start = [](const QString &server, const QString &key) -> QString {
		if (obs_frontend_streaming_active())
			return "OBS is already streaming. Stop the current stream before Prestart.";
		auto *data = obs_data_create();
		obs_data_set_string(data, "server", server.toUtf8().constData());
		obs_data_set_string(data, "key", key.toUtf8().constData());
		obs_data_set_bool(data, "use_auth", false);
		auto *service = obs_service_create("rtmp_custom", "StreamingQATool", data, nullptr);
		obs_data_release(data);
		if (!service)
			return "Could not create the OBS RTMP service.";
		obs_frontend_set_streaming_service(service);
		obs_service_release(service);
		obs_frontend_save_streaming_service();
		obs_frontend_streaming_start();
		return {};
	};
	encoder.stop = []() {
		obs_frontend_streaming_stop();
	};
	live_control = new LiveControl(std::move(encoder), widget);
	layout->addWidget(live_control);
	event_log = new EventLog(settings.logFolder(), widget);
	openlp = new OpenLpMonitor(widget);
	companion = new CompanionMonitor(widget);
	live_control->eventSink = [](const QString &name, const QJsonObject &details) {
		if (event_log)
			event_log->append("live-control", name, details);
	};
	auto *storage = new QLabel(widget);
	storage->setWordWrap(true);
	storage->setTextFormat(Qt::PlainText);
	layout->addWidget(storage);
	storage->hide();
	QObject::connect(event_log, &EventLog::storageError, storage, [storage](const QString &error) {
		storage->setText(error);
		storage->setVisible(!error.isEmpty());
	});
	auto *connections = new QHBoxLayout;
	layout->addLayout(connections);
	auto wire = [widget, connections](auto *monitor, const QString &source) {
		auto *row = new QHBoxLayout;
		auto *dot = new QLabel(widget);
		dot->setFixedSize(8, 8);
		dot->setStyleSheet("background-color: #dc3545; border-radius: 4px;");
		dot->setAccessibleName(source + " disconnected");
		row->addWidget(dot);
		auto *label = new QLabel(widget);
		label->setWordWrap(true);
		label->setTextFormat(Qt::PlainText);
		row->addWidget(label, 1);
		connections->addLayout(row, 1);
		using Monitor = std::remove_pointer_t<decltype(monitor)>;
		QObject::connect(monitor, &Monitor::event, widget,
				 [source](const QString &name, const QJsonObject &details) {
					 if (event_log)
						 event_log->append(source, name, details);
				 });
		QObject::connect(monitor, &Monitor::statusChanged, label, [label, dot, source](const QString &state) {
			const bool connected = state.startsWith("Connected");
			dot->setStyleSheet(QString("background-color: %1; border-radius: 4px;")
						   .arg(connected ? "#28a745" : "#dc3545"));
			dot->setAccessibleName(source + (connected ? " connected" : " disconnected"));
			dot->setToolTip(state);
			label->setText(source == "openlp" ? "OpenLP" : "Companion");
			label->setToolTip(state);
			label->setAccessibleName(source + ": " + state);
			if (event_log)
				event_log->append(source, "connection.state", {{"state", state}});
		});
	};
	wire(openlp.data(), "openlp");
	wire(companion.data(), "companion");
	auto *logs = new QToolButton(widget);
	logs->setText(QStringLiteral("\u2637"));
	logs->setFixedSize(26, 26);
	logs->setToolTip("Open Log Stream");
	logs->setAccessibleName("Open Log Stream");
	header->addWidget(logs);
	QObject::connect(logs, &QToolButton::clicked, widget, []() {
		if (!log_window)
			log_window = new LogWindow(*event_log, panel);
		log_window->showNormal();
		log_window->raise();
		log_window->activateWindow();
	});
	camera_assist = new CameraAssist(
		[](const QString &name) -> QString {
			const auto current = current_scene_name();
			if (!settings.values()["cameraAssistEnabled"].toBool() ||
			    (current != settings.values()["cameraAssistSlides"].toString() &&
			     current != settings.values()["cameraAssistCamera"].toString()))
				return "Camera Assist is inactive in this scene.";
			auto *scene = obs_get_source_by_name(name.toUtf8().constData());
			if (!scene || !obs_source_is_scene(scene)) {
				if (scene)
					obs_source_release(scene);
				return "Scene unavailable: " + name;
			}
			obs_frontend_set_current_scene(scene);
			obs_source_release(scene);
			return {};
		},
		widget);
	camera_assist->eventSink = [](const QString &name, const QJsonObject &data) {
		if (event_log)
			event_log->append("camera-assist", name, data);
	};
	event_log->append("application", "started");
	apply_settings();
	auto *button = new QToolButton(widget);
	button->setText(QStringLiteral("\u2699"));
	button->setFixedSize(26, 26);
	button->setToolTip("Settings");
	button->setAccessibleName("Settings");
	QObject::connect(button, &QToolButton::clicked, widget, show_settings);
	header->addWidget(button);
	if (!obs_frontend_add_dock_by_id(dock_id, "StreamingQATool", widget)) {
		obs_log(LOG_ERROR, "Could not register StreamingQATool dock");
		delete widget;
		return;
	}
	marker_monitor = new MarkerMonitor(widget);
	auto *markerStatus = new QLabel("Marker reader: starting", widget);
	markerStatus->setWordWrap(true);
	markerStatus->setTextFormat(Qt::PlainText);
	layout->addWidget(markerStatus);
	QObject::connect(marker_monitor, &MarkerMonitor::statusChanged, markerStatus,
			 [markerStatus](const QString &state) {
				 markerStatus->setToolTip(state);
				 markerStatus->setText(state);
				 markerStatus->setVisible(state != "Marker reader: watching program output");
			 });
	auto *markerStats = new QLabel("Marker: None\nAppear: 0 | Disappear: 0 | Reads: 0/0", widget);
	markerStats->setObjectName("markerStats");
	markerStats->setTextFormat(Qt::PlainText);
	markerStats->setWordWrap(true);
	markerStats->setToolTip(
		"Counts since OBS started. A valid reading passes marker checksum validation; appearance requires three matching readings.");
	layout->addWidget(markerStats);
	QObject::connect(marker_monitor, &MarkerMonitor::statisticsChanged, markerStats,
			 [markerStats](const QJsonObject &stats) {
				 const auto marker = stats["marker"].toObject();
				 QString current = marker.isEmpty() ? "None" : marker["section"].toString();
				 if (marker.contains("serviceDate"))
					 current += " | " + marker["serviceDate"].toString() + " " +
						    marker["serviceKind"].toString();
				 markerStats->setToolTip("Last seen: " +
							 (stats["lastSeen"].toString().isEmpty()
								  ? "Never"
								  : stats["lastSeen"].toString()) +
							 "\nCounts since OBS started. Reads = valid / sampled frames.");
				 markerStats->setText(QString("Marker: %1\nAppear: %2 | Disappear: %3 | Reads: %4/%5")
							      .arg(current)
							      .arg(stats["appearances"].toInteger())
							      .arg(stats["disappearances"].toInteger())
							      .arg(stats["valid"].toInteger())
							      .arg(stats["samples"].toInteger()));
			 });
	QObject::connect(marker_monitor, &MarkerMonitor::event, widget,
			 [](const QString &name, const QJsonObject &data, const QDateTime &time) {
				 if (event_log)
					 event_log->append("marker", name, data, time);
				 if (camera_assist) {
					 camera_assist->sceneChanged(current_scene_name());
					 camera_assist->markerEvent(name, data);
				 }
			 });
	layout->addStretch();
	obs_frontend_add_event_callback(frontend_event, nullptr);
	callback_registered = true;
	if (auto *dock = qobject_cast<QDockWidget *>(widget->parentWidget())) {
		dock->setAllowedAreas(Qt::AllDockWidgetAreas);
		// OBS creates plugin docks floating and hidden. Start this panel docked;
		// OBS can subsequently restore the user's saved layout.
		dock->setFloating(false);
		dock->show();
	}
}

void streaming_qa_destroy_panel(void)
{
	if (callback_registered)
		obs_frontend_remove_event_callback(frontend_event, nullptr);
	callback_registered = false;
	delete marker_monitor.data();
	delete log_window.data();
	delete dialog.data();
	if (panel)
		obs_frontend_remove_dock(dock_id);
}
