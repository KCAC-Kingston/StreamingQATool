#include "local-websocket.h"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QtEndian>

LocalWebSocket::LocalWebSocket(QObject *parent) : QObject(parent)
{
	handshakeTimeout.setSingleShot(true);
	connect(&handshakeTimeout, &QTimer::timeout, this, [this]() { reject("WebSocket handshake timed out."); });
	heartbeat.setInterval(20000);
	connect(&heartbeat, &QTimer::timeout, this, [this]() {
		if (awaitingPong) {
			reject("WebSocket heartbeat timed out.");
			return;
		}
		awaitingPong = true;
		sendFrame(9, "StreamingQATool");
	});
	connect(&socket, &QTcpSocket::connected, this, [this]() {
		QByteArray random(16, '\0');
		for (auto &byte : random)
			byte = char(QRandomGenerator::global()->generate());
		nonce = random.toBase64();
		QByteArray path = target.path(QUrl::FullyEncoded).toUtf8();
		if (path.isEmpty())
			path = "/";
		if (target.hasQuery())
			path += '?' + target.query(QUrl::FullyEncoded).toUtf8();
		QString host = target.host();
		if (host.contains(':'))
			host = '[' + host + ']';
		socket.write(
			"GET " + path + " HTTP/1.1\r\nHost: " + host.toUtf8() + ':' +
			QByteArray::number(target.port(80)) +
			"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
			nonce + "\r\n\r\n");
	});
	connect(&socket, &QTcpSocket::readyRead, this, &LocalWebSocket::receive);
	connect(&socket, &QTcpSocket::disconnected, this, [this]() {
		ready = false;
		heartbeat.stop();
		handshakeTimeout.stop();
		emit disconnected();
	});
	connect(&socket, &QTcpSocket::errorOccurred, this,
		[this](QAbstractSocket::SocketError) { reject(socket.errorString()); });
}

void LocalWebSocket::open(const QUrl &url)
{
	close();
	target = url;
	if (url.scheme() != "ws" || url.host().isEmpty() || !url.userInfo().isEmpty()) {
		emit error("Enter a local WebSocket host using ws://.");
		return;
	}
	handshakeTimeout.start(5000);
	socket.connectToHost(url.host(), quint16(url.port(80)));
}

void LocalWebSocket::close()
{
	heartbeat.stop();
	handshakeTimeout.stop();
	ready = false;
	socket.abort();
	input.clear();
	fragment.clear();
	fragmenting = awaitingPong = false;
}

void LocalWebSocket::reject(const QString &why)
{
	close();
	emit error(why);
}

void LocalWebSocket::sendFrame(quint8 opcode, const QByteArray &payload)
{
	if (!ready || payload.size() > 2 * 1024 * 1024)
		return;
	QByteArray frame;
	frame.append(char(0x80 | opcode));
	if (payload.size() < 126)
		frame.append(char(0x80 | payload.size()));
	else if (payload.size() <= 65535) {
		frame.append(char(0x80 | 126));
		frame.append(char(payload.size() >> 8));
		frame.append(char(payload.size()));
	} else {
		frame.append(char(0x80 | 127));
		for (int i = 7; i >= 0; --i)
			frame.append(char(quint64(payload.size()) >> (8 * i)));
	}
	const quint32 mask = QRandomGenerator::global()->generate();
	QByteArray maskBytes(reinterpret_cast<const char *>(&mask), 4);
	frame.append(maskBytes);
	for (qsizetype i = 0; i < payload.size(); ++i)
		frame.append(char(payload[i] ^ maskBytes[i % 4]));
	socket.write(frame);
}

void LocalWebSocket::sendText(const QByteArray &text)
{
	sendFrame(1, text);
}

void LocalWebSocket::receive()
{
	input += socket.readAll();
	if (input.size() > 4 * 1024 * 1024) {
		reject("WebSocket input limit exceeded.");
		return;
	}
	if (!ready) {
		const auto end = input.indexOf("\r\n\r\n");
		if (end < 0) {
			if (input.size() > 16384)
				reject("Invalid WebSocket handshake.");
			return;
		}
		const auto lines = input.left(end).split('\n');
		const auto expected = QCryptographicHash::hash(nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
							       QCryptographicHash::Sha1)
					      .toBase64();
		QByteArray accept;
		bool upgrade = false;
		bool connection = false;
		for (const auto &line : lines) {
			const auto separator = line.indexOf(':');
			const auto key = line.left(separator).trimmed().toLower();
			const auto value = line.mid(separator + 1).trimmed();
			if (key == "sec-websocket-accept")
				accept = value;
			if (key == "upgrade")
				upgrade = value.toLower() == "websocket";
			if (key == "connection")
				connection = value.toLower().contains("upgrade");
		}
		if (!lines.first().startsWith("HTTP/1.1 101 ") || accept != expected || !upgrade || !connection) {
			reject("WebSocket upgrade rejected. Check the host, port, and access permissions.");
			return;
		}
		input.remove(0, end + 4);
		ready = true;
		handshakeTimeout.stop();
		heartbeat.start();
		emit opened();
	}
	while (ready && input.size() >= 2) {
		const quint8 first = quint8(input[0]);
		const quint8 second = quint8(input[1]);
		const int opcode = first & 15;
		const bool final = (first & 128) != 0;
		quint64 length = second & 127;
		int header = 2;
		if ((first & 0x70) || (second & 0x80)) {
			reject("Unsupported WebSocket frame.");
			return;
		}
		if (length == 126) {
			if (input.size() < 4)
				return;
			length = qFromBigEndian<quint16>(input.constData() + 2);
			header = 4;
		} else if (length == 127) {
			if (input.size() < 10)
				return;
			length = qFromBigEndian<quint64>(input.constData() + 2);
			header = 10;
		}
		if (length > 2 * 1024 * 1024 || (opcode >= 8 && (!final || length > 125))) {
			reject("Invalid WebSocket frame size.");
			return;
		}
		if (quint64(input.size()) < quint64(header) + length)
			return;
		const auto payload = input.mid(header, qsizetype(length));
		input.remove(0, header + qsizetype(length));
		if (opcode == 8) {
			sendFrame(8, payload);
			socket.disconnectFromHost();
			return;
		}
		if (opcode == 9) {
			sendFrame(10, payload);
			continue;
		}
		if (opcode == 10) {
			awaitingPong = false;
			continue;
		}
		if ((opcode != 0 && opcode != 1 && opcode != 2) || (opcode == 0 && !fragmenting) ||
		    ((opcode == 1 || opcode == 2) && fragmenting)) {
			reject("Unexpected WebSocket message type.");
			return;
		}
		fragment += payload;
		if (fragment.size() > 2 * 1024 * 1024) {
			reject("WebSocket message limit exceeded.");
			return;
		}
		fragmenting = !final;
		if (final) {
			const auto text = fragment;
			fragment.clear();
			emit message(text);
		}
	}
}

LocalWebSocket::~LocalWebSocket()
{
	socket.disconnect(this);
	close();
}
