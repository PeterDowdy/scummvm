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
 *   view/<view>/tweens.json       in-between frames: {"<loop>.<cel>": [{"pos", "file", "w", "h", "ox", "oy"}]},
 *                                 drawn between that cel and the next one; w/h/ox/oy (size and anchor) in
 *                                 low-res pixels, "file" relative to the pack
 *
 *   view/<view>/face.json         full-face animation for a talker portrait (see below):
 *                                 {"talk": {"fps": F, "frames": [file, ...]}, "idle": {...}, "eyes": {"<cel>": file},
 *                                  "loops": {"bust": 0, "mouth": 1, "eyes": 2}, "hold": ms}
 *
 * Faces: a talker portrait (Talker.sc) is one view drawn as a bust, with the mouth and eyes redrawn on top as
 * separate small cels: the mouth cycles random cels while text is up, the eyes blink. With a face.json the
 * mouth and eye draws inside the bust count as the bust, and the whole bust shows a full-face frame instead:
 * the "talk" loop in order while the mouth is moving (or for "hold" ms after it last moved), else "idle" (or
 * the bust's HD image). While the eyes show a cel other than 0, "eyes" layers that cel's image (RGBA, bust
 * size, transparent outside the eyes) on top. Frames are sampled over the bust like any cel; files are
 * relative to the pack.
 *
 * In-betweens: an actor (a cast member drawn by GfxAnimate) whose view has in-betweens is an *overlay*.
 * Its low-res pixels show the HD background, and its whole HD frame is drawn on top, hidden where the
 * priority screen puts something in front and never over pixels no HD draw owns (text, windows). The frame
 * runs one cel behind the game: after a cel change, the cel's display time shows the previous cel, then each
 * in-between in turn, while the actor eases from its old position to its new one. Nothing is predicted, so
 * stops, turns and loop changes can't show a wrong frame; the cost is one cel of visual latency.
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
	bool overlay; ///< an actor drawn as an overlay (see above): its pixels show the background
	int16 face;   ///< a talker bust with a face.json: index into GfxHd's faces, else -1
};

/** A looping sequence of face frames. */
struct HdFaceAnim {
	Common::Array<Common::String> frames;
	float fps = 12.0f;
};

/** A talker portrait with a full-face animation (see above). */
struct HdFace {
	int view = -1;
	int bustLoop = 0, mouthLoop = 1, eyesLoop = 2;
	HdFaceAnim talk, idle;
	Common::HashMap<int, Common::String> eyes; ///< eye cel -> layer file
	uint32 holdMs = 400;
	// State
	uint16 record = 0;            ///< the latest bust draw
	int mouthCel = 0;
	uint32 mouthAt = 0;           ///< when the mouth was last drawn with another cel
	int eyesCel = 0;
	bool talking = false;
	uint32 since = 0;             ///< when the current animation (talk or idle) started
	int key = -1;                 ///< the frame chosen by the last update
	int shownKey = -2;            ///< the frame on screen
	const HdImage *base = nullptr;
	const HdImage *layer = nullptr;
};

/** An in-between frame from a view's tweens.json. */
struct HdTween {
	float pos;       ///< 0-1: how far from its cel to the next one
	int16 w, h;      ///< size in low-res pixels
	int16 ox, oy;    ///< anchor inside the frame (unmirrored), low-res pixels
	Common::String file;
};

/** What the HD layer knows about one animated actor (a cast object). */
struct HdActor {
	int view = -1, loop = -1, cel = -1, cels = 0;
	bool mirror = false;
	int16 ax = 0, ay = 0;          ///< anchor on screen (low-res)
	int16 celW = 0, celH = 0;      ///< the drawn cel's size
	int16 celAx = 0, celAy = 0;    ///< anchor inside the drawn cel
	byte priority = 0;
	uint16 record = 0;             ///< the record of its latest draw
	// The cel before the latest change, which the frame is still moving away from
	bool hasPrev = false;
	int prevCel = -1;
	int16 prevAx = 0, prevAy = 0, prevW = 0, prevH = 0, prevCelAx = 0, prevCelAy = 0;
	uint32 changedAt = 0;          ///< when the cel last changed (ms)
	uint32 period = 0;             ///< time between cel changes (ms), smoothed
	// What the screen shows now, to redraw only on change
	int shownKey = -3;
	int shownX = 0, shownY = 0;
	Common::Rect shownRect;
};

