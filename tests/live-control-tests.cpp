#include "live-control.h"
#include "settings.h"
#include "request-policy.h"
#include <QApplication>
#include <QComboBox>
#include <QDate>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QAction>
#include <QToolButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <cstdio>
#include <stdexcept>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void waitFor(const std::function<bool()> &condition, int timeout = 4000)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (!condition() && elapsed.elapsed() < timeout) {
		QApplication::processEvents();
		QThread::msleep(5);
	}
	check(condition(), "Timed out waiting for workflow state");
}

struct Worker : QTcpServer {
	QString state = "ready";
	bool receiving = false;
	bool autoStart = false;
	bool failEnd = false;
	bool confirmEnd = true;
	bool deny = false;
	int ends = 0;
	int starts = 0;
	int gets = 0;
	int lists = 0;
	int startDenials = 0;
	int endDenials = 0;
	QElapsedTimer clock;
	QList<qint64> startTimes;
	QList<qint64> endTimes;
	bool authenticated = true;
	int responseCode = 200;
	bool malformed = false;
	bool hang = false;

	Worker()
	{
		clock.start();
		check(listen(QHostAddress::LocalHost), "Mock Worker could not listen");
		connect(this, &QTcpServer::newConnection, this, [this]() {
			while (hasPendingConnections()) {
				auto *socket = nextPendingConnection();
				connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
				connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
					QByteArray bytes =
						socket->property("request").toByteArray() + socket->readAll();
					socket->setProperty("request", bytes);
					if (!bytes.contains("\r\n\r\n") || socket->property("handled").toBool())
						return;
					socket->setProperty("handled", true);
					authenticated &= bytes.toLower().contains("x-api-key: test-key");
					const QByteArray path = bytes.split(' ').value(1);
					const bool post = bytes.startsWith("POST");
					if (hang)
						return;
					int code = deny ? 401 : responseCode;
					QJsonObject body;
					if (path.startsWith("/api/admin/v1/services?")) {
						++lists;
						body = {{"items", QJsonArray{QJsonObject{
									  {"id", 1},
									  {"title", "Test service"},
									  {"serviceDate",
									   QDate::currentDate().addDays(1).toString(
										   Qt::ISODate)},
									  {"serviceTime", "10:00"},
									  {"youtubeBroadcastId", "test-video"}}}},
							{"totalCount", 1}};
					} else {
						if (post && path.endsWith("/start")) {
							++starts;
							startTimes.append(clock.elapsed());
							if (startDenials-- > 0)
								code = 403;
							else
								state = "live";
						}
						if (post && path.endsWith("/end")) {
							++ends;
							endTimes.append(clock.elapsed());
							if (endDenials-- > 0)
								code = 403;
							else if (failEnd)
								code = 500;
							else if (confirmEnd)
								state = "complete";
						}
						if (!post)
							++gets;
						body = {{"item",
							 QJsonObject{
								 {"service", QJsonObject{{"id", 1}}},
								 {"broadcast",
								  QJsonObject{
									  {"lifeCycleStatus", state},
									  {"enableAutoStart", autoStart},
									  {"studioLiveControlUrl",
									   "https://studio.youtube.com/video/test/livestreaming"},
									  {"watchUrl",
									   "https://www.youtube.com/watch?v=test"}}},
								 {"stream",
								  QJsonObject{{"ingestionAddress",
									       "rtmp://example.test/live"},
									      {"streamName", "private-key"},
									      {"streamStatus",
									       receiving ? "active" : "inactive"},
									      {"healthStatus", "good"}}},
								 {"canStart", receiving && state == "ready"},
								 {"canEnd", state == "live"}}}};
					}
					const auto data = malformed
								  ? QByteArray("<html>Worker exception</html>")
								  : QJsonDocument(body).toJson(QJsonDocument::Compact);
					socket->write(
						"HTTP/1.1 " + QByteArray::number(code) +
						" Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
						QByteArray::number(data.size()) + "\r\n\r\n" + data);
					socket->disconnectFromHost();
				});
			}
		});
	}
};

