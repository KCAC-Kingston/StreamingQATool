#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QStringList>
#include <functional>

class SettingsStore {
public:
	QString load(const QString &filePath);
	QString save(const QJsonObject &updated);
	const QJsonObject &values() const { return data; }
	QStringList startupLogLines() const;
	QString logFolder() const;

private:
	QString path;
	QJsonObject data;
};

class SettingsDialog : public QDialog {
public:
	SettingsDialog(SettingsStore &store, std::function<QString(const QJsonObject &)> validate,
		       std::function<void()> saved, QWidget *parent, const QStringList &scenes = {});
};
