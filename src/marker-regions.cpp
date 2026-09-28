#include "marker-regions.h"
#include <QTransform>
#include <obs.h>
#include <graphics/matrix4.h>
#include <cmath>

namespace {
struct Context {
	QVector<QRect> *regions;
	QTransform parent;
	int depth;
};
QTransform transform(const matrix4 &m)
{
	return {m.x.x, m.x.y, m.y.x, m.y.y, m.t.x, m.t.y};
}
bool collect(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto &ctx = *static_cast<Context *>(data);
	if (ctx.regions->size() >= 128)
		return false;
	if (!obs_sceneitem_visible(item))
		return true;
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	const auto mapped = transform(box) * ctx.parent;
	// Rotated/flipped markers are not part of the reference protocol's viewport model.
	if (std::abs(mapped.m12()) < .01 && std::abs(mapped.m21()) < .01 && mapped.m11() > 0 && mapped.m22() > 0) {
		const QRect bounds = mapped.mapRect(QRectF(0, 0, 1, 1)).toAlignedRect();
		if (bounds.width() >= 400 && bounds.height() >= 220 && !ctx.regions->contains(bounds))
			ctx.regions->append(bounds);
	}
	if (ctx.depth < 8) {
		auto *nested = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
							    : obs_scene_from_source(obs_sceneitem_get_source(item));
		if (nested) {
			matrix4 draw;
			obs_sceneitem_get_draw_transform(item, &draw);
			Context child{ctx.regions, transform(draw) * ctx.parent, ctx.depth + 1};
			obs_scene_enum_items(nested, collect, &child);
		}
	}
	return true;
}
} // namespace

QVector<QRect> markerSceneRegions()
{
	QVector<QRect> regions;
	auto *source = obs_get_output_source(0);
	if (!source)
		return regions;
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION) {
		auto *active = obs_transition_get_active_source(source);
		obs_source_release(source);
		source = active;
	}
	if (source) {
		Context context{&regions, {}, 0};
		if (auto *scene = obs_scene_from_source(source))
			obs_scene_enum_items(scene, collect, &context);
		obs_source_release(source);
	}
	return regions;
}
