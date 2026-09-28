#pragma once
#include <QDialog>
class EventLog;
class LogWindow : public QDialog {
public:
	LogWindow(EventLog &log, QWidget *parent);
};