/** The frame an overlay actor shows at some moment. */
struct HdOverlayFrame {
	const HdImage *img;
	int16 w, h;        ///< size in low-res pixels
	int16 ox, oy;      ///< anchor inside the frame as drawn (mirrored when flipped)
	bool flip;
	int hdX, hdY;      ///< anchor on the HD screen
	int key;           ///< which frame: -1 the current cel, -2 the previous cel, >= 0 an in-between
	Common::Rect lowRect() const;
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
	uint16 registerDraw(const HdImage *img, int16 left, int16 top, int16 dstW, int16 dstH, bool mirror, bool overlay = false);
	void setBackground(uint16 id) { _background = id; }
	bool isOverlay(uint16 id) const { return id && _records[id].overlay; }

	// Faces (see above)
	/**
	 * A cel of a view with a face.json was drawn (not by an actor). Returns the record its pixels belong to:
	 * a bust draw gets a new face record, a mouth or eye draw inside the latest bust gets that bust's record;
	 * 0 when it isn't part of a face.
	 */
	uint16 noteFaceDraw(int view, int loop, int cel, int16 left, int16 top, int16 dstW, int16 dstH);
	bool hasFace(int view);
	/** Choose every face's frame for ``now`` (ms). */
	void updateFaces(uint32 now);
	Common::Array<HdFace> &faces() { return _faces; }
	const HdRecord &record(uint16 id) const { return _records[id]; }

	// In-betweens (see above)
	bool tweensEnabled() const { return _tweensEnabled; }
	bool hasTweens(int view);
	/** An actor was drawn with this cel at this place; tracks its cel changes and their timing. */
	void noteActor(uint32 actor, const HdActor &drawn);
	Common::HashMap<uint32, HdActor> &actors() { return _actors; }
	/** The frame ``a`` shows at ``now`` (ms). False when there's no HD image for it. */
	bool actorFrame(const HdActor &a, uint32 now, HdOverlayFrame &f);
	/**
	 * Draw an overlay frame into the HD frame buffer within low-res rect ``clip``. ``priority`` is the low-res
	 * priority screen; the frame is hidden where it holds something in front of the actor.
	 */
	void drawOverlay(const HdOverlayFrame &f, byte priority, uint16 record, const byte *low, const uint16 *prov,
		const byte *priScreen, int lowPitch, const Common::Rect &clip, const byte *livePal, byte *out, int outPitch,
		const Graphics::PixelFormat &fmt) const;

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
	const Common::Array<HdTween> *tweens(int view, int loop, int cel);
	void loadTweens(int view);
	int loadFace(int view);
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
	bool _tweensEnabled;
	Common::HashMap<int, Common::HashMap<Common::String, Common::Array<HdTween> > > _tweens; ///< view -> "loop.cel"
	Common::HashMap<uint32, HdActor> _actors;
	bool _facesEnabled;
	Common::HashMap<int, int> _faceIndex; ///< view -> index into _faces, or -1 for none
	Common::Array<HdFace> _faces;
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
	/** The low-res priority screen, for hiding overlay frames behind what's in front of them. */
	void setPriorityScreen(const byte *priority) { _priority = priority; }
	/** Called often (60 fps): redraw overlay actors whose frame or position has moved on. */
	void tick();

private:
	void present(const Common::Rect &r);
	void dumpFrame();
	/** Grow ``r`` to whole faces whose frame changed, so a face never shows two frames at once. */
	void extendFaces(Common::Rect &r);
	/** Overlay actors the displayed frame holds, lowest priority first. */
	void visibleActors(Common::Array<uint32> &out);

	GfxHd *_hd;
	const int _n;
	const uint16 *_provSrc;
	const byte *_priority;
	uint16 *_currentProv;
	int32 *_currentShift; ///< per displayed pixel: source position - displayed position (see GfxHd::compose)
	byte *_cursorBuffer;
	uint _cursorBufferSize;
	Common::Path _dumpPath;
	uint32 _lastDump;
	bool _dumpPending; ///< a present was skipped by the dump interval; dump it once the interval is over
	uint32 _dumpInterval;
	int _dumpCount;
};

} // End of namespace Sci

#endif // SCI_GRAPHICS_HD_H
