#include "live-control.h"
#include "request-policy.h"

#include <QLabel>
#include <QDateTime>

int LiveControl::pollInterval() const
{
	return RequestPolicy::pollInterval(endRequested, endWaiting, live(),
					   starting || startRequested || (encoder.active() && encoderMatches()));
}

void LiveControl::schedulePoll()
{
	if (!featureEnabled || busy || pollingPaused || baseUrl.isEmpty()) {
		poll.stop();
		return;
	}
	// Do not reset the timer every time the one-second UI timer renders.
	const int interval = pollInterval();
	if (!poll.isActive() || poll.interval() != interval)
		poll.start(interval);
}

void LiveControl::request(const QString &path, const QByteArray &method, Result result)
{
	if (!featureEnabled || busy || baseUrl.isEmpty())
		return;
	busy = true;
	retryDeadline = 0;
	poll.stop();
	render();
	const bool command = method == "POST" && (path.endsWith("/start") || path.endsWith("/end"));
	attemptRequest(path, method, result, command ? RequestPolicy::commandRetries : 0, generation);
}

void LiveControl::attemptRequest(const QString &path, const QByteArray &method, Result result, int retriesLeft,
				 int token)
{
	if (!featureEnabled || token != generation)
		return;
	retryDeadline = 0;
	// A local stop/override during a retry must not start YouTube afterward.
	if (method == "POST" && path.endsWith("/start") && (!encoder.active() || !encoderMatches())) {
		busy = false;
		startRequested = false;
		fail("Start cancelled because OBS is no longer sending to the selected service.");
		return;
	}
	client.request(
		path, method,
		[this, token, result](const QJsonObject &body) {
			if (!featureEnabled || token != generation)
				return;
			busy = false;
			pollingPaused = false;
			result(body);
			render();
		},
		[this, path, method, result, retriesLeft, token](const ApiError &error) {
			if (!featureEnabled || token != generation)
				return;
			fresh = false;
			if (retriesLeft > 0) {
				const int retry = RequestPolicy::commandRetries - retriesLeft + 1;
				logEvent("youtube.command.retry",
					 {{"command", path.endsWith("/start") ? "start" : "end"}, {"retry", retry}});
				retryDeadline = QDateTime::currentMSecsSinceEpoch() + RequestPolicy::retryDelayMs;
				message->setText(QString("%1 Retrying %2 (%3/%4)...")
							 .arg(error.message, path.endsWith("/start") ? "Start" : "End")
							 .arg(retry)
							 .arg(RequestPolicy::commandRetries));
				// Keep busy throughout the delay: no competing polls or button clicks.
				QTimer::singleShot(RequestPolicy::retryDelayMs, this,
						   [this, path, method, result, retriesLeft, token]() {
							   attemptRequest(path, method, result, retriesLeft - 1, token);
						   });
				render();
				return;
			}
			busy = false;
			const bool command = method == "POST";
			// After an uncertain command result, continue reading status to discover
			// whether YouTube acted. Never replay another command after this limit.
			pollingPaused = !command && !error.retryable;
			fail(error.message +
			     (command ? QString(" Command failed after %1 retries.").arg(RequestPolicy::commandRetries)
				      : QString()) +
			     " OBS has not been stopped. You can retry or use the stop overrides.");
		});
}
