#include "marker-monitor.h"
#include "marker-decoder.h"
#include "marker-regions.h"
#include <obs.h>
#include <util/platform.h>

MarkerMonitor::MarkerMonitor(QObject *parent) : QObject(parent), worker(new QObject)
{
	worker->moveToThread(&thread);
	connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
	thread.start();
	origin = os_gettime_ns();
	obs_add_main_rendered_callback(rendered, this);
}

MarkerMonitor::~MarkerMonitor()
{
	obs_remove_main_rendered_callback(rendered, this);
	thread.quit();
	thread.wait();
	obs_enter_graphics();
	gs_stagesurface_destroy(surface);
	obs_leave_graphics();
}

void MarkerMonitor::rendered(void *data)
{
	static_cast<MarkerMonitor *>(data)->capture();
}

void MarkerMonitor::report(const QString &message)
{
	if (message == lastStatus)
		return;
	lastStatus = message;
	QMetaObject::invokeMethod(this, [this, message]() { emit statusChanged(message); }, Qt::QueuedConnection);
}

void MarkerMonitor::capture()
{
	const uint64_t now = os_gettime_ns();
	if (now - lastFrame < 100000000 || busy.load())
		return;
	lastFrame = now;
	const auto wall = QDateTime::currentDateTimeUtc();
	auto *texture = obs_get_main_texture();
	if (!texture) {
		report("Marker reader: no program frame");
		return;
	}
	const auto w = gs_texture_get_width(texture), h = gs_texture_get_height(texture);
	const auto f = gs_texture_get_color_format(texture);
	if (f != GS_RGBA && f != GS_BGRA && f != GS_RGBA_UNORM && f != GS_BGRA_UNORM) {
		report("Marker reader: requires SDR (8-bit) program output");
		return;
	}
	if (w != width || h != height || f != format || !surface) {
		gs_stagesurface_destroy(surface);
		surface = gs_stagesurface_create(w, h, f);
		width = w;
		height = h;
		format = f;
	}
	if (!surface) {
		report("Marker reader: frame capture unavailable");
		return;
	}
	gs_stage_texture(surface, texture);
	uint8_t *pixels = nullptr;
	uint32_t stride = 0;
	if (!gs_stagesurface_map(surface, &pixels, &stride)) {
		report("Marker reader: frame read failed");
		return;
	}
	const auto imageFormat = (f == GS_BGRA || f == GS_BGRA_UNORM) ? QImage::Format_ARGB32 : QImage::Format_RGBA8888;
	const QImage image = QImage(pixels, int(w), int(h), int(stride), imageFormat).copy();
	gs_stagesurface_unmap(surface);
	if (image.isNull()) {
		report("Marker reader: frame copy failed");
		return;
	}
	report("Marker reader: watching program output");
	const auto regions = markerSceneRegions();
	busy.store(true);
	const auto elapsed = qint64((now - origin) / 1000000);
	QMetaObject::invokeMethod(
		worker,
		[this, image, regions, wall, elapsed]() {
			const auto marker = MarkerDecoder::find(image, regions);
			++samples;
			if (!marker.isEmpty()) {
				++validSamples;
				lastSeen = wall.toLocalTime().toString("HH:mm:ss");
			}
			const auto events = tracker.observe(marker, elapsed, wall);
			for (const auto &entry : events) {
				if (entry.name == "marker.appeared")
					++appearances;
				else
					++disappearances;
				QMetaObject::invokeMethod(
					this,
					[this, entry]() {
						auto data = entry.data;
						data.insert("elapsedMs", entry.elapsedMs);
						emit event(entry.name, data, entry.timestamp);
					},
					Qt::QueuedConnection);
			}
			if (!events.isEmpty() || elapsed - lastStatsMs >= 1000) {
				lastStatsMs = elapsed;
				const QJsonObject stats{{"marker", tracker.current()},
							{"samples", samples},
							{"valid", validSamples},
							{"appearances", appearances},
							{"disappearances", disappearances},
							{"lastSeen", lastSeen}};
				QMetaObject::invokeMethod(
					this, [this, stats]() { emit statisticsChanged(stats); }, Qt::QueuedConnection);
			}
			busy.store(false);
		},
		Qt::QueuedConnection);
}
