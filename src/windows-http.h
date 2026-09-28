#pragma once

#include <QByteArray>
#include <QUrl>

struct WindowsHttpResult {
	int status = 0;
	QByteArray body;
	QString error;
	bool retryable = true;
};

// Blocking transport; ServiceManagerClient always runs this on a worker thread.
// Uses Windows' certificate store and Schannel, independent of OBS's Qt plugins.
WindowsHttpResult windowsHttpRequest(const QUrl &url, const QByteArray &method, const QByteArray &apiKey,
				     int timeoutMs);