static void workflow(int delay, bool skipWait, bool failEnd, int denials = 0)
{
	Worker worker;
	bool active = false;
	QString server, key;
	int stops = 0;
	EncoderControl encoder{
		[&]() { return active; },
		[&](const QString &s, const QString &k) { return s == server && k == key && !k.isEmpty(); },
		[&](const QString &s, const QString &k) {
			server = s;
			key = k;
			active = true;
			return QString();
		},
		[&]() {
			active = false;
			++stops;
		},
	};
	LiveControl panel(encoder);
	auto button = [&](const char *name) {
		return panel.findChild<QPushButton *>(name);
	};
	check(!button("primaryAction")->isEnabled() && !button("openYouTube")->isEnabled(),
	      "Controls enabled before selection");
	panel.configure(QString("http://127.0.0.1:%1").arg(worker.serverPort()), "test-key", delay);
	auto *services = panel.findChild<QComboBox *>("selectedService");
	waitFor([&]() { return services->count() == 2 && services->isEnabled(); });
	auto *poll = panel.findChild<QTimer *>("serviceManagerPoll");
	check(poll && poll->interval() == 300000 && worker.lists == 1 && worker.gets == 0,
	      "Startup did not make exactly one list request with slow idle polling");
	services->setCurrentIndex(1);
	waitFor([&]() { return button("primaryAction")->isEnabled(); });
	check(button("openStudio")->isEnabled() && button("openYouTube")->isEnabled(),
	      "Browser controls not enabled after selection");
	check(button("primaryAction")->text() == "Prestart", "Primary label before prestart");
	button("primaryAction")->click();
	waitFor([&]() { return active; });
	check(poll->interval() == 15000, "Prestart polling is not 15 seconds");
	auto *deadline = panel.findChild<QTimer *>("prestartTimeout");
	check(deadline && deadline->isActive() && deadline->interval() == 1200000,
	      "Prestart timeout is not 20 minutes");
	check(server == "rtmp://example.test/live" && key == "private-key", "Incorrect OBS RTMP credentials");
	check(!button("primaryAction")->isEnabled(), "Start enabled before YouTube readiness");
	worker.receiving = true;
	panel.refreshStatus();
	waitFor([&]() { return button("primaryAction")->isEnabled(); });
	check(!services->isEnabled(), "Selection unlocked during streaming");
	check(button("primaryAction")->text() == "Start Streaming", "Primary label when ready");
	check(panel.findChild<QToolButton *>("overrides")->defaultAction() ==
		      panel.findChild<QAction *>("cancelPrestart"),
	      "Cancel override during prestart");
	worker.startDenials = denials;
	button("primaryAction")->click();
	waitFor(
		[&]() {
			return button("primaryAction")->text() == "End Streaming" &&
			       button("primaryAction")->isEnabled();
		},
		6000);
	check(worker.starts == denials + 1, "Incorrect number of Start retries");
	check(!deadline->isActive(), "Prestart timeout remained armed after YouTube went live");
	check(poll->interval() == 30000, "Live polling is not 30 seconds");
	for (int i = 1; i < worker.startTimes.size(); ++i)
		check(worker.startTimes[i] - worker.startTimes[i - 1] >= 950, "Start retries were too fast");
	worker.confirmEnd = false;
	worker.failEnd = failEnd;
	worker.endDenials = denials;
	button("primaryAction")->click();
	if (delay > 0)
		check(worker.ends == 0 && active, "OBS or YouTube stopped before the countdown");
	if (skipWait)
		panel.findChild<QAction *>("endNow")->trigger();
	waitFor(
		[&]() {
			return worker.ends == (failEnd ? 4 : denials + 1) &&
			       panel.findChild<QAction *>("endNow")->isEnabled();
		},
		6000);
	check(poll->interval() == 15000, "End confirmation polling is not 15 seconds");
	for (int i = 1; i < worker.endTimes.size(); ++i)
		check(worker.endTimes[i] - worker.endTimes[i - 1] >= 950, "End retries were too fast");
	check(active && stops == 0, "OBS stopped without YouTube completion confirmation");
	check(panel.findChild<QAction *>("stopObsNow")->isEnabled(), "Override unavailable while ending");
	if (failEnd) {
		QElapsedTimer noExtraRetry;
		noExtraRetry.start();
		waitFor([&]() { return noExtraRetry.elapsed() >= 1200; });
		check(worker.ends == 4, "End retried beyond the three-retry limit");
		panel.findChild<QAction *>("stopObsNow")->trigger();
		waitFor([&]() { return panel.findChild<QMessageBox *>() != nullptr; });
		panel.findChild<QMessageBox *>()->done(QMessageBox::Yes);
		waitFor([&]() { return stops == 1; });
		check(!active, "OBS override failed");
	} else {
		worker.state = "complete";
		panel.refreshStatus();
		waitFor([&]() { return stops == 1; });
		check(!active, "OBS not stopped after YouTube confirmation");
	}
	check(worker.authenticated, "Missing API-key authentication");
}

