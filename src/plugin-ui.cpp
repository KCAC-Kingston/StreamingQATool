#include "plugin-ui.h"
#include "live-control.h"
#include "settings.h"
#include "event-log.h"
#include "log-window.h"
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
QPointer<OpenLpMonitor> openlp;
QPointer<CompanionMonitor> companion;
QJsonObject applied;
bool callback_registered = false;

void frontend_event(enum obs_frontend_event event, void *)
{
	QString name;
	QJsonObject details;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED)
		name = "streaming.started";
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		name = "streaming.ended";
	else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED) {
		name = "scene.changed";
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
	if (applied.isEmpty() || applied["serviceManagerHost"] != values["serviceManagerHost"] ||
	    applied["serviceManagerApiKey"] != values["serviceManagerApiKey"] ||
	    applied["endStreamingDelay"] != values["endStreamingDelay"])
		live_control->configure(values["serviceManagerHost"].toString(),
					values["serviceManagerApiKey"].toString(),
					values["endStreamingDelay"].toString().toInt());
	event_log->configure(values["logRetentionDays"].toString().toInt());
	openlp->configure(values["openlpHost"].toString(), values["openlpPort"].toString().toInt(),
			  values["openlpVersion"].toString(), values["openlpEnabled"].toBool(true));
	companion->configure(values["companionHost"].toString(), values["companionPort"].toString().toInt(),
			     values["companionEnabled"].toBool(true));
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
	dialog = new SettingsDialog(
		settings,
		[](const QJsonObject &updated) -> QString {
			if (live_control && live_control->sessionActive()) {
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
		static_cast<QWidget *>(obs_frontend_get_main_window()));
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
	auto *title = new QLabel("StreamingQATool", widget);
	auto font = title->font();
	font.setBold(true);
	title->setFont(font);
	layout->addWidget(title);
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
	QObject::connect(event_log, &EventLog::storageError, storage, &QLabel::setText);
	auto wire = [widget, layout](auto *monitor, const QString &source) {
		auto *row = new QHBoxLayout;
		auto *dot = new QLabel(widget);
		dot->setFixedSize(12, 12);
		dot->setStyleSheet("background-color: #dc3545; border-radius: 6px;");
		dot->setAccessibleName(source + " disconnected");
		row->addWidget(dot);
		auto *label = new QLabel(widget);
		label->setWordWrap(true);
		label->setTextFormat(Qt::PlainText);
		row->addWidget(label, 1);
		layout->addLayout(row);
		using Monitor = std::remove_pointer_t<decltype(monitor)>;
		QObject::connect(monitor, &Monitor::event, widget,
				 [source](const QString &name, const QJsonObject &details) {
					 if (event_log)
						 event_log->append(source, name, details);
				 });
		QObject::connect(monitor, &Monitor::statusChanged, label, [label, dot, source](const QString &state) {
			const bool connected = state.startsWith("Connected");
			dot->setStyleSheet(QString("background-color: %1; border-radius: 6px;")
						   .arg(connected ? "#28a745" : "#dc3545"));
			dot->setAccessibleName(source + (connected ? " connected" : " disconnected"));
			dot->setToolTip(state);
			label->setText(source + ": " + state);
			if (event_log)
				event_log->append(source, "connection.state", {{"state", state}});
		});
	};
	wire(openlp.data(), "openlp");
	wire(companion.data(), "companion");
	auto *logs = new QPushButton("Open Log Stream", widget);
	layout->addWidget(logs);
	QObject::connect(logs, &QPushButton::clicked, widget, []() {
		if (!log_window)
			log_window = new LogWindow(*event_log, panel);
		log_window->showNormal();
		log_window->raise();
		log_window->activateWindow();
	});
	event_log->append("application", "started");
	apply_settings();
	auto *button = new QPushButton("Settings", widget);
	QObject::connect(button, &QPushButton::clicked, widget, show_settings);
	layout->addWidget(button);
	layout->addStretch();
	if (!obs_frontend_add_dock_by_id(dock_id, "StreamingQATool", widget)) {
		obs_log(LOG_ERROR, "Could not register StreamingQATool dock");
		delete widget;
		return;
	}
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
	delete log_window.data();
	delete dialog.data();
	if (panel)
		obs_frontend_remove_dock(dock_id);
}
