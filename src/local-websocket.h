#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

// Plain ws:// for local OpenLP and Companion; no additional Qt TLS/WebSockets DLL.
class LocalWebSocket : public QObject {
	Q_OBJECT
public:
	explicit LocalWebSocket(QObject *parent = nullptr);
	~LocalWebSocket() override;
	void open(const QUrl &url);
	void close();
	void sendText(const QByteArray &text);
signals:
	void opened();
	void message(const QByteArray &text);
	void disconnected();
	void error(const QString &message);

private:
	void receive();
	void sendFrame(quint8 opcode, const QByteArray &payload);
	void reject(const QString &why);
	QTcpSocket socket;
	QTimer handshakeTimeout;
	QTimer heartbeat;
	QUrl target;
	QByteArray input;
	QByteArray fragment;
	QByteArray nonce;
	bool ready = false;
	bool fragmenting = false;
	bool awaitingPong = false;
};
