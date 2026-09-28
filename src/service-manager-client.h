#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <functional>
class QThread;

struct ApiError {
	QString message;
	bool retryable = true;
	int status = 0;
};

class ServiceManagerClient : public QObject {
public:
	using Success = std::function<void(const QJsonObject &)>;
	using Failure = std::function<void(const ApiError &)>;
	explicit ServiceManagerClient(QObject *parent = nullptr, int requestTimeoutMs = 20000);
	~ServiceManagerClient() override;
	void configure(const QString &baseUrl, const QString &apiKey);
	void request(const QString &path, const QByteArray &method, Success success, Failure failure,
		     const QByteArray &body = "{}", const QByteArray &contentType = "application/json");
	static QString normalizeHost(QString host);
	static bool validHost(const QString &host);

private:
	QNetworkAccessManager network;
#ifdef Q_OS_WIN
	QList<QThread *> workers;
#endif
	QString baseUrl;
	QByteArray apiKey;
	int timeoutMs;
};
