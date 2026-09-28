#include "service-manager-client.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>

// Opt-in, read-only connectivity check. Never prints credentials or service data.
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (argc != 2) {
		std::fprintf(stderr, "Usage: connection-check <settings.json>\n");
		return 2;
	}
	QFile file(QString::fromLocal8Bit(argv[1]));
	if (!file.open(QIODevice::ReadOnly))
		return 2;
	const auto settings = QJsonDocument::fromJson(file.readAll()).object();
#ifdef Q_OS_WIN
	QCoreApplication::setLibraryPaths({}); // Match OBS without Qt TLS plugins.
#endif
	ServiceManagerClient client;
	client.configure(settings["serviceManagerHost"].toString(), settings["serviceManagerApiKey"].toString());
	client.request(
		"/api/admin/v1/services?page=1&pageSize=1", "GET",
		[&](const QJsonObject &body) {
			if (!body["items"].isArray()) {
				std::fprintf(stderr, "FAIL: unexpected API response structure\n");
				app.exit(1);
				return;
			}
			std::puts(
				"PASS: HTTPS connection, API-key authentication, and service-list response (no Qt TLS plugins)");
			app.exit(0);
		},
		[&](const ApiError &error) {
			std::fprintf(stderr, "FAIL: %s\n", error.message.toUtf8().constData());
			app.exit(1);
		});
	return app.exec();
}
