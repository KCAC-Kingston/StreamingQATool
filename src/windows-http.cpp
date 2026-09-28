#include "windows-http.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <QElapsedTimer>
#include <QThread>

namespace {
class InternetHandle {
public:
	explicit InternetHandle(HINTERNET value) : value(value) {}
	~InternetHandle()
	{
		if (value)
			WinHttpCloseHandle(value);
	}
	InternetHandle(const InternetHandle &) = delete;
	InternetHandle &operator=(const InternetHandle &) = delete;
	operator HINTERNET() const { return value; }

private:
	HINTERNET value;
};

WindowsHttpResult transportError(DWORD code = GetLastError())
{
	WindowsHttpResult result;
	switch (code) {
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
		result.error = "Cannot resolve Service Manager. Check internet access and the configured host.";
		break;
	case ERROR_WINHTTP_CANNOT_CONNECT:
	case ERROR_WINHTTP_CONNECTION_ERROR:
		result.error =
			"Cannot connect to Service Manager. Check internet access and whether the service is running.";
		break;
	case ERROR_WINHTTP_TIMEOUT:
		result.error = "Service Manager timed out. Status will retry automatically.";
		break;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
		result.error = "TLS certificate verification failed. Check the server certificate and system clock.";
		result.retryable = false;
		break;
	default:
		result.error =
			QString("Connection to Service Manager failed (Windows error %1). Check your network and retry.")
				.arg(code);
	}
	return result;
}
} // namespace

WindowsHttpResult windowsHttpRequest(const QUrl &url, const QByteArray &method, const QByteArray &apiKey, int timeoutMs,
				     const QByteArray &payload, const QByteArray &contentType)
{
	QElapsedTimer deadline;
	deadline.start();
	InternetHandle session(WinHttpOpen(L"StreamingQATool/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
					   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session)
		return transportError();
	if (!WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs))
		return transportError();
	const bool secure = url.scheme() == "https";
	const auto host = url.host().toStdWString();
	InternetHandle connection(WinHttpConnect(session, host.c_str(), INTERNET_PORT(url.port(secure ? 443 : 80)), 0));
	if (!connection)
		return transportError();
	QString resource = url.path(QUrl::FullyEncoded);
	if (resource.isEmpty())
		resource = "/";
	if (url.hasQuery())
		resource += '?' + url.query(QUrl::FullyEncoded);
	const auto target = resource.toStdWString();
	const auto verb = QString::fromLatin1(method).toStdWString();
	InternetHandle request(WinHttpOpenRequest(connection, verb.c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER,
						  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
	if (!request)
		return transportError();
	DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)))
		return transportError();
	// Do not negotiate Windows account credentials with the Worker or a proxy.
	DWORD autoLogon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
	if (!WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autoLogon, sizeof(autoLogon)))
		return transportError();
	const auto headers = (QString("Content-Type: ") + QString::fromLatin1(contentType) +
			      "\r\nX-API-Key: " + QString::fromUtf8(apiKey) + "\r\n")
				     .toStdWString();
	QByteArray body = method == "POST" ? payload : QByteArray();
	if (!WinHttpSendRequest(request, headers.c_str(), DWORD(headers.size()), body.isEmpty() ? nullptr : body.data(),
				DWORD(body.size()), DWORD(body.size()), 0) ||
	    !WinHttpReceiveResponse(request, nullptr))
		return transportError();
	DWORD code = 0;
	DWORD codeSize = sizeof(code);
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				 WINHTTP_HEADER_NAME_BY_INDEX, &code, &codeSize, WINHTTP_NO_HEADER_INDEX))
		return transportError();
	WindowsHttpResult result;
	result.status = int(code);
	// Error bodies are unnecessary and can contain sensitive data.
	if (code < 200 || code >= 300)
		return result;
	char buffer[16384];
	for (;;) {
		if (deadline.elapsed() >= timeoutMs || QThread::currentThread()->isInterruptionRequested())
			return transportError(ERROR_WINHTTP_TIMEOUT);
		const int remaining = qMax(1, timeoutMs - int(deadline.elapsed()));
		if (!WinHttpSetTimeouts(request, remaining, remaining, remaining, remaining))
			return transportError();
		DWORD received = 0;
		if (!WinHttpReadData(request, buffer, sizeof(buffer), &received))
			return transportError();
		if (!received)
			break;
		result.body.append(buffer, int(received));
		if (result.body.size() > 2 * 1024 * 1024) {
			result.error = "Service Manager returned an unexpectedly large response.";
			result.retryable = false;
			result.body.clear();
			break;
		}
	}
	return result;
}
