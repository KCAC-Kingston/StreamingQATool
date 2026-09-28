#include "marker-decoder.h"
#include <QDate>
#include <cmath>

QJsonObject MarkerDecoder::bits(const QVector<int> &bits)
{
	if (bits.size() != 104)
		return {};
	for (int bit : bits)
		if (bit != 0 && bit != 1)
			return {};
	constexpr int positions[]{3, 5, 6, 7, 9, 10, 11, 12};
	int payload[8]{};
	for (int byte = 0; byte < 8; ++byte) {
		int word[14]{}, syndrome = 0, parity = 0;
		for (int i = 1; i <= 13; ++i) {
			word[i] = bits[(i - 1) * 8 + byte];
			parity ^= word[i];
			if (i < 13 && word[i])
				syndrome ^= i;
		}
		if (syndrome && (!parity || syndrome > 12))
			return {};
		if (syndrome)
			word[syndrome] ^= 1;
		for (int pos : positions)
			payload[byte] = (payload[byte] << 1) | word[pos];
	}
	quint16 crc = 0xffff;
	for (int i = 0; i < 6; ++i) {
		crc ^= quint16(payload[i] << 8);
		for (int b = 0; b < 8; ++b)
			crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
	}
	if (payload[0] != 0xb7 || crc != ((payload[6] << 8) | payload[7]))
		return {};
	const QStringList sections{"theme",      "call-to-worship", "scripture-reading", "golden-verse", "sermon-start",
				   "sermon-end", "offering-verse",  "announcement",      "doxology",     "lords-prayer",
				   "welcome",    "worship-song",    "response-song"};
	if (payload[1] == 2) {
		if (payload[2] || payload[3] || payload[4] != 255 || payload[5] < 12 || payload[5] > 13)
			return {};
		return {{"section", sections[payload[5] - 1]}};
	}
	if (payload[1] != 1 || payload[4] > 3 || payload[5] < 1 || payload[5] > 11)
		return {};
	const QStringList services{"chinese", "english", "combined", "special"};
	return {{"section", sections[payload[5] - 1]},
		{"serviceKind", services[payload[4]]},
		{"serviceDate", QDate(2020, 1, 1).addDays(payload[2] * 256 + payload[3]).toString(Qt::ISODate)}};
}

QJsonObject MarkerDecoder::pixels(const QImage &image, const QRect &box)
{
	if (box.width() < 112 || box.height() < 8 || !image.rect().contains(box))
		return {};
	double cells[4][56]{};
	auto sample = [&](int row, int col) {
		int x0 = int(std::floor((col + .25) * box.width() / 56)),
		    x1 = int(std::ceil((col + .75) * box.width() / 56));
		int y0 = int(std::floor((row + .25) * box.height() / 4)),
		    y1 = int(std::ceil((row + .75) * box.height() / 4));
		double sum = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) {
				const auto color = image.pixel(x + box.x(), y + box.y());
				sum += .2126 * qRed(color) + .7152 * qGreen(color) + .0722 * qBlue(color);
			}
		return sum / ((x1 - x0) * (y1 - y0));
	};
	const double dark = sample(0, 0), light = sample(0, 1), threshold = (dark + light) / 2;
	if (light - dark < 18)
		return {};
	constexpr int anchors[4][4]{{1, 0, 0, 1}, {1, 1, 0, 0}, {1, 0, 1, 1}, {0, 0, 0, 1}};
	constexpr int columns[]{0, 1, 54, 55};
	for (int y = 0; y < 4; ++y)
		for (int x = 0; x < 4; ++x)
			if ((sample(y, columns[x]) < threshold) != bool(anchors[y][x]))
				return {};
	for (int y = 0; y < 4; ++y)
		for (int x = 2; x < 54; ++x)
			cells[y][x] = sample(y, x);
	QVector<int> values;
	values.reserve(104);
	for (int row : {0, 2})
		for (int x = 2; x < 54; ++x)
			values.append(cells[row][x] < cells[row + 1][x]);
	return bits(values);
}

QJsonObject MarkerDecoder::find(const QImage &image)
{
	if (image.isNull())
		return {};
	const double width = image.width(), height = image.height();
	for (double ratio : {16.0 / 9, 4.0 / 3, width / height}) {
		const double sw = qMin(width, height * ratio), sh = qMin(height, width / ratio);
		const int x = int(std::nearbyint((width - sw) / 2 + sw * 9.15 / 13.333)),
			  y = int(std::nearbyint((height - sh) / 2 + sh * 7.175 / 7.5));
		for (double scale : {1.0, .995, 1.005})
			for (int dy : {0, -1, 1, -2, 2})
				for (int dx : {0, -1, 1, -2, 2}) {
					const QRect box(x + dx, y + dy, int(std::nearbyint(sw * 3.5 / 13.333 * scale)),
							int(std::nearbyint(sh * .25 / 7.5 * scale)));
					const auto marker = pixels(image, box);
					if (!marker.isEmpty())
						return marker;
				}
	}
	return {};
}

QJsonObject MarkerDecoder::find(const QImage &image, const QVector<QRect> &regions)
{
	auto marker = find(image);
	if (!marker.isEmpty())
		return marker;
	for (const auto &region : regions) {
		// Only inspect pixels actually visible in program output, never hidden source pixels.
		// A clipped viewport no longer has the reference marker geometry.
		if (!image.rect().contains(region) || region == image.rect())
			continue;
		marker = find(image.copy(region));
		if (!marker.isEmpty())
			return marker;
	}
	return {};
}
