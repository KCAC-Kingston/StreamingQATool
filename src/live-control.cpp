#include "live-control.h"
#include "request-policy.h"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QMenu>
#include <QToolButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QUrlQuery>
#include <QVBoxLayout>

LiveControl::LiveControl(EncoderControl control, QWidget *parent) : QWidget(parent), encoder(std::move(control))
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);
	setStyleSheet(
		"QPushButton { padding: 3px 8px; min-height: 20px; } QToolButton { padding: 3px 6px; } QPushButton#primaryAction { font-weight: bold; }");
	services = new QComboBox(this);
	services->setObjectName("selectedService");
	services->setAccessibleName("Selected Service");
	services->setToolTip("Selected Service");
	services->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	services->setMinimumContentsLength(12);
	services->setMinimumWidth(0);
	auto addButton = [this](const char *text, const char *name) {
		auto *button = new QPushButton(text, this);
		button->setObjectName(name);
		return button;
	};
	refresh = addButton("", "refreshServices");
	refresh->setText(QStringLiteral("\u21bb"));
	refresh->setAccessibleName("Refresh services");
	refresh->setToolTip("Refresh services");
	refresh->setFixedSize(26, 26);
	refresh->setStyleSheet("padding: 0px; min-height: 0px;");
	serviceSelector = new QWidget(this);
	serviceSelector->setMinimumWidth(0);
	auto *serviceRow = new QHBoxLayout(serviceSelector);
	serviceRow->setContentsMargins(0, 0, 0, 0);
	serviceRow->setSpacing(4);
	serviceRow->addWidget(services, 1);
	serviceRow->addWidget(refresh);
	layout->addWidget(serviceSelector);
	studio = addButton("Live Control Panel", "openStudio");
	youtube = addButton("YouTube", "openYouTube");
	auto *links = new QHBoxLayout;
	links->addWidget(studio, 1);
	links->addWidget(youtube, 1);
	layout->addLayout(links);
	primary = addButton("Prestart", "primaryAction");
	primary->setFixedHeight(76);
	primary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	overrides = new QToolButton(this);
	overrides->setObjectName("overrides");
	overrides->setFixedHeight(32);
	overrides->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	overrides->setToolButtonStyle(Qt::ToolButtonTextOnly);
	overrides->setPopupMode(QToolButton::MenuButtonPopup);
	auto *menu = new QMenu(overrides);
	cancel = menu->addAction("Cancel Prestart");
	cancel->setObjectName("cancelPrestart");
	endOverride = menu->addAction("End YouTube Now");
	endOverride->setObjectName("endNow");
	stopOverride = menu->addAction("Stop OBS Now");
	stopOverride->setObjectName("stopObsNow");
	overrides->setMenu(menu);
	layout->addWidget(primary);
	layout->addWidget(overrides);
	summary = new QLabel(this);
	summary->setObjectName("streamSummary");
	summary->setWordWrap(true);
	summary->setTextFormat(Qt::PlainText);
	layout->addWidget(summary);
	message = new QLabel("Configure Service Manager in Settings.", this);
	message->setObjectName("controlMessage");
	message->setWordWrap(true);
	message->setTextFormat(Qt::PlainText);
	layout->addWidget(message);
	connect(services, &QComboBox::currentIndexChanged, this, [this]() { selectService(); });
	connect(refresh, &QPushButton::clicked, this, [this]() {
		pollingPaused = false;
		if (sessionActive() || (!serviceId.isEmpty() && !finished()))
			refreshStatus();
		else
			loadServices();
	});
	connect(primary, &QPushButton::clicked, this, [this]() {
		if (canPrestart())
			prestart();
		else if (canTransition())
			transition(live() ? "end" : "start");
	});
	connect(endOverride, &QAction::triggered, this, [this]() { endNow(); });
	connect(stopOverride, &QAction::triggered, this, [this]() {
		auto *confirm = new QMessageBox(
			QMessageBox::Warning, "Stop OBS now?",
			"This stops OBS immediately without waiting for YouTube. The YouTube broadcast may remain live.",
			QMessageBox::Yes | QMessageBox::No, this);
		confirm->setAttribute(Qt::WA_DeleteOnClose);
		confirm->setDefaultButton(QMessageBox::No);
		connect(confirm, &QMessageBox::finished, this, [this](int choice) {
			if (choice == QMessageBox::Yes && featureEnabled) {
				logEvent("obs.stop_override.requested");
				prestartTimeout.stop();
				encoder.stop();
				starting = false;
				message->setText("OBS stop requested by override. YouTube status is still monitored.");
				render();
			}
		});
		confirm->open();
	});
	connect(cancel, &QAction::triggered, this, [this]() {
		if (busy || live() || !fresh || !managed || startRequested || endRequested)
			return;
		logEvent("prestart.cancelled", {{"reason", "user"}});
		encoder.stop();
		prestartTimeout.stop();
		starting = false;
		managed = false;
		message->setText("Prestart cancelled. Waiting for OBS to stop.");
		render();
	});
	auto open = [this](const char *key) {
		const QString url = status["broadcast"].toObject()[key].toString();
		if (validBrowserUrl(url) && !QDesktopServices::openUrl(QUrl(url)))
			message->setText("Could not open the browser.");
	};
	connect(studio, &QPushButton::clicked, this, [open]() { open("studioLiveControlUrl"); });
	connect(youtube, &QPushButton::clicked, this, [open]() { open("watchUrl"); });
	poll.setParent(this);
	poll.setObjectName("serviceManagerPoll");
	poll.setSingleShot(true);
	connect(&poll, &QTimer::timeout, this, [this]() {
		if (serviceId.isEmpty() || (finished() && !encoder.active()))
			loadServices();
		else
			refreshStatus();
		schedulePoll();
	});
	prestartTimeout.setParent(this);
	prestartTimeout.setObjectName("prestartTimeout");
	prestartTimeout.setSingleShot(true);
	prestartTimeout.setInterval(RequestPolicy::prestartTimeoutMs);
	connect(&prestartTimeout, &QTimer::timeout, this, [this]() {
		if (!managed || live() || finished())
			return;
		logEvent("prestart.cancelled", {{"reason", "20 minute timeout"}});
		// Cancel queued retries and ignore in-flight results for this prestart.
		++generation;
		busy = false;
		pollingPaused = false;
		if (encoderMatches())
			encoder.stop();
		managed = starting = startRequested = false;
		fail("Prestart automatically cancelled: YouTube did not start within 20 minutes. OBS stop requested.");
	});
	encoderWatch.setInterval(1000);
	connect(&encoderWatch, &QTimer::timeout, this, [this]() {
		if (endWaiting) {
			const int remaining = qMax(0, endDelaySeconds - int(endDelay.elapsed() / 1000));
			message->setText("OBS stays on until YouTube ends.");
			if (remaining == 0 && !busy)
				endNow();
		}
		if (starting && encoder.active()) {
			starting = false;
			message->setText("Waiting for YouTube readiness.");
		} else if (starting && QDateTime::currentMSecsSinceEpoch() > startDeadline) {
			prestartTimeout.stop();
			starting = false;
			managed = false;
			fail("OBS did not start within 30 seconds. Check the OBS log and retry Prestart.");
		}
		if (finished() && !encoder.active() && !busy) {
			logEvent("service.completed");
			loadServices();
		}
		render();
	});
	encoderWatch.start();
	render();
}