static void serviceManagerToggle()
{
	Worker worker;
	bool active = false;
	int stops = 0;
	LiveControl panel({[&]() { return active; }, [](const QString &, const QString &) { return true; },
			   [&](const QString &, const QString &) {
				   active = true;
				   return QString();
			   },
			   [&]() {
				   active = false;
				   ++stops;
			   }});
	const auto host = QString("http://127.0.0.1:%1").arg(worker.serverPort());
	panel.setServiceManagerEnabled(false);
	panel.configure(host, "test-key");
	QElapsedTimer wait;
	wait.start();
	waitFor([&]() { return wait.elapsed() >= 100; });
	check(panel.isHidden() && worker.lists == 0, "Disabled Service Manager made a startup request");
	panel.setServiceManagerEnabled(true);
	panel.configure(host, "test-key");
	auto *services = panel.findChild<QComboBox *>("selectedService");
	auto *primary = panel.findChild<QPushButton *>("primaryAction");
	waitFor([&]() { return services->count() == 2 && services->isEnabled(); });
	services->setCurrentIndex(1);
	waitFor([&]() { return primary->isEnabled(); });
	primary->click();
	waitFor([&]() { return active; });
	worker.receiving = true;
	worker.startDenials = 10;
	panel.refreshStatus();
	waitFor([&]() { return primary->isEnabled(); });
	primary->click();
	waitFor([&]() { return worker.starts == 1; });
	panel.setServiceManagerEnabled(false);
	wait.restart();
	waitFor([&]() { return wait.elapsed() >= 1300; });
	check(panel.isHidden() && worker.starts == 1 && stops == 0 && active,
	      "Disabling must cancel queued retries without stopping OBS");
	check(!panel.findChild<QTimer *>("serviceManagerPoll")->isActive() &&
		      !panel.findChild<QTimer *>("prestartTimeout")->isActive(),
	      "Disabled timers still armed");
}

static void prestartAutoCancel()
{
	Worker worker;
	bool active = false;
	int stops = 0;
	LiveControl panel({[&]() { return active; }, [](const QString &, const QString &) { return true; },
			   [&](const QString &, const QString &) {
				   active = true;
				   return QString();
			   },
			   [&]() {
				   active = false;
				   ++stops;
			   }});
	panel.configure(QString("http://127.0.0.1:%1").arg(worker.serverPort()), "test-key");
	auto *services = panel.findChild<QComboBox *>("selectedService");
	waitFor([&]() { return services->count() == 2 && services->isEnabled(); });
	services->setCurrentIndex(1);
	auto *prepare = panel.findChild<QPushButton *>("primaryAction");
	waitFor([&]() { return prepare->isEnabled(); });
	prepare->click();
	waitFor([&]() { return active; });
	auto *deadline = panel.findChild<QTimer *>("prestartTimeout");
	check(deadline && deadline->interval() == RequestPolicy::prestartTimeoutMs, "Incorrect prestart deadline");
	// Advance only the deadline timer, exercising the real expiry handler.
	deadline->start(20);
	waitFor([&]() { return stops == 1; });
	check(!active && worker.starts == 0 && worker.ends == 0, "Prestart timeout did not stop only the OBS encoder");
	check(panel.findChild<QLabel *>("controlMessage")->text().contains("20 minutes"),
	      "Timeout explanation missing");
}

