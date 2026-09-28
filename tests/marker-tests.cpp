#include "marker-decoder.h"
#include "marker-tracker.h"
#include "event-format.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <QPainter>

static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}
static QImage raster(const QJsonObject &fixture, int w, int h)
{
	QImage image(w, h, QImage::Format_RGB32);
	const auto cells = fixture["cells"].toArray();
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			image.setPixel(x, y,
				       cells[y * 4 / h].toArray()[x * 56 / w].toInt() ? qRgb(111, 145, 175)
										      : qRgb(215, 231, 247));
	return image;
}
int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	try {
		QFile file(QStringLiteral(MARKER_FIXTURES));
		check(file.open(QIODevice::ReadOnly), "fixture read");
		const auto fixtures = QJsonDocument::fromJson(file.readAll()).array();
		check(fixtures.size() == 46, "all encoder fixtures present");
		for (const auto &value : fixtures) {
			const auto fixture = value.toObject();
			const auto expected = fixture["marker"].toObject();
			QVector<int> bits;
			for (const auto &bit : fixture["bits"].toArray())
				bits.append(bit.toInt());
			check(MarkerDecoder::bits(bits) == expected, "generator payload");
			const auto image = raster(fixture, 336, 24);
			check(MarkerDecoder::pixels(image, image.rect()) == expected, "generator raster");
			for (int size : {1, 8})
				for (int start = 0; start <= 104 - size; ++start) {
					auto corrupt = bits;
					for (int i = start; i < start + size; ++i)
						corrupt[i] ^= 1;
					check(MarkerDecoder::bits(corrupt) == expected, "Hamming correction");
				}
			bits[0] ^= 1;
			bits[8] ^= 1;
			check(MarkerDecoder::bits(bits).isEmpty(), "double-bit corruption rejected");
		}
		const auto fixture = fixtures[4].toObject();
		for (const QSize size : {QSize(1280, 720), QSize(1024, 768), QSize(1920, 1080)})
			for (double ratio : {16.0 / 9, 4.0 / 3}) {
				QImage image(size, QImage::Format_RGB32);
				image.fill(Qt::black);
				double sw = qMin(double(size.width()), size.height() * ratio),
				       sh = qMin(double(size.height()), size.width() / ratio);
				int x = int(std::nearbyint((size.width() - sw) / 2 + sw * 9.15 / 13.333)) + 1;
				int y = int(std::nearbyint((size.height() - sh) / 2 + sh * 7.175 / 7.5)) - 1;
				auto marker = raster(fixture, int(std::nearbyint(sw * 3.5 / 13.333)),
						     int(std::nearbyint(sh * .25 / 7.5)));
				for (int j = 0; j < marker.height(); ++j)
					for (int i = 0; i < marker.width(); ++i)
						image.setPixel(x + i, y + j, marker.pixel(i, j));
				check(MarkerDecoder::find(image) == fixture["marker"].toObject(),
				      "letterbox and offsets");
			}
		QImage blank(1280, 720, QImage::Format_RGB32);
		blank.fill(Qt::gray);
		check(MarkerDecoder::find(blank).isEmpty(), "blank rejected");
		auto tiny = raster(fixture, 56, 4);
		check(MarkerDecoder::pixels(tiny, tiny.rect()).isEmpty(), "minimum size");
		for (const auto &value : fixtures) {
			const auto f = value.toObject();
			auto small = raster(f, 672, 48).scaled(112, 8, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			check(MarkerDecoder::pixels(small, small.rect()) == f["marker"].toObject(),
			      "two pixels per cell with smooth scaling");
		}
		for (QRect region : {QRect(141, 71, 853, 480), QRect(77, 43, 640, 360), QRect(800, 460, 480, 270),
				     QRect(80, 100, 600, 450), QRect(40, 90, 640, 270), QRect(950, 100, 480, 600)}) {
			QImage output(1920, 1080, QImage::Format_RGB32);
			output.fill(QColor(160, 170, 130));
			QImage slide(region.size(), QImage::Format_RGB32);
			slide.fill(Qt::white);
			const int x = int(std::nearbyint(region.width() * 9.15 / 13.333));
			const int y = int(std::nearbyint(region.height() * 7.175 / 7.5));
			auto code = raster(fixture, 672, 48)
					    .scaled(int(std::nearbyint(region.width() * 3.5 / 13.333)),
						    int(std::nearbyint(region.height() * .25 / 7.5)),
						    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			{
				QPainter painter(&slide);
				painter.drawImage(x, y, code);
			}
			{
				QPainter painter(&output);
				painter.drawImage(region.topLeft(), slide);
			}
			check(MarkerDecoder::find(output, {region}) == fixture["marker"].toObject(),
			      "small repositioned program slide");
			{
				QPainter painter(&output);
				painter.fillRect(QRect(region.topLeft() + QPoint(x, y), code.size()), Qt::black);
			}
			check(MarkerDecoder::find(output, {region}).isEmpty(),
			      "covered marker must not be detected from original source");
		}
		MarkerTracker tracker;
		auto hymn = fixtures.last().toObject()["marker"].toObject();
		const auto wall = QDateTime::fromString("2026-09-27T15:00:00Z", Qt::ISODate);
		check(tracker.observe({}, 0, wall).isEmpty() && tracker.observe({}, 1000, wall).isEmpty(),
		      "no false initial disappearance");
		check(tracker.observe(hymn, 1100, wall).isEmpty() && tracker.observe(hymn, 1200, wall).isEmpty(),
		      "three readings required");
		auto events = tracker.observe(hymn, 1300, wall.addMSecs(200));
		check(events.size() == 1 && events[0].timestamp == wall && events[0].elapsedMs == 1100 &&
			      !events[0].data.contains("serviceDate"),
		      "first observation time/no invented hymn metadata");
		check(tracker.observe({}, 1400, wall).isEmpty() && tracker.observe({}, 2300, wall).isEmpty(),
		      "absence debounce");
		events = tracker.observe({}, 2400, wall);
		check(events.size() == 1 && events[0].name == "marker.disappeared" && events[0].data == hymn &&
			      events[0].elapsedMs == 1400,
		      "disappearance retains marker data");
		tracker.observe(hymn, 2500, wall);
		tracker.observe(hymn, 2600, wall);
		check(tracker.observe(hymn, 2700, wall).size() == 1, "same marker returning");
		auto next = fixture["marker"].toObject();
		tracker.observe(next, 2800, wall);
		tracker.observe(next, 2900, wall);
		events = tracker.observe(next, 3000, wall);
		check(events.size() == 2 && events[0].name == "marker.disappeared" &&
			      events[1].name == "marker.appeared",
		      "replacement logs old and new marker");
		check(EventFormat::details("marker.appeared", next) == next, "saved marker fields retained");
		fprintf(stdout,
			"PASS: 46 generator fixtures, error correction, pixels, letterboxing, event timing and marker data\n");
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}
}
