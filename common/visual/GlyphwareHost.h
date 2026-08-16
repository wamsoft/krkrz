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
#include <vector>

namespace glyphware { class Face; }

// The process-wide glyphware registry, backed by a storage/GDI FontLoader.
glyphware::Registry& TVPGetGlyphwareRegistry();

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
