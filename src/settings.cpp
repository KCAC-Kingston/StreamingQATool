#include "settings.h"
#include "service-manager-client.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QDesktopServices>
#include <QPushButton>
#include <QUrl>

namespace {
struct Field {
	const char *key;
	const char *label;
	bool port;
};
constexpr Field fields[] = {
	{"serviceManagerHost", "Service Manager Host", false},
	{"serviceManagerApiKey", "Service Manager API Key", false},
	{"companionHost", "Bitfocus Companion Host", false},
	{"companionPort", "Bitfocus Companion Port", true},
	{"openlpHost", "OpenLP Host", false},
	{"openlpPort", "OpenLP Port", true},
	{"endStreamingDelay", "End Streaming Delay (seconds)", false},
	{"logRetentionDays", "Log retention (days)", false},
};
} // namespace

QString SettingsStore::load(const QString &filePath)
{
	path = filePath;
	const QJsonObject defaults{{"endStreamingDelay", "30"}, {"companionHost", "localhost"},
				   {"companionPort", "8000"},   {"openlpHost", "localhost"},
				   {"openlpPort", "4316"},      {"openlpVersion", "v3"},
				   {"companionEnabled", true},  {"openlpEnabled", true},
				   {"logRetentionDays", "90"}};
	data = defaults;
	QFile file(path);
	if (!file.exists())
		return {};
	if (!file.open(QIODevice::ReadOnly))
		return "Could not read settings.json; using empty settings.";
	QJsonParseError error;
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject())
		return "Could not parse settings.json; using empty settings.";
	data = document.object();
	for (auto it = defaults.begin(); it != defaults.end(); ++it)
		if (!data.contains(it.key()) || (data[it.key()].isString() && data[it.key()].toString().isEmpty()))
			data.insert(it.key(), it.value());
	if (data["openlpVersion"].toString() != "v2" && data["openlpVersion"].toString() != "v3")
		data.insert("openlpVersion", "v3");
	bool retentionValid = false;
	const int retention = data["logRetentionDays"].toString().toInt(&retentionValid);
	if (!retentionValid || retention < 1 || retention > 3650)
		data.insert("logRetentionDays", "90");
	bool valid = false;
	const int delay = data["endStreamingDelay"].toString().toInt(&valid);
	if (!valid || delay < 0 || delay > 3600)
		data.insert("endStreamingDelay", "30");
	return {};
}

QString SettingsStore::logFolder() const
{
	return QDir(QFileInfo(path).absolutePath()).filePath("logs");
}

QString SettingsStore::save(const QJsonObject &updated)
{
	if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
		return "Could not create the settings folder. Settings were not saved.";
	QSaveFile file(path);
	const auto bytes = QJsonDocument(updated).toJson();
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
		return "Could not save settings: " + file.errorString();
	data = updated;
	return {};
}

QStringList SettingsStore::startupLogLines() const
{
	QStringList lines;
	for (const auto &field : fields) {
		QString value = data[field.key].toString();
		if (QString::fromUtf8(field.key) == "serviceManagerApiKey" && !value.isEmpty())
			value = "[redacted]";
		value.replace('\r', ' ').replace('\n', ' ');
		lines.append(QString("%1: %2").arg(field.label, value.isEmpty() ? "(not set)" : value));
	}
	lines.append("OpenLP version: " + data["openlpVersion"].toString());
	lines.append(QString("OpenLP logging: %1; Companion logging: %2")
			     .arg(data["openlpEnabled"].toBool() ? "enabled" : "disabled",
				  data["companionEnabled"].toBool() ? "enabled" : "disabled"));
	return lines;
}

