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

#ifndef SCI_GRAPHICS_HD_H
#define SCI_GRAPHICS_HD_H

#include "common/array.h"
#include "common/fs.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/rect.h"
#include "common/str.h"
#include "sci/graphics/drivers/gfxdriver_intern.h"

namespace Sci {

/**
 * HD presentation layer for SCI1.1 (VGA) games.
 *
 * The game keeps running at 320x200 in 8-bit: the visual, priority and control screens, the scripts and
 * all hit-testing are untouched. Alongside the visual screen, GfxScreen keeps a *provenance plane*: for each
 * low-res pixel, the id of the HD-managed draw (a view cel or pic cel at a position) that last wrote it,
 * or 0. At presentation time HdGfxDriver builds an N x N block per low-res pixel: from the HD replacement
 * of whatever drew it, or a nearest-neighbour block of the live palette colour when nothing HD drew it.
 *
 * HD art is true colour. Palette effects (fades, day/night, cycling) are applied per pixel by grading the
 * HD colour with the ratio between the live palette entry and the entry the asset was authored against
 * (its reference palette, shipped in the pack).
 *
 * Pack layout (directory "hd" in the game path, or ConfMan "hd_pack_path"):
 *   manifest.json                 {"scale": N}
 *   view/<view>/<loop>.<cel>.png  RGBA, any size (sampled proportionally over the cel); mirrored loops
 *                                 use their source loop's images, flipped
 *   view/<view>/palette.pal       768-byte RGB reference palette
 *   pic/<pic>.<cel>.png           RGBA background
 *   pic/<pic>.pal                 768-byte RGB reference palette
 */

struct HdImage {
	uint16 w;
	uint16 h;
	Common::Array<uint32> px; ///< packed r << 24 | g << 16 | b << 8 | a
	byte refPal[256 * 3];
	bool hasRef;
};

/** One HD-managed draw: which image, and where its full (unclipped) cel landed in low-res screen space. */
struct HdRecord {
	const HdImage *img;
	int16 left;
	int16 top;
	int16 dstW;
	int16 dstH;
	bool mirror;
};

class GfxHd {
public:
	/** Returns nullptr when no HD pack is configured or found. */
	static GfxHd *create();
	~GfxHd();

	int scale() const { return _scale; }

	const HdImage *findView(int view, int loop, int cel);
	const HdImage *findPic(int pic, int cel);

	/**
	 * Returns a record id (>= 1). Ids live in a ring: an id is only reused after 65535 newer draws, so ids
	 * still held by the displayed frame, saved screen bits or transition backups stay valid.
	 */
	uint16 registerDraw(const HdImage *img, int16 left, int16 top, int16 dstW, int16 dstH, bool mirror);
	void setBackground(uint16 id) { _background = id; }

	/**
	 * Composite low-res rect `r` into the HD frame buffer `out` (full-size, pitch in bytes).
	 * `low`, `prov` and `shift` are full 320-wide planes with pitch `lowPitch` pixels. `shift` holds, per
	 * displayed pixel, where it was drawn relative to where it is shown (screen copies to another position,
	 * e.g. scroll transitions), packed as (dx & 0xFFFF) | dy << 16.
	 */
	void compose(const byte *low, const uint16 *prov, const int32 *shift, int lowPitch, const Common::Rect &r,
		const byte *livePal, byte *out, int outPitch, const Graphics::PixelFormat &fmt) const;

private:
	GfxHd(const Common::FSNode &root, int scale);
	const HdImage *load(const Common::String &path, const Common::String &palPath);
	void indexFiles(const Common::FSNode &dir, const Common::String &prefix);

	Common::FSNode _root;
	int _scale;
	Common::HashMap<Common::String, bool> _files;           ///< every file in the pack, by relative path
	Common::HashMap<Common::String, HdImage *> _images;     ///< loaded (or failed = nullptr) images
	Common::Array<HdRecord> _records;                       ///< ring of 65536 slots, index 0 unused
	Common::Array<uint64> _recordKeys;                      ///< key of each slot, to drop it from the index on reuse
	uint16 _nextRecord;
	struct KeyHash { uint operator()(uint64 v) const { return (uint)(v ^ (v >> 32)); } };
	Common::HashMap<uint64, uint16, KeyHash> _recordIndex;
	uint16 _background;
};

/** Presents the game at N x its resolution in true colour, compositing HD assets via GfxHd. */
class HdGfxDriver : public GfxDefaultDriver {
public:
	HdGfxDriver(GfxHd *hd, uint16 width, uint16 height);
	~HdGfxDriver() override;
	bool initScreen(const Graphics::PixelFormat *format) override;
	void setPalette(const byte *colors, uint start, uint num, bool update, const PaletteMod *palMods, const byte *palModMapping) override;
	void copyRectToScreen(const byte *src, int srcX, int srcY, int pitch, int destX, int destY, int w, int h, const PaletteMod *palMods, const byte *palModMapping) override;
	void replaceCursor(const void *cursor, uint w, uint h, int hotspotX, int hotspotY, uint32 keycolor) override;
	Common::Point getMousePos() const override;
	void setMousePos(const Common::Point &pos) const override;
	void setShakePos(int shakeXOffset, int shakeYOffset) const override;
	void clearRect(const Common::Rect &r) const override;
	Common::Point getRealCoords(Common::Point &pos) const override;

	/** Provenance plane (same layout as the src of the next copyRectToScreen), or nullptr for none. */
	void setProvenanceSource(const uint16 *prov) { _provSrc = prov; }
	void copyCurrentProvenance(uint16 *dest) const;

private:
	void present(const Common::Rect &r);
	void dumpFrame();

	GfxHd *_hd;
	const int _n;
	const uint16 *_provSrc;
	uint16 *_currentProv;
	int32 *_currentShift; ///< per displayed pixel: source position - displayed position (see GfxHd::compose)
	byte *_cursorBuffer;
	uint _cursorBufferSize;
	Common::Path _dumpPath;
	uint32 _lastDump;
	uint32 _dumpInterval;
	int _dumpCount;
};

} // End of namespace Sci

#endif // SCI_GRAPHICS_HD_H
