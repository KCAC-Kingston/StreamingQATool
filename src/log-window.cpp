#include "log-window.h"
#include "event-log.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>

LogWindow::LogWindow(EventLog &log, QWidget *parent) : QDialog(parent)
{
	setWindowTitle("StreamingQATool — Event Log Stream");
	setAttribute(Qt::WA_DeleteOnClose);
	setModal(false);
	resize(1000, 500);
	auto *layout = new QVBoxLayout(this);
	auto *output = new QPlainTextEdit(this);
	output->setReadOnly(true);
	output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	output->setMaximumBlockCount(2000);
	output->setPlainText(log.recent().join('\n'));
	layout->addWidget(output);
	auto *pause = new QCheckBox("Pause display (logging continues)", this);
	layout->addWidget(pause);
	auto *status = new QLabel(log.error(), this);
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto *folder = new QPushButton("Open Log Folder", this);
	layout->addWidget(folder);
	connect(folder, &QPushButton::clicked, this,
		[&log]() { QDesktopServices::openUrl(QUrl::fromLocalFile(log.folder())); });
	connect(&log, &EventLog::storageError, status, &QLabel::setText);
	connect(&log, &EventLog::entryAdded, this, [output, pause](const QString &line) {
		if (pause->isChecked())
			return;
		const bool atBottom = output->verticalScrollBar()->value() == output->verticalScrollBar()->maximum();
		output->appendPlainText(line);
		if (atBottom)
			output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());
	});
	connect(pause, &QCheckBox::toggled, this, [output, &log](bool paused) {
		if (!paused)
			output->setPlainText(log.recent().join('\n'));
	});
}
