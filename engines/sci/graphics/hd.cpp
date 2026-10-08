/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/algorithm.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/formats/json.h"
#include "common/system.h"
#include "engines/util.h"
#include "graphics/cursorman.h"
#include "graphics/surface.h"
#include "image/png.h"
#include "sci/sci.h"
#include "sci/graphics/hd.h"

namespace Sci {

// ---------------------------------------------------------------------------------------------------------
// Pack

GfxHd *GfxHd::create() {
#ifdef USE_PNG
	Common::FSNode root;
	if (ConfMan.hasKey("hd_pack_path"))
		root = Common::FSNode(ConfMan.getPath("hd_pack_path"));
	else
		root = Common::FSNode(ConfMan.getPath("path")).getChild("hd");
	if (!root.exists() || !root.isDirectory())
		return nullptr;
	if (ConfMan.hasKey("hd_disable") && ConfMan.getBool("hd_disable"))
		return nullptr;

	int scale = 4;
	Common::FSNode manifest = root.getChild("manifest.json");
	if (manifest.exists()) {
		Common::SeekableReadStream *s = manifest.createReadStream();
		if (s) {
			Common::String text = s->readString(0, s->size());
			delete s;
			Common::JSONValue *v = Common::JSON::parse(text);
			if (v && v->isObject() && v->hasChild("scale") && v->child("scale")->isIntegerNumber())
				scale = (int)v->child("scale")->asIntegerNumber();
			delete v;
		}
	}
	if (scale < 1 || scale > 8) {
		warning("HD pack: unsupported scale %d, HD disabled", scale);
		return nullptr;
	}
	GfxHd *hd = new GfxHd(root, scale);
	debug("HD pack: %s, scale %d, %u files", root.getPath().toString().c_str(), scale, hd->_files.size());
	return hd;
#else
	return nullptr;
#endif
}

GfxHd::GfxHd(const Common::FSNode &root, int scale) : _root(root), _scale(scale), _nextRecord(1), _background(0) {
	indexFiles(root, "");
	_tweensEnabled = !ConfMan.hasKey("hd_tweens") || ConfMan.getBool("hd_tweens");
	_facesEnabled = !ConfMan.hasKey("hd_faces") || ConfMan.getBool("hd_faces");
	HdRecord none = { nullptr, 0, 0, 0, 0, false, false, -1 };
	_records.resize(0x10000);
	for (auto &r : _records)
		r = none;
	_recordKeys.resize(0x10000);
}

GfxHd::~GfxHd() {
	for (auto &i : _images)
		delete i._value;
}

void GfxHd::indexFiles(const Common::FSNode &dir, const Common::String &prefix) {
	Common::FSList list;
	if (!dir.getChildren(list, Common::FSNode::kListAll))
		return;
	for (const auto &node : list) {
		Common::String name = prefix + node.getName();
		if (node.isDirectory())
			indexFiles(node, name + "/");
		else
			_files[name] = true;
	}
}

const HdImage *GfxHd::findView(int view, int loop, int cel) {
	return load(Common::String::format("view/%d/%d.%d.png", view, loop, cel), Common::String::format("view/%d/palette.pal", view));
}

const HdImage *GfxHd::findPic(int pic, int cel) {
	return load(Common::String::format("pic/%d.%d.png", pic, cel), Common::String::format("pic/%d.pal", pic));
}

const HdImage *GfxHd::load(const Common::String &path, const Common::String &palPath) {
	if (!_files.contains(path))
		return nullptr;
	if (_images.contains(path))
		return _images[path];

	HdImage *img = nullptr;
#ifdef USE_PNG
	// Resolve the relative path one component at a time (FSNode has no multi-level getChild)
	Common::FSNode node = _root;
	for (const auto &part : Common::Path(path).splitComponents())
		node = node.getChild(part);
	Common::SeekableReadStream *s = node.createReadStream();
	Image::PNGDecoder dec;
	if (s && dec.loadStream(*s) && dec.getSurface()) {
		const Graphics::PixelFormat rgba(4, 8, 8, 8, 8, 24, 16, 8, 0);
		Graphics::Surface *conv = dec.getSurface()->convertTo(rgba, dec.getPalette().data(), dec.getPalette().size());
		img = new HdImage();
		img->w = conv->w;
		img->h = conv->h;
		img->px.resize(conv->w * conv->h);
		for (int y = 0; y < conv->h; y++)
			memcpy(&img->px[y * conv->w], conv->getBasePtr(0, y), conv->w * 4);
		conv->free();
		delete conv;

		img->hasRef = false;
		if (_files.contains(palPath)) {
			Common::FSNode palNode = _root;
			for (const auto &part : Common::Path(palPath).splitComponents())
				palNode = palNode.getChild(part);
			Common::SeekableReadStream *ps = palNode.createReadStream();
			if (ps && ps->read(img->refPal, sizeof(img->refPal)) == sizeof(img->refPal))
				img->hasRef = true;
			delete ps;
		}
	} else {
		warning("HD pack: could not load %s", path.c_str());
	}
	delete s;
#endif
	_images[path] = img;
	return img;
}

uint16 GfxHd::registerDraw(const HdImage *img, int16 left, int16 top, int16 dstW, int16 dstH, bool mirror, bool overlay) {
	// Pack the draw into a key; images are identified by their record-independent address
	const uint64 key = ((uint64)(uintptr)img << 32) ^ ((uint64)(uint16)left << 20) ^ ((uint64)(uint16)top << 8)
		^ ((uint64)dstW << 40) ^ ((uint64)dstH << 52) ^ (mirror ? 1ULL << 63 : 0) ^ (overlay ? 1ULL << 62 : 0);
	if (_recordIndex.contains(key)) {
		const HdRecord &r = _records[_recordIndex[key]];
		if (r.img == img && r.left == left && r.top == top && r.dstW == dstW && r.dstH == dstH && r.mirror == mirror
				&& r.overlay == overlay)
			return _recordIndex[key];
	}
	const uint16 id = _nextRecord;
	_nextRecord = _nextRecord == 0xFFFF ? 1 : _nextRecord + 1;
	if (_records[id].img && _recordIndex.contains(_recordKeys[id]) && _recordIndex[_recordKeys[id]] == id)
		_recordIndex.erase(_recordKeys[id]);
	HdRecord r = { img, left, top, dstW, dstH, mirror, overlay, -1 };
	_records[id] = r;
	_recordKeys[id] = key;
	_recordIndex[key] = id;
	return id;
}

// ---------------------------------------------------------------------------------------------------------
// Faces

bool GfxHd::hasFace(int view) {
	return _facesEnabled && loadFace(view) >= 0;
}

static void readAnim(const Common::JSONValue *v, HdFaceAnim &anim) {
	if (!v || !v->isObject())
		return;
	const Common::JSONObject &o = v->asObject();
	if (o.contains("fps") && o["fps"]->isNumber())
		anim.fps = MAX<float>(0.1f, (float)o["fps"]->asNumber());
	else if (o.contains("fps") && o["fps"]->isIntegerNumber())
		anim.fps = MAX<float>(0.1f, (float)o["fps"]->asIntegerNumber());
	if (o.contains("frames") && o["frames"]->isArray()) {
		for (const Common::JSONValue *f : o["frames"]->asArray()) {
			if (f->isString())
				anim.frames.push_back(f->asString());
		}
	}
}

int GfxHd::loadFace(int view) {
	if (_faceIndex.contains(view))
		return _faceIndex[view];
	_faceIndex[view] = -1;
	const Common::String path = Common::String::format("view/%d/face.json", view);
	if (!_files.contains(path))
		return -1;
	Common::FSNode node = _root;
	for (const auto &part : Common::Path(path).splitComponents())
		node = node.getChild(part);
	Common::SeekableReadStream *s = node.createReadStream();
	if (!s)
		return -1;
	Common::String text = s->readString(0, s->size());
	delete s;
	Common::JSONValue *v = Common::JSON::parse(text);
	if (!v || !v->isObject()) {
		warning("HD pack: bad %s", path.c_str());
		delete v;
		return -1;
	}
	const Common::JSONObject &o = v->asObject();
	HdFace face;
	face.view = view;
	if (o.contains("talk"))
		readAnim(o["talk"], face.talk);
	if (o.contains("idle"))
		readAnim(o["idle"], face.idle);
	if (o.contains("eyes") && o["eyes"]->isObject()) {
		for (const auto &e : o["eyes"]->asObject()) {
			if (e._value->isString())
				face.eyes[(int)e._key.asUint64()] = e._value->asString();
		}
	}
	if (o.contains("loops") && o["loops"]->isObject()) {
		const Common::JSONObject &l = o["loops"]->asObject();
		if (l.contains("bust") && l["bust"]->isIntegerNumber())
			face.bustLoop = (int)l["bust"]->asIntegerNumber();
		if (l.contains("mouth") && l["mouth"]->isIntegerNumber())
			face.mouthLoop = (int)l["mouth"]->asIntegerNumber();
		if (l.contains("eyes") && l["eyes"]->isIntegerNumber())
			face.eyesLoop = (int)l["eyes"]->asIntegerNumber();
	}
	if (o.contains("hold") && o["hold"]->isIntegerNumber())
		face.holdMs = (uint32)o["hold"]->asIntegerNumber();
	delete v;
	_faces.push_back(face);
	_faceIndex[view] = _faces.size() - 1;
	debug("HD pack: view %d has a face: %u talk, %u idle frames, %u eye layers", view, face.talk.frames.size(),
		face.idle.frames.size(), face.eyes.size());
	return _faceIndex[view];
}

uint16 GfxHd::noteFaceDraw(int view, int loop, int cel, int16 left, int16 top, int16 dstW, int16 dstH) {
	const int idx = _facesEnabled ? loadFace(view) : -1;
	if (idx < 0)
		return 0;
	HdFace &f = _faces[idx];
	const Common::String pal = Common::String::format("view/%d/palette.pal", view);
	if (loop == f.bustLoop) {
		const HdImage *img = findView(view, loop, cel);
		if (!img && !f.idle.frames.empty())
			img = load(f.idle.frames[0], pal);
		if (!img && !f.talk.frames.empty())
			img = load(f.talk.frames[0], pal);
		if (!img)
			return 0;
		const uint16 id = registerDraw(img, left, top, dstW, dstH, false);
		_records[id].face = idx;
		f.record = id;
		return id;
	}
	if ((loop != f.mouthLoop && loop != f.eyesLoop) || !f.record)
		return 0;
	// Only inside the bust it belongs to (the view might be drawn elsewhere too)
	const HdRecord &bust = _records[f.record];
	if (bust.face != idx || left < bust.left || top < bust.top || left + dstW > bust.left + bust.dstW
			|| top + dstH > bust.top + bust.dstH)
		return 0;
	if (loop == f.mouthLoop) {
		if (cel != f.mouthCel) {
			f.mouthCel = cel;
			f.mouthAt = g_system->getMillis();
		}
	} else {
		f.eyesCel = cel;
	}
	return f.record;
}

void GfxHd::updateFaces(uint32 now) {
	for (uint i = 0; i < _faces.size(); i++) {
		HdFace &f = _faces[i];
		if (!f.record || _records[f.record].face != (int16)i)
			continue;
		const bool talking = f.mouthCel != 0 || (f.mouthAt && now - f.mouthAt < f.holdMs);
		if (talking != f.talking || !f.since) {
			f.talking = talking;
			f.since = now;
		}
		const HdFaceAnim &anim = talking ? f.talk : f.idle;
		const Common::String pal = Common::String::format("view/%d/palette.pal", f.view);
		int frame = 999;
		f.base = nullptr;
		if (!anim.frames.empty()) {
			frame = (int)((uint64)(now - f.since) * (uint64)(anim.fps * 1000) / 1000000 % anim.frames.size());
			f.base = load(anim.frames[frame], pal);
		}
		if (!f.base)
			f.base = _records[f.record].img;
		f.layer = nullptr;
		if (f.eyesCel && f.eyes.contains(f.eyesCel))
			f.layer = load(f.eyes[f.eyesCel], pal);
		f.key = (talking ? 1000 : 2000) + frame + (f.layer ? f.eyesCel * 10000 : 0);
	}
}

// ---------------------------------------------------------------------------------------------------------
// In-betweens and actors

bool GfxHd::hasTweens(int view) {
	if (!_tweensEnabled)
		return false;
	loadTweens(view);
	return !_tweens[view].empty();
}

void GfxHd::loadTweens(int view) {
	if (_tweens.contains(view))
		return;
	Common::HashMap<Common::String, Common::Array<HdTween> > &byCel = _tweens[view];
	const Common::String path = Common::String::format("view/%d/tweens.json", view);
	if (!_files.contains(path))
		return;
	Common::FSNode node = _root;
	for (const auto &part : Common::Path(path).splitComponents())
		node = node.getChild(part);
	Common::SeekableReadStream *s = node.createReadStream();
	if (!s)
		return;
	Common::String text = s->readString(0, s->size());
	delete s;
	Common::JSONValue *v = Common::JSON::parse(text);
	if (!v || !v->isObject()) {
		warning("HD pack: bad %s", path.c_str());
		delete v;
		return;
	}
	for (const auto &cel : v->asObject()) {
		if (!cel._value->isArray())
			continue;
		Common::Array<HdTween> &list = byCel[cel._key];
		for (const Common::JSONValue *t : cel._value->asArray()) {
			if (!t->isObject() || !t->hasChild("file"))
				continue;
			HdTween tw;
			tw.pos = (float)t->asObject()["pos"]->asNumber();
			tw.w = (int16)t->asObject()["w"]->asIntegerNumber();
			tw.h = (int16)t->asObject()["h"]->asIntegerNumber();
			tw.ox = (int16)t->asObject()["ox"]->asIntegerNumber();
			tw.oy = (int16)t->asObject()["oy"]->asIntegerNumber();
			tw.file = t->asObject()["file"]->asString();
			list.push_back(tw);
		}
		Common::sort(list.begin(), list.end(), [](const HdTween &a, const HdTween &b) { return a.pos < b.pos; });
	}
	delete v;
	debug("HD pack: view %d has in-betweens for %u cels", view, byCel.size());
}

const Common::Array<HdTween> *GfxHd::tweens(int view, int loop, int cel) {
	loadTweens(view);
	const Common::String key = Common::String::format("%d.%d", loop, cel);
	return _tweens[view].contains(key) ? &_tweens[view][key] : nullptr;
}

void GfxHd::noteActor(uint32 actor, const HdActor &drawn) {
	const uint32 now = g_system->getMillis();
	HdActor &a = _actors[actor];
	const bool sameLoop = a.view == drawn.view && a.loop == drawn.loop && a.mirror == drawn.mirror;
	if (sameLoop && a.cel != drawn.cel) {
		const uint32 interval = now - a.changedAt;
		if (interval > 0 && interval < 1000)
			a.period = a.period ? (a.period + interval) / 2 : interval;
		// A jump (teleport, room change) isn't a step: don't ease across it
		a.hasPrev = ABS(drawn.ax - a.ax) <= 8 && ABS(drawn.ay - a.ay) <= 8;
		a.prevCel = a.cel;
		a.prevAx = a.ax;
		a.prevAy = a.ay;
		a.prevW = a.celW;
		a.prevH = a.celH;
		a.prevCelAx = a.celAx;
		a.prevCelAy = a.celAy;
		a.changedAt = now;
	} else if (!sameLoop) {
		a.hasPrev = false;
		a.changedAt = now;
	}
	a.view = drawn.view;
	a.loop = drawn.loop;
	a.cel = drawn.cel;
	a.cels = drawn.cels;
	a.mirror = drawn.mirror;
	a.ax = drawn.ax;
	a.ay = drawn.ay;
	a.celW = drawn.celW;
	a.celH = drawn.celH;
	a.celAx = drawn.celAx;
	a.celAy = drawn.celAy;
	a.priority = drawn.priority;
	a.record = drawn.record;
}

bool GfxHd::actorFrame(const HdActor &a, uint32 now, HdOverlayFrame &f) {
	const int n = _scale;
	f.flip = a.mirror;
	f.img = findView(a.view, a.loop, a.cel);
	f.w = a.celW;
	f.h = a.celH;
	f.ox = a.celAx;
	f.oy = a.celAy;
	f.hdX = a.ax * n;
	f.hdY = a.ay * n;
	f.key = -1;
	const float t = a.period ? (float)(now - a.changedAt) / a.period : 1.0f;
	if (!a.hasPrev || t >= 1.0f || a.cels < 2 || (a.prevCel + 1) % a.cels != a.cel)
		return f.img != nullptr;

	// Between the previous cel and this one: ease the position, and show the latest in-between reached
	f.hdX = (int)((a.prevAx + (a.ax - a.prevAx) * t) * n + 0.5f);
	f.hdY = (int)((a.prevAy + (a.ay - a.prevAy) * t) * n + 0.5f);
	const Common::Array<HdTween> *list = tweens(a.view, a.loop, a.prevCel);
	int k = -1;
	if (list) {
		for (uint i = 0; i < list->size(); i++) {
			if ((*list)[i].pos <= t)
				k = i;
		}
	}
	if (k < 0) {
		const HdImage *prev = findView(a.view, a.loop, a.prevCel);
		if (prev) {
			f.img = prev;
			f.w = a.prevW;
			f.h = a.prevH;
			f.ox = a.prevCelAx;
			f.oy = a.prevCelAy;
			f.key = -2;
		}
	} else {
		const HdTween &tw = (*list)[k];
		const HdImage *img = load(tw.file, Common::String::format("view/%d/palette.pal", a.view));
		if (img) {
			f.img = img;
			f.w = tw.w;
			f.h = tw.h;
			// Mirrored loops flip the frame around the same anchor rule the engine uses for cels
			f.ox = a.mirror ? (int16)(((tw.w >> 1) << 1) - tw.ox) : tw.ox;
			f.oy = tw.oy;
			f.key = k;
		}
	}
	return f.img != nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// Compositing

namespace {

/** Per-channel grade from an asset's reference palette entry to the live one (see hd.h). */
struct Grade {
	int mul[3]; // 16.16 fixed point
	int add[3];
	bool identity;

	void set(const byte *live, const HdImage *img, byte idx) {
		identity = true;
		for (int c = 0; c < 3; c++) {
			mul[c] = 1 << 16;
			add[c] = 0;
			if (!img->hasRef)
				continue;
			const int ref = img->refPal[idx * 3 + c], l = live[c];
			if (ref == l)
				continue;
			identity = false;
			if (ref >= 16)
				mul[c] = ((l << 16) + ref / 2) / ref;
			else
				add[c] = l - ref;
		}
	}
	inline void apply(uint32 p, int &r, int &g, int &b) const {
		r = (p >> 24) & 0xFF; g = (p >> 16) & 0xFF; b = (p >> 8) & 0xFF;
		if (identity)
			return;
		r = CLIP(((r * mul[0] + 0x8000) >> 16) + add[0], 0, 255);
		g = CLIP(((g * mul[1] + 0x8000) >> 16) + add[1], 0, 255);
		b = CLIP(((b * mul[2] + 0x8000) >> 16) + add[2], 0, 255);
	}
};

inline uint32 sample(const HdRecord &rec, int n, int x, int y, int i, int j) {
	const HdImage *img = rec.img;
	int hx = ((x - rec.left) * n + i) * img->w / (rec.dstW * n);
	int hy = ((y - rec.top) * n + j) * img->h / (rec.dstH * n);
	hx = CLIP<int>(hx, 0, img->w - 1);
	hy = CLIP<int>(hy, 0, img->h - 1);
	if (rec.mirror)
		hx = img->w - 1 - hx;
	return img->px[hy * img->w + hx];
}

inline bool covers(const HdRecord &rec, int x, int y) {
	return rec.img && x >= rec.left && y >= rec.top && x < rec.left + rec.dstW && y < rec.top + rec.dstH;
}

inline void putOut(byte *dst, const Graphics::PixelFormat &fmt, int r, int g, int b) {
	const uint32 c = fmt.RGBToColor(r, g, b);
	if (fmt.bytesPerPixel == 4)
		*(uint32 *)dst = c;
	else
		*(uint16 *)dst = (uint16)c;
}

} // End of anonymous namespace

void GfxHd::compose(const byte *low, const uint16 *prov, const int32 *shift, int lowPitch, const Common::Rect &r,
		const byte *livePal, byte *out, int outPitch, const Graphics::PixelFormat &fmt) const {
	const int n = _scale;
	const int bpp = fmt.bytesPerPixel;
	const HdRecord *bg = (_background && _background < _records.size()) ? &_records[_background] : nullptr;

	for (int y = r.top; y < r.bottom; y++) {
		for (int x = r.left; x < r.right; x++) {
			const byte idx = low[y * lowPitch + x];
			const byte *live = &livePal[idx * 3];
			const uint16 id = prov ? prov[y * lowPitch + x] : 0;
			byte *block = out + (y * n) * outPitch + (x * n) * bpp;

			if (!id || id >= _records.size() || !_records[id].img) {
				// Nothing HD drew this pixel: nearest-neighbour block of the live colour
				for (int j = 0; j < n; j++)
					for (int i = 0; i < n; i++)
						putOut(block + j * outPitch + i * bpp, fmt, live[0], live[1], live[2]);
				continue;
			}

			HdRecord rec = _records[id];
			// A talker bust shows its face's current frame, with the eye layer on top (see hd.h)
			const HdImage *layer = nullptr;
			if (rec.face >= 0) {
				const HdFace &f = _faces[rec.face];
				if (f.base)
					rec.img = f.base;
				layer = f.layer;
			}
			// Sample where the pixel was drawn, which differs from where it is shown during scroll transitions
			const int32 sh = shift[y * lowPitch + x];
			const int sx = x + (int16)(sh & 0xFFFF), sy = y + (sh >> 16);

			if (rec.overlay) {
				// An overlay actor's own pixels: the HD background shows here, the actor is drawn on top later
				for (int j = 0; j < n; j++) {
					for (int i = 0; i < n; i++) {
						int cr = live[0], cg = live[1], cb = live[2];
						if (bg && covers(*bg, sx, sy)) {
							const uint32 p = sample(*bg, n, sx, sy, i, j);
							cr = (p >> 24) & 0xFF; cg = (p >> 16) & 0xFF; cb = (p >> 8) & 0xFF;
						}
						putOut(block + j * outPitch + i * bpp, fmt, cr, cg, cb);
					}
				}
				continue;
			}
			Grade grade;
			grade.set(live, rec.img, idx);
			const bool underlay = bg && bg != &rec && covers(*bg, sx, sy);

			for (int j = 0; j < n; j++) {
				for (int i = 0; i < n; i++) {
					const uint32 p = sample(rec, n, sx, sy, i, j);
					const int a = p & 0xFF;
					int cr, cg, cb;
					grade.apply(p, cr, cg, cb);
					if (a < 255) {
						// Partly transparent HD pixel inside the low-res silhouette: blend over the HD background
						// (graded like this pixel), else over the low-res colour
						int ur = live[0], ug = live[1], ub = live[2];
						if (underlay)
							grade.apply(sample(*bg, n, sx, sy, i, j), ur, ug, ub);
						cr = (cr * a + ur * (255 - a)) / 255;
						cg = (cg * a + ug * (255 - a)) / 255;
						cb = (cb * a + ub * (255 - a)) / 255;
					}
					if (layer) {
						HdRecord lrec = rec;
						lrec.img = layer;
						const uint32 lp = sample(lrec, n, sx, sy, i, j);
						const int la = lp & 0xFF;
						if (la) {
							int lr, lg, lb;
							grade.apply(lp, lr, lg, lb);
							cr = (lr * la + cr * (255 - la)) / 255;
							cg = (lg * la + cg * (255 - la)) / 255;
							cb = (lb * la + cb * (255 - la)) / 255;
						}
					}
					putOut(block + j * outPitch + i * bpp, fmt, cr, cg, cb);
				}
			}
		}
	}
}

void GfxHd::drawOverlay(const HdOverlayFrame &f, byte priority, uint16 record, const byte *low, const uint16 *prov,
		const byte *priScreen, int lowPitch, const Common::Rect &clip, const byte *livePal, byte *out, int outPitch,
		const Graphics::PixelFormat &fmt) const {
	const int n = _scale;
	const int bpp = fmt.bytesPerPixel;
	const int x0 = f.hdX - f.ox * n, y0 = f.hdY - f.oy * n;
	const int w = f.w * n, h = f.h * n;
	const int cx0 = MAX(x0, clip.left * n), cy0 = MAX(y0, clip.top * n);
	const int cx1 = MIN(x0 + w, clip.right * n), cy1 = MIN(y0 + h, clip.bottom * n);
	const HdImage *img = f.img;

	// Grade the whole frame like the actor's own low-res pixels (fades, tints): pixels outside its current
	// outline sit over the background, whose palette entries say nothing about the actor's colours
	Grade grade;
	grade.identity = true;
	{
		int sum[3] = { 0, 0, 0 }, add[3] = { 0, 0, 0 }, count = 0;
		Common::Rect own(x0 / n, y0 / n, (x0 + w) / n, (y0 + h) / n);
		own.clip(clip); // the presented region, which the driver grows to hold every overlay frame it touches
		for (int ly = own.top; ly < own.bottom; ly++) {
			for (int lx = own.left; lx < own.right; lx++) {
				const int o = ly * lowPitch + lx;
				if (prov[o] != record)
					continue;
				Grade g;
				g.set(&livePal[low[o] * 3], img, low[o]);
				for (int c = 0; c < 3; c++) {
					sum[c] += g.mul[c] >> 8;
					add[c] += g.add[c];
				}
				count++;
			}
		}
		if (count) {
			for (int c = 0; c < 3; c++) {
				grade.mul[c] = (sum[c] / count) << 8;
				grade.add[c] = add[c] / count;
				if (grade.mul[c] != 1 << 16 || grade.add[c])
					grade.identity = false;
			}
		} else {
			for (int c = 0; c < 3; c++) {
				grade.mul[c] = 1 << 16;
				grade.add[c] = 0;
			}
		}
	}

	for (int hy = cy0; hy < cy1; hy++) {
		const int ly = hy / n;
		int iy = (hy - y0) * img->h / h;
		iy = CLIP<int>(iy, 0, img->h - 1);
		for (int hx = cx0; hx < cx1; hx++) {
			const int lx = hx / n;
			const int o = ly * lowPitch + lx;
			const uint16 id = prov[o];
			// Visible where the actor itself drew, or over another HD draw that isn't in front of it.
			// Pixels no HD draw owns (text, windows, the status line) always stay on top.
			if (id != record && (!id || id >= _records.size() || priScreen[o] > priority))
				continue;
			int ix = (hx - x0) * img->w / w;
			ix = CLIP<int>(ix, 0, img->w - 1);
			if (f.flip)
				ix = img->w - 1 - ix;
			const uint32 p = img->px[iy * img->w + ix];
			const int a = p & 0xFF;
			if (!a)
				continue;
			int cr, cg, cb;
			grade.apply(p, cr, cg, cb);
			byte *dst = out + hy * outPitch + hx * bpp;
			if (a < 255) {
				const uint32 cur = bpp == 4 ? *(uint32 *)dst : *(uint16 *)dst;
				byte ur, ug, ub;
				fmt.colorToRGB(cur, ur, ug, ub);
				cr = (cr * a + ur * (255 - a)) / 255;
				cg = (cg * a + ug * (255 - a)) / 255;
				cb = (cb * a + ub * (255 - a)) / 255;
			}
			putOut(dst, fmt, cr, cg, cb);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------
// Driver

HdGfxDriver::HdGfxDriver(GfxHd *hd, uint16 width, uint16 height) :
	GfxDefaultDriver(width * hd->scale(), height * hd->scale(), false, true), _hd(hd), _n(hd->scale()),
	_provSrc(nullptr), _priority(nullptr), _currentProv(nullptr), _currentShift(nullptr), _cursorBuffer(nullptr), _cursorBufferSize(0), _lastDump(0), _dumpPending(false), _dumpInterval(0), _dumpCount(0) {
	_virtualW = width;
	_virtualH = height;
	_currentProv = new uint16[width * height]();
	_currentShift = new int32[width * height]();
	if (ConfMan.hasKey("hd_dump_path")) {
		_dumpPath = ConfMan.getPath("hd_dump_path");
		_dumpInterval = ConfMan.hasKey("hd_dump_interval") ? ConfMan.getInt("hd_dump_interval") : 2000;
	}
}

HdGfxDriver::~HdGfxDriver() {
	delete[] _currentProv;
	delete[] _currentShift;
	delete[] _cursorBuffer;
}

bool HdGfxDriver::initScreen(const Graphics::PixelFormat *) {
	// True-colour art needs a 32-bit screen; the backend's default ("best") format may be 16-bit.
	Common::List<Graphics::PixelFormat> formats = g_system->getSupportedFormats();
	Graphics::PixelFormat want = formats.front();
	for (const auto &f : formats) {
		if (f.bytesPerPixel == 4) {
			want = f;
			break;
		}
	}
	initGraphics(_screenW, _screenH, &want);
	_format = g_system->getScreenFormat();
	if (_format.bytesPerPixel != 2 && _format.bytesPerPixel != 4)
		error("HD layer needs a 16- or 32-bit screen format (got %d bytes per pixel)", _format.bytesPerPixel);
	if (_format.bytesPerPixel != 4) {
		Common::String list;
		for (const auto &f : formats)
			list += f.toString() + " ";
		warning("HD layer: no 32-bit screen format available (backend offers: %s), colours will be reduced", list.c_str());
	}

	delete[] _compositeBuffer;
	delete[] _currentBitmap;
	delete[] _currentPalette;
	delete[] _internalPalette;
	_pixelSize = _format.bytesPerPixel;
	_srcPixelSize = 1;
	_compositeBuffer = new byte[_screenW * _screenH * _pixelSize]();	// the HD frame buffer
	_currentBitmap = new byte[_virtualW * _virtualH]();					// low-res indices as last presented
	_currentPalette = new byte[256 * 3]();
	_internalPalette = new byte[256 * _pixelSize]();
	_ready = true;
	return true;
}

void HdGfxDriver::setPalette(const byte *colors, uint start, uint num, bool update, const PaletteMod *palMods, const byte *palModMapping) {
	GFXDRV_ASSERT_READY;
	updatePalette(colors, start, num);
	// In CLUT8 mode a palette change recolours the whole screen at once, whether or not `update` is set,
	// and the game relies on that (fades call this with update = false). Recompose everything to match.
	present(Common::Rect(0, 0, _virtualW, _virtualH));
	CursorMan.replaceCursorPalette(_currentPalette, 0, 256);
}

void HdGfxDriver::copyRectToScreen(const byte *src, int srcX, int srcY, int pitch, int destX, int destY, int w, int h, const PaletteMod *palMods, const byte *palModMapping) {
	GFXDRV_ASSERT_READY;
	assert(h >= 0 && w >= 0);

	const byte *s = src + srcY * pitch + srcX;
	if (s != _currentBitmap)
		SciGfxDrvInternal::updateBitmapBuffer(_currentBitmap, _virtualW, s, pitch, destX, destY, w, h);
	// Screen copies may land somewhere else than where they were drawn (scroll transitions): remember the
	// displacement, packed as in GfxHd::compose, so HD art is sampled where the pixel came from
	const int32 shift = (int32)(uint16)(int16)(srcX - destX) | (int32)((srcY - destY) * 65536);
	for (int y = 0; y < h; y++) {
		uint16 *d = _currentProv + (destY + y) * _virtualW + destX;
		int32 *sh = _currentShift + (destY + y) * _virtualW + destX;
		if (_provSrc)
			memcpy(d, _provSrc + (srcY + y) * pitch + srcX, w * sizeof(uint16));
		else
			memset(d, 0, w * sizeof(uint16));
		for (int x = 0; x < w; x++)
			sh[x] = shift;
	}
	_provSrc = nullptr;

	present(Common::Rect(destX, destY, destX + w, destY + h));
}

static Common::Rect frameRect(const HdOverlayFrame &f, int n) {
	// Low-res pixels the frame touches
	const int x0 = f.hdX - f.ox * n, y0 = f.hdY - f.oy * n;
	return Common::Rect(x0 / n, y0 / n, (x0 + f.w * n + n - 1) / n, (y0 + f.h * n + n - 1) / n);
}

void HdGfxDriver::visibleActors(Common::Array<uint32> &out) {
	out.clear();
	Common::HashMap<uint32, HdActor> &actors = _hd->actors();
	if (actors.empty())
		return;
	// Records the displayed frame holds (an actor's latest draw may not be on screen yet)
	Common::HashMap<uint16, bool> shown;
	const uint16 *p = _currentProv, *end = _currentProv + _virtualW * _virtualH;
	for (; p < end; p++) {
		if (*p && _hd->isOverlay(*p))
			shown[*p] = true;
	}
	for (auto &a : actors) {
		if (shown.contains(a._value.record))
			out.push_back(a._key);
	}
	Common::sort(out.begin(), out.end(), [&actors](uint32 a, uint32 b) { return actors[a].priority < actors[b].priority; });
}

void HdGfxDriver::present(const Common::Rect &dirty) {
	const int outPitch = _screenW * _pixelSize;
	const uint32 start = g_system->getMillis();
	Common::Rect r = dirty;
	_hd->updateFaces(start);
	extendFaces(r);

	// Overlay actors draw beyond their low-res pixels: recompose all of any one the update touches
	Common::Array<uint32> ids;
	visibleActors(ids);
	Common::Array<HdOverlayFrame> frames(ids.size());
	Common::Array<bool> have(ids.size());
	for (uint i = 0; i < ids.size(); i++)
		have[i] = _priority && _hd->actorFrame(_hd->actors()[ids[i]], start, frames[i]);
	const Common::Rect screen(_virtualW, _virtualH);
	// Frames still showing for actors the displayed frame no longer holds (stopped: another view; turned;
	// hidden; gone) are erased when an update touches them
	for (auto &a : _hd->actors()) {
		if (a._value.shownRect.isEmpty() || Common::find(ids.begin(), ids.end(), a._key) != ids.end())
			continue;
		if (a._value.shownRect.intersects(r))
			r.extend(a._value.shownRect);
	}
	for (int pass = 0; pass < 3; pass++) {
		const Common::Rect before = r;
		for (uint i = 0; i < ids.size(); i++) {
			HdActor &a = _hd->actors()[ids[i]];
			if (!a.shownRect.isEmpty() && a.shownRect.intersects(r))
				r.extend(a.shownRect);
			if (have[i]) {
				const Common::Rect fr = frameRect(frames[i], _n);
				if (fr.intersects(r))
					r.extend(fr);
			}
		}
		r.clip(screen);
		if (r == before)
			break;
	}

	_hd->compose(_currentBitmap, _currentProv, _currentShift, _virtualW, r, _currentPalette, _compositeBuffer, outPitch, _format);
	for (HdFace &f : _hd->faces()) {
		if (!f.record)
			continue;
		const HdRecord &b = _hd->record(f.record);
		Common::Rect fr(b.left, b.top, b.left + b.dstW, b.top + b.dstH);
		fr.clip(screen);
		if (r.contains(fr))
			f.shownKey = f.key;
	}
	for (auto &a : _hd->actors()) {
		if (!a._value.shownRect.isEmpty() && r.contains(a._value.shownRect)
				&& Common::find(ids.begin(), ids.end(), a._key) == ids.end()) {
			a._value.shownRect = Common::Rect();  // erased by the compose above
			a._value.shownKey = -3;
		}
	}
	for (uint i = 0; i < ids.size(); i++) {
		HdActor &a = _hd->actors()[ids[i]];
		if (!have[i])
			continue;
		const Common::Rect fr = frameRect(frames[i], _n);
		if (!fr.intersects(r))
			continue;
		_hd->drawOverlay(frames[i], a.priority, a.record, _currentBitmap, _currentProv, _priority, _virtualW, r,
			_currentPalette, _compositeBuffer, outPitch, _format);
		a.shownKey = frames[i].key;
		a.shownX = frames[i].hdX;
		a.shownY = frames[i].hdY;
		a.shownRect = fr;
	}
	if (r.width() == _virtualW && r.height() == _virtualH)
		debugC(1, kDebugLevelGraphics, "HD: full-screen compose took %u ms", g_system->getMillis() - start);
	g_system->copyRectToScreen(_compositeBuffer + r.top * _n * outPitch + r.left * _n * _pixelSize, outPitch,
		r.left * _n, r.top * _n, r.width() * _n, r.height() * _n);

	if (_dumpInterval) {
		if (g_system->getMillis() - _lastDump >= _dumpInterval) {
			_lastDump = g_system->getMillis();
			_dumpPending = false;
			dumpFrame();
		} else {
			_dumpPending = true;
		}
	}
}

void HdGfxDriver::extendFaces(Common::Rect &r) {
	const Common::Rect screen(_virtualW, _virtualH);
	for (const HdFace &f : _hd->faces()) {
		if (!f.record || f.key == f.shownKey)
			continue;
		const HdRecord &b = _hd->record(f.record);
		Common::Rect fr(b.left, b.top, b.left + b.dstW, b.top + b.dstH);
		fr.clip(screen);
		if (!fr.isEmpty() && fr.intersects(r))
			r.extend(fr);
	}
}

void HdGfxDriver::tick() {
	if (!_ready)
		return;
	const uint32 now = g_system->getMillis();
	Common::Rect dirty;
	const Common::Rect screen(_virtualW, _virtualH);

	if (_dumpPending && now - _lastDump >= _dumpInterval) {
		_lastDump = now;
		_dumpPending = false;
		dumpFrame();
	}

	// Faces whose frame moved on, if the displayed frame holds them
	if (!_hd->faces().empty()) {
		_hd->updateFaces(now);
		for (const HdFace &f : _hd->faces()) {
			if (!f.record || f.key == f.shownKey)
				continue;
			const HdRecord &b = _hd->record(f.record);
			Common::Rect fr(b.left, b.top, b.left + b.dstW, b.top + b.dstH);
			fr.clip(screen);
			bool shown = false;
			for (int y = fr.top; y < fr.bottom && !shown; y++) {
				const uint16 *p = _currentProv + y * _virtualW;
				for (int x = fr.left; x < fr.right; x++) {
					if (p[x] == f.record) {
						shown = true;
						break;
					}
				}
			}
			if (!shown || fr.isEmpty())
				continue;
			if (dirty.isEmpty())
				dirty = fr;
			else
				dirty.extend(fr);
		}
	}

	if (_priority && !_hd->actors().empty()) {
		Common::Array<uint32> ids;
		visibleActors(ids);
		for (auto &a : _hd->actors()) {
			if (a._value.shownRect.isEmpty() || Common::find(ids.begin(), ids.end(), a._key) != ids.end())
				continue;
			if (dirty.isEmpty())
				dirty = a._value.shownRect;
			else
				dirty.extend(a._value.shownRect);
		}
		for (uint32 id : ids) {
			HdActor &a = _hd->actors()[id];
			HdOverlayFrame f;
			if (!_hd->actorFrame(a, now, f))
				continue;
			if (f.key == a.shownKey && f.hdX == a.shownX && f.hdY == a.shownY)
				continue;
			Common::Rect r = frameRect(f, _n);
			if (!a.shownRect.isEmpty())
				r.extend(a.shownRect);
			if (dirty.isEmpty())
				dirty = r;
			else
				dirty.extend(r);
		}
	}
	if (!dirty.isEmpty())
		present(dirty);
}

void HdGfxDriver::copyCurrentProvenance(uint16 *dest) const {
	memcpy(dest, _currentProv, _virtualW * _virtualH * sizeof(uint16));
}

/** Debug/test aid: write the HD frame and the low-res frame side by side into hd_dump_path. */
void HdGfxDriver::dumpFrame() {
#ifdef USE_PNG
	Graphics::Surface hd;
	hd.init(_screenW, _screenH, _screenW * _pixelSize, _compositeBuffer, _format);
	Graphics::Surface low;
	low.init(_virtualW, _virtualH, _virtualW, _currentBitmap, Graphics::PixelFormat::createFormatCLUT8());

	Common::DumpFile f;
	if (_dumpCount == 0 && f.open(_dumpPath.appendComponent("hd-format.txt"), true)) {
		// Channel bit depths of the screen, so a verifier can quantise the expected colours the same way
		f.writeString(Common::String::format("%d %d %d\n", _format.rBits(), _format.gBits(), _format.bBits()));
		f.close();
	}
	if (f.open(_dumpPath.appendComponent(Common::String::format("hd-%04d.png", _dumpCount)), true))
		Image::writePNG(f, hd);
	f.close();
	if (f.open(_dumpPath.appendComponent(Common::String::format("low-%04d.png", _dumpCount)), true))
		Image::writePNG(f, low, _currentPalette, 256);
	f.close();
	// Written last, so a reader knows which dump is complete
	if (f.open(_dumpPath.appendComponent("hd-last.txt"), true))
		f.writeString(Common::String::format("%d\n", _dumpCount));
	f.close();
	_dumpCount++;
#endif
}

void HdGfxDriver::replaceCursor(const void *cursor, uint w, uint h, int hotspotX, int hotspotY, uint32 keycolor) {
	GFXDRV_ASSERT_READY;
	const uint size = w * h * _n * _n;
	if (size > _cursorBufferSize) {
		delete[] _cursorBuffer;
		_cursorBuffer = new byte[size];
		_cursorBufferSize = size;
	}
	const byte *s = (const byte *)cursor;
	for (uint y = 0; y < h * _n; y++)
		for (uint x = 0; x < w * _n; x++)
			_cursorBuffer[y * w * _n + x] = s[(y / _n) * w + x / _n];
	CursorMan.replaceCursor(_cursorBuffer, w * _n, h * _n, hotspotX * _n, hotspotY * _n, keycolor);
	CursorMan.replaceCursorPalette(_currentPalette, 0, 256);
}

Common::Point HdGfxDriver::getMousePos() const {
	Common::Point p = GfxDriver::getMousePos();
	return Common::Point(p.x / _n, p.y / _n);
}

void HdGfxDriver::setMousePos(const Common::Point &pos) const {
	g_system->warpMouse(pos.x * _n, pos.y * _n);
}

void HdGfxDriver::setShakePos(int shakeXOffset, int shakeYOffset) const {
	g_system->setShakePos(shakeXOffset * _n, shakeYOffset * _n);
}

void HdGfxDriver::clearRect(const Common::Rect &r) const {
	GfxDriver::clearRect(Common::Rect(r.left * _n, r.top * _n, r.right * _n, r.bottom * _n));
}

Common::Point HdGfxDriver::getRealCoords(Common::Point &pos) const {
	return Common::Point(pos.x * _n, pos.y * _n);
}

} // End of namespace Sci
