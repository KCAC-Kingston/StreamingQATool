#include "service-manager-client.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QTimer>
#ifdef Q_OS_WIN
#include "windows-http.h"
#include <QThread>
#include <memory>
#endif

namespace {
ApiError describeHttpError(int code)
{
	switch (code) {
	case 401:
		return {"API key rejected. Correct the Service Manager API key in Settings.", false};
	case 403:
		return {"Access denied. Check the API key permissions or Cloudflare access rules.", false};
	case 404:
		return {"Service or API endpoint not found. Check the host and refresh services.", false};
	case 409:
		return {"YouTube is not ready for that action or its state changed. Refresh status and retry."};
	case 429:
		return {"Service Manager is rate limiting requests. Retrying status more slowly."};
	default:
		break;
	}
	if (code >= 500)
		return {"Service Manager is unavailable (server error). Status will retry automatically."};
	if (code >= 300 && code < 400)
		return {"Service Manager redirected the request. Set the final Worker HTTPS host in Settings.", false};
	if (code >= 400)
		return {QString("Service Manager rejected the request (HTTP %1). Refresh status before retrying.")
				.arg(code),
			false};
	return {"Service Manager returned an unexpected HTTP response."};
}

#ifndef Q_OS_WIN
ApiError describeError(QNetworkReply *reply)
{
	const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply->property("timedOut").toBool())
		return {"Service Manager timed out. Check your connection or try again."};
	if (reply->property("oversized").toBool())
		return {"Service Manager returned an unexpectedly large response.", false};
	if (code >= 300)
		return describeHttpError(code);
	switch (reply->error()) {
	case QNetworkReply::HostNotFoundError:
		return {"Cannot resolve Service Manager. Check internet access and the configured host."};
	case QNetworkReply::ConnectionRefusedError:
		return {"Service Manager refused the connection. Check the host and whether the service is running."};
	case QNetworkReply::SslHandshakeFailedError:
		return {"TLS connection failed. Check the server certificate and system clock.", false};
	case QNetworkReply::TimeoutError:
		return {"Service Manager timed out. Status will retry automatically."};
	default:
		return {"Connection to Service Manager was lost. Check internet access; status will retry automatically."};
	}
}
#endif
} // namespace

ServiceManagerClient::ServiceManagerClient(QObject *parent, int requestTimeoutMs)
	: QObject(parent),
	  timeoutMs(qMax(100, requestTimeoutMs))
{
}

ServiceManagerClient::~ServiceManagerClient()
{
#ifdef Q_OS_WIN
	// Do not unload plugin code while its native network workers are running.
	for (auto *worker : workers)
		worker->requestInterruption();
	for (auto *worker : workers)
		worker->wait();
#endif
}

QString ServiceManagerClient::normalizeHost(QString host)
{
	host = host.trimmed();
	if (host.startsWith("//"))
		host.remove(0, 2);
	if (!host.isEmpty() && !host.contains("://"))
		host.prepend("https://");
	while (host.endsWith('/'))
		host.chop(1);
	return QUrl(host).toString();
}

bool ServiceManagerClient::validHost(const QString &host)
{
	const QUrl url(host);
	const bool local = url.host() == "localhost" || url.host() == "127.0.0.1" || url.host() == "::1";
	return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() &&
	       !url.hasFragment() && (url.path().isEmpty() || url.path() == "/") &&
	       (url.scheme() == "https" || (local && url.scheme() == "http"));
}

void ServiceManagerClient::configure(const QString &host, const QString &key)
{
	baseUrl = normalizeHost(host);
	apiKey = key.trimmed().toUtf8();
}

void ServiceManagerClient::request(const QString &path, const QByteArray &method, Success success, Failure failure,
				   const QByteArray &body, const QByteArray &contentType)
{
	if (!validHost(baseUrl) || apiKey.isEmpty() || apiKey.contains('\r') || apiKey.contains('\n')) {
		failure({"Set a valid Service Manager host and API key in Settings.", false});
		return;
	}
#ifdef Q_OS_WIN
	const QUrl url(baseUrl + path);
	const QByteArray key = apiKey;
	const int timeout = timeoutMs;
	auto result = std::make_shared<WindowsHttpResult>();
	auto *worker = QThread::create([url, method, key, timeout, result, body, contentType]() {
		*result = windowsHttpRequest(url, method, key, timeout, body, contentType);
	});
	worker->setParent(this);
	workers.append(worker);
	connect(worker, &QThread::finished, this, [this, worker, result, success, failure]() {
		workers.removeOne(worker);
		worker->deleteLater();
		if (!result->error.isEmpty()) {
			failure({result->error, result->retryable});
			return;
		}
		if (result->status < 200 || result->status >= 300) {
			auto error = describeHttpError(result->status);
			error.status = result->status;
			failure(error);
			return;
		}
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(result->body, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			failure({"Service Manager returned invalid JSON. Check the Worker host and service status."});
			return;
		}
		success(document.object());
	});
	worker->start();
#else
	QNetworkRequest request{QUrl(baseUrl + path)};
	request.setRawHeader("X-API-Key", apiKey);
	request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
	// Never forward a credential to a redirect destination or ignore TLS errors.
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(timeoutMs);
	auto *reply = method == "POST" ? network.post(request, body) : network.get(request);
	QTimer::singleShot(timeoutMs, reply, [reply]() {
		if (!reply->isFinished()) {
			reply->setProperty("timedOut", true);
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
		if (received > 2 * 1024 * 1024) {
			reply->setProperty("oversized", true);
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [reply, success, failure]() {
		reply->deleteLater();
		const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300) {
			auto error = describeError(reply);
			error.status = code;
			failure(error);
			return;
		}
		QJsonParseError error;
		const auto document = QJsonDocument::fromJson(reply->readAll(), &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			failure({"Service Manager returned invalid JSON. Check the Worker host and service status."});
			return;
		}
		success(document.object());
	});
#endif
}
