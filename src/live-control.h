#pragma once

#include <QJsonObject>
#include <QElapsedTimer>
#include "service-manager-client.h"
#include <QTimer>
#include <QWidget>
#include <functional>

class QAction;
class QToolButton;
class QComboBox;
class QLabel;
class QPushButton;
class QHBoxLayout;

// OBS operations are injected so the complete workflow can be tested without
// starting a real encoder or publishing a YouTube broadcast.
struct EncoderControl {
	std::function<bool()> active;
	std::function<bool(const QString &, const QString &)> matches;
	std::function<QString(const QString &, const QString &)> start;
	std::function<void()> stop;
};

class LiveControl : public QWidget {
public:
	LiveControl(EncoderControl encoder, QWidget *parent = nullptr);
	void configure(const QString &host, const QString &apiKey, int endDelaySeconds = 30);
	QString recoverService(const QString &id, const QJsonObject &body);
	void refreshStatus();
	void setServiceManagerEnabled(bool enabled);
	void placeServiceSelector(QHBoxLayout *toolbar);
	bool sessionActive() const;
	std::function<void(const QString &, const QJsonObject &)> eventSink;

private:
	void logEvent(const QString &name, QJsonObject details = {});
	using Result = std::function<void(const QJsonObject &)>;
	void request(const QString &path, const QByteArray &method, Result result);
	void attemptRequest(const QString &path, const QByteArray &method, Result result, int retriesLeft, int token);
	void schedulePoll();
	int pollInterval() const;
	void loadServices(int page = 1);
	void selectService();
	void acceptStatus(const QJsonObject &body);
	bool canPrestart() const;
	bool canTransition() const;
	void prestart();
	void transition(const QString &action);
	void endNow();
	void render();
	void fail(const QString &message);
	QString controlPath() const;
	QString lifecycle() const;
	bool live() const;
	bool finished() const;
	bool encoderMatches() const;
	bool validBrowserUrl(const QString &url) const;

	EncoderControl encoder;
	ServiceManagerClient client;
	QTimer poll;
	QTimer encoderWatch;
	QTimer prestartTimeout;
	QComboBox *services;
	QWidget *serviceSelector;
	QPushButton *refresh;
	QPushButton *studio;
	QPushButton *youtube;
	QPushButton *primary;
	QToolButton *overrides;
	QAction *cancel;
	QAction *endOverride;
	QAction *stopOverride;
	QLabel *summary;
	QLabel *message;
	QString baseUrl;
	QString apiKey;
	QString serviceId;
	QJsonObject status;
	int generation = 0;
	bool featureEnabled = true;
	bool busy = false;
	bool pollingPaused = false;
	bool fresh = false;
	bool managed = false;
	bool starting = false;
	bool endRequested = false;
	bool startRequested = false;
	bool endWaiting = false;
	QElapsedTimer endDelay;
	int endDelaySeconds = 30;
	qint64 startDeadline = 0;
	qint64 retryDeadline = 0;
};
