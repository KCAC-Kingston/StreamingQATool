#pragma once
#include <QFrame>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>

class QLabel;
class QPushButton;
class CameraAssist : public QFrame {
public:
	using SwitchScene = std::function<QString(const QString &)>;
	CameraAssist(SwitchScene switchScene, QWidget *parent);
	void configure(bool enabled, const QString &slides, const QString &camera, int delay);
	void markerEvent(const QString &event, const QJsonObject &data);
	void sceneChanged(const QString &scene);
	std::function<void(const QString &, const QJsonObject &)> eventSink;

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	void dismiss();
	void prompt(bool returnToSlides);
	void updateCountdown();
	void apply(bool automatic);
	void position();
	SwitchScene switchScene;
	QLabel *message;
	QPushButton *accept;
	QTimer timer;
	QElapsedTimer elapsed;
	bool enabled = false, returning = false;
	QString slides, camera, currentScene;
	QJsonObject endMarker;
	int delay = 15;
};