bool LiveControl::sessionActive() const
{
	return starting || encoder.active() || live() || startRequested || endRequested;
}

void LiveControl::configure(const QString &host, const QString &key, int delaySeconds)
{
	if (!featureEnabled)
		return;
	const QString normalizedHost = ServiceManagerClient::normalizeHost(host);
	const bool preserveSession = !serviceId.isEmpty() && sessionActive();
	if (preserveSession && normalizedHost != baseUrl)
		return;
	endDelaySeconds = qBound(0, delaySeconds, 3600);
	++generation; // Ignore replies from a previous connection or selection.
	busy = false;
	pollingPaused = false;
	poll.stop();
	fresh = false;
	if (!preserveSession) {
		prestartTimeout.stop();
		status = {};
		serviceId.clear();
	}
	baseUrl = normalizedHost;
	apiKey = key.trimmed();
	if (!ServiceManagerClient::validHost(baseUrl) || apiKey.isEmpty()) {
		baseUrl.clear();
		services->clear();
		fail("Set the Service Manager domain and API key in Settings. HTTPS is added automatically.");
		return;
	}
	client.configure(baseUrl, apiKey);
	if (preserveSession)
		refreshStatus();
	else
		loadServices();
}

void LiveControl::loadServices(int page)
{
	if (busy || starting || live() || startRequested || endRequested || (managed && encoder.active()) ||
	    baseUrl.isEmpty())
		return;
	if (page == 1) {
		++generation;
		QSignalBlocker blocker(services);
		services->clear();
		services->addItem("Select a service...", "");
		status = {};
		serviceId.clear();
		fresh = false;
		message->setText("Loading services...");
	}
	const auto today = QDateTime::currentDateTimeUtc().toTimeZone(QTimeZone("America/Toronto")).date();
	QUrlQuery query;
	query.addQueryItem("page", QString::number(page));
	query.addQueryItem("pageSize", "100");
	query.addQueryItem("dateFilter", "upcoming");
	query.addQueryItem("date", today.toString(Qt::ISODate));
	query.addQueryItem("youtubeLinked", "true");
	query.addQueryItem("liveControlPending", "true");
	request("/api/admin/v1/services?" + query.toString(), "GET", [this, today, page](const QJsonObject &body) {
		if (!body["items"].isArray()) {
			fail("Service Manager returned an invalid service list.");
			return;
		}
		const auto items = body["items"].toArray();
		QSignalBlocker blocker(services);
		for (const auto &value : items) {
			const auto item = value.toObject();
			const auto date = QDate::fromString(item["serviceDate"].toString(), Qt::ISODate);
			if (!date.isValid() || date < today || date > today.addDays(30) ||
			    !item["youtubeLiveControlCompletedAt"].toString().isEmpty() ||
			    (item["youtubeBroadcastId"].toString().isEmpty() &&
			     item["youtubeVideoId"].toString().isEmpty()))
				continue;
			const QString id = QString::number(item["id"].toInteger());
			if (id == "0" || services->findData(id) >= 0)
				continue;
			services->addItem(QString("%1 %2 - %3")
						  .arg(item["serviceDate"].toString(), item["serviceTime"].toString(),
						       item["title"].toString()),
					  id);
		}
		if (page * 100 < body["totalCount"].toInt() && items.size() == 100 && page < 100) {
			loadServices(page + 1);
			return;
		}
		message->setText(services->count() > 1 ? "Select a service."
						       : "No linked services in the next 30 days.");
	});
}

