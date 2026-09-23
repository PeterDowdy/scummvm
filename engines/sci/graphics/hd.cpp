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

GfxHd::GfxHd(const Common::FSNode &root, int scale) : _root(root), _scale(scale), _background(0) {
	indexFiles(root, "");
	resetRecords();
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

uint16 GfxHd::registerDraw(const HdImage *img, int16 left, int16 top, int16 dstW, int16 dstH, bool mirror) {
	// Pack the draw into a key; images are identified by their record-independent address
	const uint64 key = ((uint64)(uintptr)img << 32) ^ ((uint64)(uint16)left << 20) ^ ((uint64)(uint16)top << 8)
		^ ((uint64)dstW << 40) ^ ((uint64)dstH << 52) ^ (mirror ? 1ULL << 63 : 0);
	if (_recordIndex.contains(key)) {
		const HdRecord &r = _records[_recordIndex[key]];
		if (r.img == img && r.left == left && r.top == top && r.dstW == dstW && r.dstH == dstH && r.mirror == mirror)
			return _recordIndex[key];
	}
	if (_records.size() >= 0xFFFF)
		return 0;
	HdRecord r = { img, left, top, dstW, dstH, mirror };
	_records.push_back(r);
	const uint16 id = _records.size() - 1;
	_recordIndex[key] = id;
	return id;
}

void GfxHd::resetRecords() {
	_records.clear();
	_recordIndex.clear();
	HdRecord none = { nullptr, 0, 0, 0, 0, false };
	_records.push_back(none);
	_background = 0;
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

void GfxHd::compose(const byte *low, const uint16 *prov, int lowPitch, const Common::Rect &r, const byte *livePal,
		byte *out, int outPitch, const Graphics::PixelFormat &fmt) const {
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

			const HdRecord &rec = _records[id];
			Grade grade;
			grade.set(live, rec.img, idx);
			const bool underlay = bg && bg != &rec && covers(*bg, x, y);

			for (int j = 0; j < n; j++) {
				for (int i = 0; i < n; i++) {
					const uint32 p = sample(rec, n, x, y, i, j);
					const int a = p & 0xFF;
					int cr, cg, cb;
					grade.apply(p, cr, cg, cb);
					if (a < 255) {
						// Partly transparent HD pixel inside the low-res silhouette: blend over the HD background
						// (graded like this pixel), else over the low-res colour
						int ur = live[0], ug = live[1], ub = live[2];
						if (underlay)
							grade.apply(sample(*bg, n, x, y, i, j), ur, ug, ub);
						cr = (cr * a + ur * (255 - a)) / 255;
						cg = (cg * a + ug * (255 - a)) / 255;
						cb = (cb * a + ub * (255 - a)) / 255;
					}
					putOut(block + j * outPitch + i * bpp, fmt, cr, cg, cb);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------------------
// Driver

HdGfxDriver::HdGfxDriver(GfxHd *hd, uint16 width, uint16 height) :
	GfxDefaultDriver(width * hd->scale(), height * hd->scale(), false, true), _hd(hd), _n(hd->scale()),
	_provSrc(nullptr), _currentProv(nullptr), _cursorBuffer(nullptr), _cursorBufferSize(0), _lastDump(0), _dumpInterval(0), _dumpCount(0) {
	_virtualW = width;
	_virtualH = height;
	_currentProv = new uint16[width * height]();
	if (ConfMan.hasKey("hd_dump_path")) {
		_dumpPath = ConfMan.getPath("hd_dump_path");
		_dumpInterval = ConfMan.hasKey("hd_dump_interval") ? ConfMan.getInt("hd_dump_interval") : 2000;
	}
}

HdGfxDriver::~HdGfxDriver() {
	delete[] _currentProv;
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
	for (int y = 0; y < h; y++) {
		uint16 *d = _currentProv + (destY + y) * _virtualW + destX;
		if (_provSrc)
			memcpy(d, _provSrc + (srcY + y) * pitch + srcX, w * sizeof(uint16));
		else
			memset(d, 0, w * sizeof(uint16));
	}
	_provSrc = nullptr;

	present(Common::Rect(destX, destY, destX + w, destY + h));
}

void HdGfxDriver::present(const Common::Rect &r) {
	const int outPitch = _screenW * _pixelSize;
	const uint32 start = g_system->getMillis();
	_hd->compose(_currentBitmap, _currentProv, _virtualW, r, _currentPalette, _compositeBuffer, outPitch, _format);
	if (r.width() == _virtualW && r.height() == _virtualH)
		debugC(1, kDebugLevelGraphics, "HD: full-screen compose took %u ms", g_system->getMillis() - start);
	g_system->copyRectToScreen(_compositeBuffer + r.top * _n * outPitch + r.left * _n * _pixelSize, outPitch,
		r.left * _n, r.top * _n, r.width() * _n, r.height() * _n);

	if (_dumpInterval && g_system->getMillis() - _lastDump >= _dumpInterval) {
		_lastDump = g_system->getMillis();
		dumpFrame();
	}
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
