#include "live-control.h"
#include "request-policy.h"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
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
	services = new QComboBox(this);
	services->setObjectName("selectedService");
	services->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	services->setMinimumContentsLength(15);
	auto *form = new QFormLayout;
	form->addRow("Selected Service", services);
	layout->addLayout(form);
	auto addButton = [this, layout](const char *text, const char *name) {
		auto *button = new QPushButton(text, this);
		button->setObjectName(name);
		layout->addWidget(button);
		return button;
	};
	refresh = addButton("Refresh Services", "refreshServices");
	studio = addButton("Open YouTube Live Control Panel", "openStudio");
	youtube = addButton("Open YouTube", "openYouTube");
	prepare = addButton("Prestart", "prestart");
	startStop = addButton("Start Streaming", "startStop");
	cancel = addButton("Cancel Prestart", "cancelPrestart");
	endOverride = addButton("End YouTube Now (skip wait)", "endNow");
	stopOverride = addButton("Stop OBS Now (override)", "stopObsNow");
	summary = new QLabel(this);
	summary->setObjectName("streamSummary");
	summary->setWordWrap(true);
	summary->setTextFormat(Qt::PlainText);
	layout->addWidget(summary);
	message = new QLabel("Enter the Service Manager host and API key in Settings.", this);
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
	connect(prepare, &QPushButton::clicked, this, [this]() { prestart(); });
	connect(startStop, &QPushButton::clicked, this, [this]() { transition(live() ? "end" : "start"); });
	connect(endOverride, &QPushButton::clicked, this, [this]() { endNow(); });
	connect(stopOverride, &QPushButton::clicked, this, [this]() {
		auto *confirm = new QMessageBox(
			QMessageBox::Warning, "Stop OBS now?",
			"This stops OBS immediately without waiting for YouTube. The YouTube broadcast may remain live.",
			QMessageBox::Yes | QMessageBox::No, this);
		confirm->setAttribute(Qt::WA_DeleteOnClose);
		confirm->setDefaultButton(QMessageBox::No);
		connect(confirm, &QMessageBox::finished, this, [this](int choice) {
			if (choice == QMessageBox::Yes) {
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
	connect(cancel, &QPushButton::clicked, this, [this]() {
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
			message->setText(
				QString("Ending YouTube in %1 seconds. OBS keeps streaming until YouTube confirms completion.")
					.arg(remaining));
			if (remaining == 0 && !busy)
				endNow();
		}
		if (starting && encoder.active()) {
			starting = false;
			message->setText("OBS is sending video. Waiting for YouTube to be ready...");
		} else if (starting && QDateTime::currentMSecsSinceEpoch() > startDeadline) {
			prestartTimeout.stop();
			starting = false;
			managed = false;
			fail("OBS did not start within 30 seconds. Check the OBS log and retry Prestart.");
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
		message->setText(services->count() > 1 ? "Select a service to load its YouTube stream."
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
		message->setText("YouTube broadcast ended. Refresh Services to select the next service.");
	} else if (endRequested && !endWaiting) {
		message->setText(
			"Waiting for YouTube to confirm the broadcast ended. Use End YouTube Now to retry if needed.");
	} else if (live()) {
		message->setText("YouTube broadcast is live.");
	} else if (startRequested) {
		// After the bounded command retries, status polling reconciles the result.
		startRequested = false;
		message->setText("YouTube has not confirmed live yet. Retry Start Streaming if needed.");
	} else if (encoder.active() && encoderMatches()) {
		message->setText(status["canStart"].toBool()
					 ? "YouTube is ready. Click Start Streaming to go live."
					 : "OBS is sending video. Waiting for YouTube to be ready...");
	} else if (!starting) {
		message->setText("Service loaded. Click Prestart to send OBS video to YouTube.");
	}
	if (status["broadcast"].toObject()["enableAutoStart"].toBool() && !live() && !finished())
		message->setText(
			"YouTube auto-start is enabled. Disable it in YouTube Studio to use the separate Prestart / Start workflow.");
}

void LiveControl::prestart()
{
	if (!prepare->isEnabled())
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
	if (!startStop->isEnabled())
		return;
	logEvent(action == "start" ? "youtube.start.requested" : "youtube.end.requested",
		 {{"delaySeconds", action == "end" ? endDelaySeconds : 0}});
	if (action == "start")
		startRequested = true;
	else {
		endRequested = true;
		endWaiting = true;
		endDelay.start();
		message->setText(QString("Ending YouTube in %1 seconds. Use End YouTube Now to skip the wait.")
					 .arg(endDelaySeconds));
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
	message->setText("Ending YouTube; waiting for confirmed completion before stopping OBS...");
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
	refresh->setText(serviceId.isEmpty() || finished() ? "Refresh Services" : "Refresh Status / Retry Connection");
	refresh->setEnabled(!busy && !baseUrl.isEmpty());
	studio->setEnabled(validBrowserUrl(broadcast["studioLiveControlUrl"].toString()));
	youtube->setEnabled(validBrowserUrl(broadcast["watchUrl"].toString()));
	prepare->setEnabled(fresh && !busy && !active && !locked && !finished() && !serviceId.isEmpty() &&
			    !stream["streamName"].toString().isEmpty() && !broadcast["enableAutoStart"].toBool());
	startStop->setText(live() ? "Stop Streaming" : "Start Streaming");
	startStop->setEnabled(fresh && !busy && !starting && !endRequested &&
			      (live() ? status["canEnd"].toBool()
				      : active && encoderMatches() && status["canStart"].toBool() && !startRequested &&
						!endRequested));
	cancel->setVisible(managed && !live() && !finished());
	cancel->setEnabled(managed && fresh && !busy && !live() && !startRequested && !endRequested);
	endOverride->setVisible(endRequested || live());
	endOverride->setEnabled((endRequested || live()) && !busy && !finished());
	stopOverride->setEnabled((active || starting) && (managed || encoderMatches()));
	summary->setText(status.isEmpty() ? QString()
					  : QString("OBS: %1\nYouTube: %2\nEncoder: %3 (%4)")
						    .arg(active     ? "streaming"
							 : starting ? "starting"
								    : "stopped",
							 lifecycle(), stream["streamStatus"].toString(),
							 stream["healthStatus"].toString()));
}

void LiveControl::logEvent(const QString &name, QJsonObject details)
{
	details.insert("serviceId", serviceId);
	details.insert("serviceTitle", services->currentText());
	if (eventSink)
		eventSink(name, details);
}