QString LiveControl::controlPath() const
{
	return "/api/admin/v1/services/" + serviceId + "/youtube/live-control";
}

void LiveControl::selectService()
{
	prestartTimeout.stop();
	++generation;
	busy = false;
	pollingPaused = false;
	serviceId = services->currentData().toString();
	status = {};
	fresh = false;
	managed = false;
	startRequested = false;
	endRequested = false;
	message->setText(serviceId.isEmpty() ? "Select a service." : "Loading stream details...");
	render();
	refreshStatus();
}

void LiveControl::refreshStatus()
{
	if (serviceId.isEmpty() || busy)
		return;
	request(controlPath(), "GET", [this](const QJsonObject &body) { acceptStatus(body); });
}

QString LiveControl::lifecycle() const
{
	return status["broadcast"].toObject()["lifeCycleStatus"].toString().toLower();
}
bool LiveControl::live() const
{
	return lifecycle() == "live" || lifecycle() == "livestarting";
}
bool LiveControl::finished() const
{
	return lifecycle() == "complete" || lifecycle() == "revoked";
}
bool LiveControl::encoderMatches() const
{
	const auto stream = status["stream"].toObject();
	return encoder.matches(stream["ingestionAddress"].toString(), stream["streamName"].toString());
}

void LiveControl::acceptStatus(const QJsonObject &body)
{
	const auto item = body["item"].toObject();
	if (!item["broadcast"].isObject() || !item["stream"].isObject() ||
	    QString::number(item["service"].toObject()["id"].toInteger()) != serviceId ||
	    item["broadcast"].toObject()["lifeCycleStatus"].toString().isEmpty()) {
		fail("Service Manager returned incomplete or mismatched live-control details.");
		return;
	}
	const QString before = lifecycle();
	status = item;
	if (before != lifecycle()) {
		logEvent("youtube.status", {{"previous", before}, {"status", lifecycle()}});
		if (lifecycle() == "live")
			logEvent("youtube.streaming.started");
		if (finished())
			logEvent("youtube.streaming.ended", {{"status", lifecycle()}});
	}
	fresh = true;
	if (live()) {
		startRequested = false;
		prestartTimeout.stop();
	}
	if (finished()) {
		prestartTimeout.stop();
		if ((managed || endRequested) && encoder.active() && encoderMatches())
			encoder.stop();
		managed = starting = startRequested = endRequested = endWaiting = false;
		message->setText("Stream ended. Refresh to select another service.");
	} else if (endRequested && !endWaiting) {
		message->setText("Waiting for YouTube to end. Override to retry.");
	} else if (live()) {
		message->setText("YouTube broadcast is live.");
	} else if (startRequested) {
		// After the bounded command retries, status polling reconciles the result.
		startRequested = false;
		message->setText("YouTube has not confirmed live yet. Retry Start Streaming if needed.");
	} else if (encoder.active() && encoderMatches()) {
		message->setText(status["canStart"].toBool() ? "Ready to start YouTube."
							     : "Waiting for YouTube readiness.");
	} else if (!starting) {
		message->setText("Ready to prestart.");
	}
	if (status["broadcast"].toObject()["enableAutoStart"].toBool() && !live() && !finished())
		message->setText(
			"YouTube auto-start is enabled. Disable it in YouTube Studio to use the separate Prestart / Start workflow.");
}

