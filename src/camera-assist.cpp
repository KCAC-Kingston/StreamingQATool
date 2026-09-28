#include "camera-assist.h"
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QEvent>

CameraAssist::CameraAssist(SwitchScene callback, QWidget *parent) : QFrame(parent), switchScene(std::move(callback))
{
	setObjectName("cameraAssist");
	setFrameShape(QFrame::StyledPanel);
	setAttribute(Qt::WA_StyledBackground, true);
	setStyleSheet(
		"QFrame#cameraAssist { background-color: palette(window); border: 2px solid #558dcc; border-radius: 5px; } QFrame#cameraAssist QLabel { background: transparent; color: palette(window-text); border: none; } QFrame#cameraAssist QPushButton { padding: 3px 8px; }");
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);
	auto *title = new QLabel("Camera Assist", this);
	auto font = title->font();
	font.setBold(true);
	title->setFont(font);
	layout->addWidget(title);
	message = new QLabel(this);
	message->setObjectName("cameraAssistMessage");
	message->setWordWrap(true);
	message->setTextFormat(Qt::PlainText);
	layout->addWidget(message);
	auto *row = new QHBoxLayout;
	accept = new QPushButton("Switch", this);
	accept->setObjectName("cameraAssistSwitch");
	auto *close = new QPushButton("Dismiss", this);
	close->setObjectName("cameraAssistDismiss");
	// OBS themes can cap ordinary buttons at a single line. Reserve two lines
	// explicitly so the countdown remains visible in every prompt state.
	const int buttonHeight = fontMetrics().lineSpacing() * 2 + 12;
	const QString buttonStyle =
		QString("QPushButton { min-height: %1px; max-height: %1px; padding: 4px 6px; }").arg(buttonHeight);
	accept->setStyleSheet(buttonStyle);
	close->setStyleSheet(buttonStyle);
	accept->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	close->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	row->addWidget(accept);
	row->addWidget(close);
	layout->addLayout(row);
	connect(accept, &QPushButton::clicked, this, [this]() { apply(false); });
	connect(close, &QPushButton::clicked, this, [this]() {
		if (eventSink)
			eventSink("dismissed", {{"scene", returning ? slides : camera}});
		dismiss();
	});
	timer.setObjectName("cameraAssistCountdown");
	timer.setInterval(100);
	connect(&timer, &QTimer::timeout, this, &CameraAssist::updateCountdown);
	parent->installEventFilter(this);
	hide();
}

void CameraAssist::setActionArea(QWidget *primary, QWidget *overrides)
{
	primaryAction = primary;
	overrideAction = overrides;
	if (primary)
		primary->installEventFilter(this);
	if (overrides)
		overrides->installEventFilter(this);
}

void CameraAssist::configure(bool on, const QString &slideScene, const QString &cameraScene, int seconds)
{
	seconds = qBound(1, seconds, 3600);
	if (enabled == on && slides == slideScene && camera == cameraScene && delay == seconds)
		return;
	dismiss();
	endMarker = {};
	enabled = on;
	slides = slideScene;
	camera = cameraScene;
	delay = seconds;
}

void CameraAssist::dismiss()
{
	timer.stop();
	hide();
}
void CameraAssist::sceneChanged(const QString &scene)
{
	if (scene == currentScene)
		return;
	currentScene = scene;
	dismiss();
	if (currentScene != slides && currentScene != camera)
		endMarker = {};
}

void CameraAssist::markerEvent(const QString &event, const QJsonObject &data)
{
	if (!enabled || slides.isEmpty() || camera.isEmpty() || (currentScene != slides && currentScene != camera))
		return;
	const QString section = data["section"].toString();
	QJsonObject identity{{"section", section},
			     {"serviceDate", data["serviceDate"]},
			     {"serviceKind", data["serviceKind"]}};
	if (event == "marker.appeared" && section == "sermon-start") {
		endMarker = {};
		prompt(false);
	} else if (event == "marker.appeared" && section == "sermon-end") {
		dismiss();
		endMarker = identity;
	} else if (event == "marker.disappeared" && section == "sermon-end" && !endMarker.isEmpty() &&
		   identity == endMarker) {
		endMarker = {};
		prompt(true);
	}
}

void CameraAssist::prompt(bool returnToSlides)
{
	dismiss();
	returning = returnToSlides;
	if (currentScene == (returning ? slides : camera))
		return;
	accept->setText(returning ? "Switch to slides now" : "Switch to camera");
	if (returning) {
		elapsed.start();
		timer.start();
		updateCountdown();
	} else
		message->setText("Sermon started. Switch to \"" + camera + "\"?");
	show();
	position();
	raise();
}

void CameraAssist::updateCountdown()
{
	const int remaining = qMax(0, delay - int(elapsed.elapsed() / 1000));
	message->setText(QString("Sermon ended. Switch to \"%1\"? Dismiss to stay here.").arg(slides));
	accept->setText(QString("Switch to slides now\nAuto-switch in %1s").arg(remaining));
	if (remaining == 0)
		apply(true);
}

void CameraAssist::apply(bool automatic)
{
	if (!enabled || isHidden() || (currentScene != slides && currentScene != camera)) {
		dismiss();
		return;
	}
	timer.stop();
	const QString target = returning ? slides : camera;
	const QString error = switchScene(target);
	if (!error.isEmpty()) {
		accept->setText(returning ? "Retry switch to slides" : "Retry switch to camera");
		message->setText(error + "\nAutomatic switching stopped.");
		if (eventSink)
			eventSink("error", {{"scene", target}, {"message", error}});
		show();
		position();
		raise();
		return;
	}
	if (eventSink)
		eventSink("switched", {{"scene", target}, {"reason", automatic ? "countdown" : "user"}});
	dismiss();
}

void CameraAssist::position()
{
	QRect area(6, 32, qMax(160, parentWidget()->width() - 12), 0);
	if (primaryAction && overrideAction && primaryAction->isVisible() && overrideAction->isVisible()) {
		area = QRect(primaryAction->mapTo(parentWidget(), QPoint()), primaryAction->size());
		area = area.united(QRect(overrideAction->mapTo(parentWidget(), QPoint()), overrideAction->size()));
	}
	const int width = area.width();
	setFixedWidth(width);
	layout()->invalidate();
	const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : sizeHint().height();
	setFixedHeight(qMax(height, area.height()));
	layout()->activate();
	move(area.topLeft());
}

bool CameraAssist::eventFilter(QObject *object, QEvent *event)
{
	if ((object == parentWidget() || object == primaryAction || object == overrideAction) &&
	    (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
	     event->type() == QEvent::LayoutRequest) &&
	    !isHidden())
		position();
	return QFrame::eventFilter(object, event);
}
