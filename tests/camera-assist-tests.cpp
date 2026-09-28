#include "camera-assist.h"
#include <QApplication>
#include <QPushButton>
#include <QEventLoop>
#include <QLabel>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char *why)
{
	if (!ok)
		throw std::runtime_error(why);
}
static void wait(int ms)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}
int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	try {
		QWidget parent;
		parent.resize(360, 500);
		parent.setStyleSheet(
			"QWidget { background: #262930; color: #eeeeee; } QPushButton { background: #3c404c; }");
		auto *underlay = new QLabel(
			"Underlying scene selector\nUnderlying YouTube links\nUnderlying streaming controls", &parent);
		underlay->setGeometry(10, 35, 330, 150);
		parent.show();
		QStringList switches;
		CameraAssist assist(
			[&](const QString &scene) {
				switches.append(scene);
				return QString();
			},
			&parent);
		QJsonObject start{{"section", "sermon-start"}}, end{{"section", "sermon-end"}};
		auto marker = [&](const char *event, const QJsonObject &data) {
			assist.markerEvent(event, data);
		};
		assist.sceneChanged("Slides");
		marker("marker.appeared", start);
		check(assist.isHidden(), "disabled by default");
		assist.configure(true, "Slides", "Camera", 1);
		assist.sceneChanged("Other");
		marker("marker.appeared", start);
		check(assist.isHidden(), "inactive on unassigned scene");
		assist.sceneChanged("Slides");
		marker("marker.appeared", start);
		check(!assist.isHidden() && switches.isEmpty(), "start requires user approval");
		if (app.arguments().size() > 1) {
			app.processEvents();
			check(parent.grab().save(app.arguments()[1]), "overlay screenshot");
		}
		assist.findChild<QPushButton *>("cameraAssistDismiss")->click();
		wait(1100);
		check(switches.isEmpty(), "dismissed start does not switch");
		marker("marker.appeared", start);
		assist.findChild<QPushButton *>("cameraAssistSwitch")->click();
		check(switches == QStringList{"Camera"}, "manual camera switch");
		assist.sceneChanged("Camera");
		marker("marker.disappeared", end);
		check(assist.isHidden(), "end must first be observed");
		marker("marker.appeared", end);
		check(assist.isHidden(), "no return while end marker visible");
		marker("marker.disappeared", end);
		check(!assist.isHidden(), "return countdown prompt");
		assist.findChild<QPushButton *>("cameraAssistDismiss")->click();
		wait(1100);
		check(switches.size() == 1, "dismiss cancels countdown");
		marker("marker.appeared", end);
		marker("marker.disappeared", end);
		assist.sceneChanged("Other");
		wait(1100);
		check(switches.size() == 1 && assist.isHidden(), "leaving assigned scenes cancels countdown");
		assist.sceneChanged("Camera");
		marker("marker.appeared", end);
		marker("marker.disappeared", end);
		wait(1200);
		check(switches == QStringList{"Camera", "Slides"}, "automatic return without streaming dependency");
		marker("marker.appeared", end);
		marker("marker.disappeared", end);
		assist.configure(false, "Slides", "Camera", 1);
		wait(1100);
		check(switches.size() == 2 && assist.isHidden(), "disabling cancels pending switch");
		fprintf(stdout, "PASS: Camera Assist gating, approval, dismissal, countdown, and scene changes\n");
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
