// GlyphwareHost — engine-side host for the glyphware font engine.
//
// glyphware is storage-agnostic: it asks a FontLoader for font bytes by key.
// Here the key is a UTF-8 storage path (or a "@gdi:<name>" installed GDI font)
// and the bytes come from the engine's storage layer / GDI, so glyphware faces
// are backed by the same on-memory font data the engine uses. This is the seam
// through which the core (drawShapedText, the glyphware font rasterizer, and,
// later, plugins via tp_stub) reach one shared font engine.
#pragma once
#ifdef KRKRZ_USE_GLYPHWARE

#include "glyphware/Registry.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace glyphware { class Face; }

// The process-wide glyphware registry, backed by a storage/GDI FontLoader.
glyphware::Registry& TVPGetGlyphwareRegistry();

// Open a NON-shared face over the same shared bytes. Face state (pixel size,
// transform and above all variable-font axis coordinates) lives in the FT_Face,
// so a consumer that needs its own instance of a font — e.g. one weight of a
// variable font while the engine draws another — must not take the registry's
// shared face. The blob still comes from the shared loader, so no font bytes
// are duplicated. Returns nullptr when the key does not resolve.
std::shared_ptr<glyphware::Face> TVPGlyphwareOpenPrivateFace(const std::string& keyU8,
                                                             int faceIndex);

// Resolve one font token to a glyphware loader key, in priority order:
//   1. a data/fonts.json declared name  -> its storage path
//   2. an existing storage file/path    -> that path as-is
//   3. (WINVER) an installed GDI font    -> "@gdi:<name>"
//   4. otherwise                         -> the token as-is (a storage path)
std::string TVPGlyphwareResolveFontKey(const std::string& tokenU8);

// An empty key falls back to the engine's default face chain
// (TVPGetDefaultFaceNames); returns the effective key (may stay empty when no
// default is known, e.g. WINVER without -deffont).
std::string TVPGlyphwareEffectiveKey(const std::string& keyU8);

// Build a fallback face chain from a (comma-separated) fontKey (UTF-8). Faces
// are shared/cached in the registry. Pixel size is NOT set here — the caller
// sets it per use (layoutLine does it internally; the rasterizer sets it).
void TVPGlyphwareBuildChain(const std::string& keyU8,
                            std::vector<std::shared_ptr<glyphware::Face>>& chain);

// ---- variable-font axis application ---------------------------------------
// Axis coordinates are FACE STATE, so a shared registry face must never get
// setVariations(). These helpers return a PRIVATE face instance (same shared
// bytes, own FT_Face) with the coordinates applied, served from a small LRU
// keyed by (loader key, face index, applied coords).

// Return `shared` itself when none of `coords` names an axis the face has, or
// when every matching value equals the axis default (rule: apply only same-
// named axes, per fallback-chain member). Otherwise return the private
// instance. `shared` may be null (returns null).
std::shared_ptr<glyphware::Face> TVPGlyphwareFaceWithVariations(
	const std::shared_ptr<glyphware::Face>& shared,
	const std::vector<std::pair<tjs_uint32, float>>& coords);

// Map every face of `chain` through TVPGlyphwareFaceWithVariations. No-op for
// an empty coords list.
void TVPGlyphwareApplyVariationsToChain(
	std::vector<std::shared_ptr<glyphware::Face>>& chain,
	const std::vector<std::pair<tjs_uint32, float>>& coords);

// Resolve ONE font token to a face: declared name / storage path / GDI name,
// optionally carrying a variable-font instance suffix "#tag=val[,...]".
// fonts.json declared axes/instance apply first, then the suffix coords
// (private-face LRU; the shared face is never mutated). Returns null when the
// token does not resolve. This is the resolver behind TVPGlyphwareBuildChain
// tokens, the Elements/ThorVG gw bridge (openFaceByKey) and the block-text
// backend, so "MyFont#wght=700" works uniformly in Font.face, Elements JSON
// "font" and text_area.
std::shared_ptr<glyphware::Face> TVPGlyphwareFaceForToken(const std::string& tokenU8);

// ---- unspecified-axis defaults --------------------------------------------
// A variable font referenced with NO wght specification anywhere (suffix,
// fonts.json axes/instance, registered default) is normalized to wght=400
// (matching the CSS font-weight default) by TVPGlyphwareFaceForToken — fonts
// whose fvar default is not Regular (e.g. Noto VF = Thin) still read as
// Regular when unspecified. Faces without a wght axis are untouched, and a
// face whose fvar default is already 400 keeps using the shared face.
//
// Register per-name default axes consulted BEFORE that fallback (weakest
// explicit layer: suffix > fonts.json declaration > this > wght=400).
// `nameU8` is the same token spelling used in Font.face / Elements "font"
// (base name without the "#..." suffix). Empty axes clears the entry.
void TVPGlyphwareSetDefaultVariations(const std::string& nameU8,
                                      const std::string& axesU8);
std::string TVPGlyphwareGetDefaultVariations(const std::string& nameU8);

// Font.defaultUseVarStyle (opt-in) bold/italic -> axis auto-mapping, decided on
// the PRIMARY face of a chain: when the style is (or already was) expressed by
// an axis (wght / slnt / ital), the corresponding synthetic flag is cleared so
// it is not applied twice. No-op when the switch is off.
void TVPGlyphwareAutoStyleCoords(const std::shared_ptr<glyphware::Face>& primary,
	std::vector<std::pair<tjs_uint32, float>>& coords,
	bool& synthBold, bool& synthItalic);

// Registry entry id for a resolved loader key (registers on first use, then
// reuses the same entry — the registry itself does not deduplicate keys).
// A "@gdi:" key resolves its TTC face index up front.
int TVPGlyphwareEntryForKey(const std::string& keyU8);

// Register a fonts.json declared entry (with its declared descriptor: family,
// scripts, cmap ranges, flags, style) so queries can match WITHOUT opening the
// font. Returns the existing id when the key is already registered.
int TVPGlyphwareAddDeclaredEntry(glyphware::FontEntry entry);

// Whether a font NAME resolves to an available font: a fonts.json declaration,
// a runtime/bundled registration (Font.addFont / AddExtraFont), or an installed
// GDI font. Used e.g. to decide the emoji color->mono fallback.
bool TVPGlyphwareFontNameAvailable(const std::string& nameU8);

#endif // KRKRZ_USE_GLYPHWARE