void LiveControl::prestart()
{
	if (!canPrestart())
		return;
	logEvent("prestart.requested");
	// Re-read immediately before replacing OBS's streaming destination.
	request(controlPath(), "GET", [this](const QJsonObject &body) {
		acceptStatus(body);
		if (!fresh || live() || finished() || encoder.active() ||
		    status["broadcast"].toObject()["enableAutoStart"].toBool())
			return;
		const auto stream = status["stream"].toObject();
		const QString server = stream["ingestionAddress"].toString();
		const QString key = stream["streamName"].toString();
		const QUrl url(server);
		if (key.isEmpty() || url.host().isEmpty() || (url.scheme() != "rtmp" && url.scheme() != "rtmps")) {
			fail("Service Manager did not return a valid RTMP server and stream key.");
			return;
		}
		const QString error = encoder.start(server, key);
		if (!error.isEmpty()) {
			fail(error);
			return;
		}
		logEvent("prestart.encoder_start_requested");
		managed = starting = true;
		prestartTimeout.start(RequestPolicy::prestartTimeoutMs);
		startDeadline = QDateTime::currentMSecsSinceEpoch() + 30000;
		message->setText("Starting OBS encoder...");
	});
}

void LiveControl::transition(const QString &action)
{
	if (!canTransition())
		return;
	logEvent(action == "start" ? "youtube.start.requested" : "youtube.end.requested",
		 {{"delaySeconds", action == "end" ? endDelaySeconds : 0}});
	if (action == "start")
		startRequested = true;
	else {
		endRequested = true;
		endWaiting = true;
		endDelay.start();
		message->setText("OBS stays on until YouTube ends. Override to skip delay.");
		render();
		if (endDelaySeconds == 0)
			endNow();
		return;
	}
	message->setText(action == "start" ? "Starting YouTube broadcast..." : "Ending YouTube broadcast...");
	request(controlPath() + '/' + action, "POST", [this](const QJsonObject &body) { acceptStatus(body); });
}

void LiveControl::endNow()
{
	if (busy || serviceId.isEmpty() || finished())
		return;
	logEvent("youtube.end.command_requested",
		 {{"skippedDelay", endWaiting && endDelay.elapsed() < endDelaySeconds * 1000}});
	endWaiting = false;
	endRequested = true;
	message->setText("Waiting for YouTube to end; OBS stays on.");
	request(controlPath() + "/end", "POST", [this](const QJsonObject &body) { acceptStatus(body); });
}

bool LiveControl::validBrowserUrl(const QString &text) const
{
	const QUrl url(text);
	return url.scheme() == "https" &&
	       (url.host() == "youtube.com" || url.host().endsWith(".youtube.com") || url.host() == "youtu.be");
}

void LiveControl::fail(const QString &text)
{
	logEvent("control.error", {{"message", text}});
	fresh = false;
	message->setText(text);
	render();
}