static void apiFailures()
{
	for (const QString &input :
	     {QString("servicemanager.kcac.ca"), QString("servicemanager.kcac.ca/"),
	      QString("https://servicemanager.kcac.ca/"), QString("//servicemanager.kcac.ca/")}) {
		const auto normalized = ServiceManagerClient::normalizeHost(input);
		check(normalized == "https://servicemanager.kcac.ca" && ServiceManagerClient::validHost(normalized),
		      "Bare hostname or optional trailing slash was not normalized correctly");
	}
	Worker worker;
	ServiceManagerClient client(nullptr, 100);
	client.configure(QString("http://127.0.0.1:%1").arg(worker.serverPort()), "test-key");
	for (int code : {401, 403, 404, 409, 429, 500, 503, 302}) {
		worker.responseCode = code;
		bool done = false;
		bool failed = false;
		ApiError error;
		client.request(
			"/api/admin/v1/services", "GET", [&](const QJsonObject &) { done = true; },
			[&](const ApiError &e) {
				error = e;
				failed = done = true;
			});
		waitFor([&]() { return done; });
		check(failed && !error.message.isEmpty(), "HTTP failure did not produce an actionable error");
		if (code == 401 || code == 403)
			check(!error.retryable, "Authentication failure retried automatically");
		check(!error.message.contains("test-key"), "API credential exposed in error");
	}
	worker.responseCode = 200;
	worker.malformed = true;
	bool failed = false;
	client.request(
		"/api/admin/v1/services", "GET", [](const QJsonObject &) {}, [&](const ApiError &) { failed = true; });
	waitFor([&]() { return failed; });
	worker.malformed = false;
	worker.hang = true;
	failed = false;
	client.request(
		"/api/admin/v1/services", "GET", [](const QJsonObject &) {}, [&](const ApiError &) { failed = true; });
	waitFor([&]() { return failed; });
	worker.close();
	failed = false;
	client.request(
		"/api/admin/v1/services", "GET", [](const QJsonObject &) {}, [&](const ApiError &) { failed = true; });
	waitFor([&]() { return failed; });
	check(!ServiceManagerClient::validHost("http://public.example"), "Insecure remote API allowed");
	check(!ServiceManagerClient::validHost("https://example.com/api/admin"), "API path accepted as host");
}

static void settingsPersistence()
{
	QTemporaryDir directory;
	check(directory.isValid(), "Temporary settings directory unavailable");
	const auto path = directory.filePath("settings.json");
	SettingsStore settings;
	check(settings.load(path).isEmpty(), "Default settings failed to load");
	check(settings.values()["endStreamingDelay"].toString() == "30", "Default end delay is not 30");
	QJsonObject saved{{"endStreamingDelay", "17"},
			  {"serviceManagerHost", "https://example.com"},
			  {"serviceManagerApiKey", "secret-credential"},
			  {"companionPort", "8000"},
			  {"openlpPort", "4316"}};
	check(settings.save(saved).isEmpty(), "Settings save failed");
	SettingsStore reloaded;
	check(reloaded.load(path).isEmpty() && reloaded.values()["endStreamingDelay"] == saved["endStreamingDelay"] &&
		      reloaded.values()["serviceManagerApiKey"] == saved["serviceManagerApiKey"] &&
		      reloaded.values()["logRetentionDays"].toString() == "90",
	      "Settings did not persist across reload");
	check(!reloaded.startupLogLines().join('\n').contains("secret-credential"), "API key exposed in startup logs");
	QFile file(path);
	check(file.open(QIODevice::WriteOnly), "Could not create corrupt settings fixture");
	file.write("not JSON");
	file.close();
	check(!reloaded.load(path).isEmpty(), "Corrupt settings silently accepted");
	check(reloaded.values()["endStreamingDelay"].toString() == "30", "Corrupt settings lost safe defaults");
	SettingsStore unwritable;
	unwritable.load(directory.path());
	check(!unwritable.save(saved).isEmpty(), "Settings write failure was not reported");
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
#ifdef Q_OS_WIN
	// Reproduce OBS's missing TLS-plugin deployment. Native HTTPS must not need it.
	QCoreApplication::setLibraryPaths({});
#endif
	try {
		workflow(30, true, false);    // Default wait can be overridden.
		workflow(1, false, false);    // Custom countdown fires automatically.
		workflow(0, false, true);     // Zero delay and emergency override after API error.
		workflow(0, false, false, 3); // Access denied on both commands, successful third retries.
		prestartAutoCancel();
		serviceManagerToggle();
		apiFailures();
		settingsPersistence();
		std::puts(
			"PASS: workflow, delays, overrides, HTTP/auth failures, malformed JSON, timeout, offline connection, settings persistence, corrupt settings, and secret redaction");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