SettingsDialog::SettingsDialog(SettingsStore &store, std::function<QString(const QJsonObject &)> validate,
			       std::function<void()> saved, QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle("StreamingQATool Settings");
	setAttribute(Qt::WA_DeleteOnClose);
	setModal(false);
	resize(520, 360);
	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout;
	layout->addLayout(form);
	QList<QLineEdit *> inputs;
	auto *openlpVersion = new QComboBox(this);
	openlpVersion->setObjectName("openlpVersion");
	openlpVersion->addItem("OpenLP 3.0 (WebSocket)", "v3");
	openlpVersion->addItem("OpenLP 2.4 (HTTP polling)", "v2");
	openlpVersion->setCurrentIndex(openlpVersion->findData(store.values()["openlpVersion"].toString()));
	auto *companionEnabled = new QCheckBox("Log Companion key presses", this);
	companionEnabled->setChecked(store.values()["companionEnabled"].toBool(true));
	auto *openlpEnabled = new QCheckBox("Log OpenLP slide changes", this);
	openlpEnabled->setChecked(store.values()["openlpEnabled"].toBool(true));
	form->addRow(companionEnabled);
	form->addRow(openlpEnabled);
	form->addRow("OpenLP version", openlpVersion);
	for (const auto &field : fields) {
		auto *input = new QLineEdit(store.values()[field.key].toString(), this);
		input->setObjectName(field.key);
		if (QString::fromUtf8(field.key) == "serviceManagerApiKey")
			input->setEchoMode(QLineEdit::Password);
		if (field.port)
			input->setPlaceholderText("1-65535");
		if (QString::fromUtf8(field.key) == "endStreamingDelay")
			input->setPlaceholderText("30 (0-3600 seconds)");
		if (QString::fromUtf8(field.key) == "serviceManagerHost")
			input->setPlaceholderText("servicemanager.example.com (HTTPS is automatic)");
		form->addRow(QString::fromUtf8(field.label), input);
		inputs.append(input);
	}
	auto *status = new QLabel(this);
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	layout->addWidget(status);
	auto *folder = new QPushButton("Open Log Folder", this);
	layout->addWidget(folder);
	connect(folder, &QPushButton::clicked, this, [&store, status]() {
		if (!QDir().mkpath(store.logFolder()) ||
		    !QDesktopServices::openUrl(QUrl::fromLocalFile(store.logFolder())))
			status->setText("Could not open the log folder.");
	});
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(buttons, &QDialogButtonBox::accepted, this,
		[this, &store, inputs, status, validate, saved, openlpVersion, openlpEnabled, companionEnabled]() {
			QJsonObject updated;
			for (int i = 0; i < inputs.size(); ++i) {
				const auto &field = fields[i];
				const QString fieldKey = QString::fromUtf8(field.key);
				QString value = inputs[i]->text().trimmed();
				const bool delay = fieldKey == "endStreamingDelay";
				const bool retention = fieldKey == "logRetentionDays";
				if (field.port && value.isEmpty())
					value = fieldKey == "companionPort" ? "8000" : "4316";
				if (delay || retention || field.port) {
					bool valid = false;
					const int number = value.toInt(&valid);
					const int minimum = delay ? 0 : 1;
					const int maximum = delay ? 3600 : retention ? 3650 : 65535;
					if (!valid || number < minimum || number > maximum) {
						status->setText(QString("%1 must be between %2 and %3.")
									.arg(field.label)
									.arg(minimum)
									.arg(maximum));
						inputs[i]->setFocus();
						return;
					}
					value = QString::number(number);
				}
				if (fieldKey == "openlpHost" || fieldKey == "companionHost") {
					if (value.isEmpty())
						value = "localhost";
					QUrl local(value.contains("://") ? value : "http://" + value);
					if (!local.isValid() || local.host().isEmpty() || !local.userInfo().isEmpty() ||
					    local.hasQuery() || local.hasFragment() ||
					    (!local.path().isEmpty() && local.path() != "/")) {
						status->setText(
							"Enter a hostname or IP address for OpenLP and Companion.");
						return;
					}
					value = local.host();
				}
				if (fieldKey == "serviceManagerHost" && !value.isEmpty()) {
					value = ServiceManagerClient::normalizeHost(value);
					if (!ServiceManagerClient::validHost(value)) {
						status->setText(
							"Enter the Worker domain, for example servicemanager.example.com, without an API path or credentials.");
						return;
					}
				}
				updated.insert(field.key, value);
			}
			updated.insert("openlpVersion", openlpVersion->currentData().toString());
			updated.insert("companionEnabled", companionEnabled->isChecked());
			updated.insert("openlpEnabled", openlpEnabled->isChecked());
			QString error = validate(updated);
			if (error.isEmpty())
				error = store.save(updated);
			if (!error.isEmpty()) {
				status->setText(error);
				return;
			}
			saved();
			accept();
		});
}
