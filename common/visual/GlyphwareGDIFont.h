// GlyphwareGDIFont — extract an installed GDI font's bytes for the glyphware
// (FreeType) path. WINVER only: on Windows a system-installed font (including
// ones addFont registered in-memory via AddFontMemResourceEx, which have NO
// file path) can only be obtained as bytes through GDI's GetFontData. This is
// the same mechanism the classic FreeType path uses (tNativeFreeTypeFace); here
// we snapshot the whole font file once into memory so a glyphware memory-face
// can back onto it. The implementation lives in win32/visual/ (GDI + FreeType);
// this header is storage-agnostic so common/ code can call it under __WINVER__.
#pragma once
#ifdef KRKRZ_USE_GLYPHWARE

#include "tjsCommHead.h"
#include <string>

// A glyphware font key that names an installed GDI font (as opposed to a
// storage path) carries this prefix; the loader GDI-extracts it.
#define TVP_GLYPHWARE_GDI_KEY_PREFIX "@gdi:"

struct tTVPGDIFontData {
	std::string bytes;   // full font-file bytes (raw), snapshot via GetFontData
	int faceIndex = 0;   // face index within a TTC (0 for single-face fonts)
};

// WINVER only. Returns cached full bytes + resolved face index for an installed
// GDI font selected by `fontname`, or nullptr if GDI cannot provide it. The
// result is owned by an internal cache (stable pointer) and negative results
// are cached too. Regular weight/upright is extracted; bold/italic styling is
// applied synthetically by glyphware.
const tTVPGDIFontData* TVPGetGDIFontData(const tjs_string& fontname);

#endif // KRKRZ_USE_GLYPHWARE