void LiveControl::render()
{
	schedulePoll();
	const bool active = encoder.active();
	const bool locked = starting || live() || startRequested || endRequested ||
			    (active && (managed || encoderMatches()));
	const auto broadcast = status["broadcast"].toObject();
	const auto stream = status["stream"].toObject();
	services->setEnabled(!locked && !busy);
	refresh->setToolTip(serviceId.isEmpty() || finished() ? "Refresh services"
							      : "Refresh status / retry connection");
	services->setToolTip(services->currentText());
	refresh->setEnabled(!busy && !baseUrl.isEmpty());
	studio->setEnabled(validBrowserUrl(broadcast["studioLiveControlUrl"].toString()));
	youtube->setEnabled(validBrowserUrl(broadcast["watchUrl"].toString()));
	QString primaryText = "Prestart";
	if (endWaiting)
		primaryText = QString("Ending in %1s").arg(qMax(0, endDelaySeconds - int(endDelay.elapsed() / 1000)));
	else if (endRequested)
		primaryText = "Waiting for YouTube to end";
	else if (finished())
		primaryText = "Stream ended";
	else if (live())
		primaryText = "End Streaming";
	else if (startRequested)
		primaryText = "Starting YouTube...";
	else if (starting)
		primaryText = QString("Starting OBS... (%1s)")
				      .arg(qMax(qint64(0),
						(startDeadline - QDateTime::currentMSecsSinceEpoch() + 999) / 1000));
	else if (active && encoderMatches())
		primaryText = status["canStart"].toBool() ? "Start Streaming" : "Waiting for YouTube...";
	else if (busy)
		primaryText = "Loading...";
	else if (baseUrl.isEmpty())
		primaryText = "Configure Service Manager";
	else if (serviceId.isEmpty())
		primaryText = "Select a service";
	else if (!fresh)
		primaryText = "Refresh to reconnect";
	if (busy && retryDeadline > QDateTime::currentMSecsSinceEpoch())
		primaryText = QString("Retrying %1 in %2s")
				      .arg(endRequested ? "End" : "Start")
				      .arg((retryDeadline - QDateTime::currentMSecsSinceEpoch() + 999) / 1000);
	if (prestartTimeout.isActive() && !live() && !endRequested) {
		const int seconds = (prestartTimeout.remainingTime() + 999) / 1000;
		primaryText +=
			QString("\nAuto-cancel in %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
	}
	const QString color = endRequested                                                               ? "#92400e"
			      : live()                                                                   ? "#b91c1c"
			      : (starting || startRequested || (active && !status["canStart"].toBool())) ? "#92400e"
			      : (active && status["canStart"].toBool())                                  ? "#15803d"
			      : canPrestart()                                                            ? "#1d4ed8"
													 : "#475569";
	const QString style =
		QString("QPushButton#primaryAction { background-color: %1; color: white; font-size: 15px; font-weight: bold; min-height: 48px; padding: 8px 12px; border: 2px solid transparent; border-radius: 6px; } QPushButton#primaryAction:disabled { background-color: %1; color: white; } QPushButton#primaryAction:hover:enabled { border-color: #cbd5e1; } QPushButton#primaryAction:focus { border-color: white; }")
			.arg(color);
	if (primary->styleSheet() != style)
		primary->setStyleSheet(style);
	primary->setText(primaryText);
	primary->setEnabled(canPrestart() || canTransition());
	primary->setToolTip(primary->isEnabled() ? primaryText : message->text());
	cancel->setVisible(managed && !live() && !finished());
	cancel->setEnabled(managed && fresh && !busy && !live() && !startRequested && !endRequested);
	endOverride->setVisible(endRequested || live());
	endOverride->setEnabled((endRequested || live()) && !busy && !finished());
	stopOverride->setEnabled((active || starting) && (managed || encoderMatches()));
	QAction *preferred = cancel->isEnabled()                          ? cancel
			     : (endRequested && endOverride->isEnabled()) ? endOverride
									  : stopOverride;
	overrides->setDefaultAction(preferred);
	const bool anyOverride = cancel->isEnabled() || endOverride->isEnabled() || stopOverride->isEnabled();
	overrides->setEnabled(anyOverride);
	if (!anyOverride)
		overrides->setText("No active stream to override");
	overrides->setToolTip(anyOverride ? "Override: " + preferred->text() + ". Use the arrow for other actions."
					  : "Overrides become available during Prestart or streaming.");
	endOverride->setText(endWaiting ? "End YouTube Now" : endRequested ? "Retry YouTube End" : "End YouTube Now");
	summary->setText(status.isEmpty() ? QString()
					  : QString("OBS: %1 | YouTube: %2")
						    .arg(active     ? "streaming"
							 : starting ? "starting"
								    : "stopped",
							 lifecycle()));
	summary->setToolTip(
		QString("Encoder: %1 (%2)").arg(stream["streamStatus"].toString(), stream["healthStatus"].toString()));
	summary->setVisible(!status.isEmpty());
}

void LiveControl::logEvent(const QString &name, QJsonObject details)
{
	details.insert("serviceId", serviceId);
	details.insert("serviceTitle", services->currentText());
	if (eventSink)
		eventSink(name, details);
}

bool LiveControl::canPrestart() const
{
	const bool active = encoder.active();
	const bool locked = starting || live() || startRequested || endRequested ||
			    (active && (managed || encoderMatches()));
	return featureEnabled && fresh && !busy && !active && !locked && !finished() && !serviceId.isEmpty() &&
	       !status["stream"].toObject()["streamName"].toString().isEmpty() &&
	       !status["broadcast"].toObject()["enableAutoStart"].toBool();
}

bool LiveControl::canTransition() const
{
	return featureEnabled && fresh && !busy && !starting && !endRequested &&
	       (live() ? status["canEnd"].toBool()
		       : encoder.active() && encoderMatches() && status["canStart"].toBool() && !startRequested);
}

void LiveControl::placeServiceSelector(QHBoxLayout *toolbar)
{
	toolbar->addWidget(serviceSelector, 1);
	serviceSelector->setVisible(featureEnabled);
}

void LiveControl::setServiceManagerEnabled(bool enabled)
{
	setVisible(enabled);
	serviceSelector->setVisible(enabled);
	if (featureEnabled == enabled)
		return;
	featureEnabled = enabled;
	if (enabled) {
		encoderWatch.start();
		return;
	}
	++generation; // Ignore in-flight replies and queued command retries.
	poll.stop();
	prestartTimeout.stop();
	encoderWatch.stop();
	busy = fresh = managed = starting = endRequested = startRequested = endWaiting = false;
	pollingPaused = true;
	status = {};
	serviceId.clear();
	baseUrl.clear();
	for (auto *confirmation : findChildren<QMessageBox *>())
		confirmation->reject();
	render();
}

QString LiveControl::recoverService(const QString &id, const QJsonObject &body)
{
	if (!featureEnabled)
		return "Enable Service Manager before recovering.";
	const auto item = body["item"].toObject();
	const auto stream = item["stream"].toObject();
	const auto state = item["broadcast"].toObject()["lifeCycleStatus"].toString().toLower();
	const QString server = stream["ingestionAddress"].toString(), key = stream["streamName"].toString();
	if (QString::number(item["service"].toObject()["id"].toInteger()) != id || state.isEmpty() ||
	    state == "complete" || state == "revoked")
		return "The service is no longer recoverable.";
	if (QUrl(server).host().isEmpty() || (QUrl(server).scheme() != "rtmp" && QUrl(server).scheme() != "rtmps") ||
	    key.isEmpty())
		return "Service Manager returned invalid stream credentials.";
	if (encoder.active() && !encoder.matches(server, key))
		return "OBS is streaming another destination. Stop it before recovery.";
	++generation;
	busy = false;
	pollingPaused = false;
	endWaiting = endRequested = startRequested = false;
	prestartTimeout.stop();
	poll.stop();
	serviceId = id;
	{
		QSignalBlocker block(services);
		services->clear();
		services->addItem(item["service"].toObject()["title"].toString("Recovered service " + id), id);
	}
	status = item;
	fresh = true;
	if (!encoder.active()) {
		const auto error = encoder.start(server, key);
		if (!error.isEmpty()) {
			render();
			return error;
		}
		starting = true;
		startDeadline = QDateTime::currentMSecsSinceEpoch() + 30000;
	}
	managed = true;
	if (!live())
		prestartTimeout.start(RequestPolicy::prestartTimeoutMs);
	message->setText("Recovered service. Reconnecting OBS to YouTube...");
	render();
	return {};
}
