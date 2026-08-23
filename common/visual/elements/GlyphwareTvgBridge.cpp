//---------------------------------------------------------------------------
// ThorVG "gw" テキストローダ (TVG_LOADER_GW) 用の glyphware ブリッジ実装。
//
// ThorVG フォークの inc/thorvg_gw_bridge.h が定義するホスト注入 I/F を
// glyphware (統一フォントエンジン) で実装し、静的初期化時に登録する。
// これにより ThorVG 内蔵の FreeType+HarfBuzz スタック (src/loaders/ft) が
// 不要になり、Elements / layerExVector のテキストが本体と同じフォント
// エンジンで描画される。
//
// 単位規約 (thorvg_gw_bridge.h 参照):
//  - face の pixel size を unitsPerEm に固定 → shaping/metrics の出力が
//    そのままフォントユニットになる
//  - shapeRun の yOffset は y-down へ符号反転 (hb は y-up)
//  - glyphOutline は y 反転 + conic→cubic 昇格 + scale 適用
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#if defined(KRKRZ_USE_GLYPHWARE)

#include "thorvg_gw_bridge.h"
#include "GlyphwareHost.h"     // TVPGlyphwareResolveFontKey / EntryForKey / Registry
#include "glyphware/Face.h"
#include "glyphware/Shaper.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// copy=false 用の非所有 blob (呼び出し元が closeFace までバイトを保持する契約)
class tTVPBorrowedFontBlob : public glyphware::FontBlob {
public:
	tTVPBorrowedFontBlob(const std::uint8_t* data, std::size_t size)
		: Data(data), Size(size) {}
	const std::uint8_t* data() const noexcept override { return Data; }
	std::size_t size() const noexcept override { return Size; }
private:
	const std::uint8_t* Data;
	std::size_t Size;
};

struct tTVPGwBridgeFace
{
	std::shared_ptr<glyphware::Face> Face;
	std::string Family;
	std::string Style;
	tjs_uint16 Upem = 0;
	tjs_int16 Ascender = 0;
	tjs_int16 Descender = 0;   // FT 規約 (負値)
	tjs_int16 Height = 0;
	std::vector<glyphware::ShapedGlyph> ShapeBuf;
};

tTVPGwBridgeFace* AsFace(void* h) { return static_cast<tTVPGwBridgeFace*>(h); }

void* GwOpenFace(void*, const char* data, uint32_t size, int copy)
{
	if (!data || size == 0) return nullptr;
	std::shared_ptr<glyphware::FontBlob> blob;
	if (copy) {
		blob = std::make_shared<glyphware::OwnedFontBlob>(
			reinterpret_cast<const std::uint8_t*>(data), size);
	} else {
		blob = std::make_shared<tTVPBorrowedFontBlob>(
			reinterpret_cast<const std::uint8_t*>(data), size);
	}
	auto face = glyphware::Face::open(blob, "@tvg", 0);
	if (!face) return nullptr;

	auto* f = new tTVPGwBridgeFace();
	f->Face = std::move(face);

	// unitsPerEm はサイズ未設定でも取得できる
	glyphware::LineMetrics lm0 = f->Face->lineMetrics();
	f->Upem = static_cast<tjs_uint16>(lm0.unitsPerEm);
	if (f->Upem == 0) f->Upem = 1000;

	// pixel size = upem に固定 → 以降の shaping/metrics がフォントユニット
	f->Face->setPixelSize(f->Upem);
	glyphware::LineMetrics lm = f->Face->lineMetrics();
	f->Ascender  = static_cast<tjs_int16>(std::lround(lm.ascent));
	f->Descender = static_cast<tjs_int16>(-std::lround(lm.descent));
	f->Height    = static_cast<tjs_int16>(std::lround(lm.ascent + lm.descent + lm.lineGap));

	const glyphware::FontDescriptor& d = f->Face->descriptor();
	f->Family = !d.typographicFamily.empty() ? d.typographicFamily : d.family;
	f->Style = d.subfamily;
	return f;
}

// ホストキー (フォント名 / storage パス / resource:// / GDI 名) で face を開く。
// glyphware レジストリの共有 Face (FontStream 共有バッファ) をそのまま使うので
// バイトコピーは発生しない (本体 drawText と同一 Face 実体)。
void* GwOpenFaceByKey(void*, const char* keyU8)
{
	if (!keyU8 || !*keyU8) return nullptr;
	// TVPGlyphwareFaceForToken は宣言名/パス/GDI 名の解決に加えて、
	// fonts.json 宣言軸と可変軸 suffix ("key#wght=700" 等) も適用する。
	// Elements JSON の "font": "MyFont#wght=700" はこの経路で face 化される。
	auto face = TVPGlyphwareFaceForToken(std::string(keyU8));
	if (!face) return nullptr;

	auto* f = new tTVPGwBridgeFace();
	f->Face = std::move(face);

	glyphware::LineMetrics lm0 = f->Face->lineMetrics();
	f->Upem = static_cast<tjs_uint16>(lm0.unitsPerEm);
	if (f->Upem == 0) f->Upem = 1000;

	f->Face->setPixelSize(f->Upem);
	glyphware::LineMetrics lm = f->Face->lineMetrics();
	f->Ascender  = static_cast<tjs_int16>(std::lround(lm.ascent));
	f->Descender = static_cast<tjs_int16>(-std::lround(lm.descent));
	f->Height    = static_cast<tjs_int16>(std::lround(lm.ascent + lm.descent + lm.lineGap));

	const glyphware::FontDescriptor& d = f->Face->descriptor();
	f->Family = !d.typographicFamily.empty() ? d.typographicFamily : d.family;
	f->Style = d.subfamily;
	return f;
}

void GwCloseFace(void*, void* face)
{
	delete AsFace(face);
}

const char* GwFaceFamily(void*, void* face)
{
	auto* f = AsFace(face);
	return f->Family.empty() ? nullptr : f->Family.c_str();
}

const char* GwFaceStyle(void*, void* face)
{
	auto* f = AsFace(face);
	return f->Style.empty() ? nullptr : f->Style.c_str();
}

uint32_t GwGlyphIndex(void*, void* face, uint32_t codepoint)
{
	return AsFace(face)->Face->glyphIndex(static_cast<char32_t>(codepoint));
}

uint16_t GwUnitsPerEm(void*, void* face)
{
	return AsFace(face)->Upem;
}

int32_t GwGlyphAdvance(void*, void* face, uint32_t gid)
{
	auto* f = AsFace(face);
	// キー共有 face は本体側 (drawText 等) がサイズを変えることがあるので、
	// フォントユニット出力の前提 (pixel size = upem) を毎回張り直す
	f->Face->setPixelSize(f->Upem);
	glyphware::GlyphMetrics m;
	if (!f->Face->glyphMetrics(gid, m)) return 0;
	return static_cast<int32_t>(std::lround(m.advanceX));
}

int16_t GwAscender(void*, void* face)  { return AsFace(face)->Ascender; }
int16_t GwDescender(void*, void* face) { return AsFace(face)->Descender; }
int16_t GwHeight(void*, void* face)    { return AsFace(face)->Height; }

// glyphware OutlineSink → TvgGwPathSink アダプタ。
// フォントユニット y-up 入力を y 反転 + scale して転送し、conic (quad) は
// cubic へ昇格する (ThorVG RenderPath は cubic のみ)。
class tTVPGwOutlineAdapter : public glyphware::OutlineSink {
public:
	tTVPGwOutlineAdapter(const TvgGwPathSink* sink, float scale)
		: Sink(sink), Scale(scale) {}
	void moveTo(float x, float y) override {
		CurX = x * Scale; CurY = -y * Scale;
		Sink->moveTo(Sink->ctx, CurX, CurY);
	}
	void lineTo(float x, float y) override {
		CurX = x * Scale; CurY = -y * Scale;
		Sink->lineTo(Sink->ctx, CurX, CurY);
	}
	void quadTo(float cx, float cy, float x, float y) override {
		// 2次→3次昇格: C1 = P0 + 2/3 (Q - P0), C2 = P1 + 2/3 (Q - P1)
		float qx = cx * Scale, qy = -cy * Scale;
		float ex = x * Scale,  ey = -y * Scale;
		float c1x = CurX + (qx - CurX) * (2.0f / 3.0f);
		float c1y = CurY + (qy - CurY) * (2.0f / 3.0f);
		float c2x = ex + (qx - ex) * (2.0f / 3.0f);
		float c2y = ey + (qy - ey) * (2.0f / 3.0f);
		Sink->cubicTo(Sink->ctx, c1x, c1y, c2x, c2y, ex, ey);
		CurX = ex; CurY = ey;
	}
	void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) override {
		Sink->cubicTo(Sink->ctx, c1x * Scale, -c1y * Scale,
			c2x * Scale, -c2y * Scale, x * Scale, -y * Scale);
		CurX = x * Scale; CurY = -y * Scale;
	}
	void close() override {
		Sink->close(Sink->ctx);
	}
private:
	const TvgGwPathSink* Sink;
	float Scale;
	float CurX = 0.f, CurY = 0.f;
};

int GwGlyphOutline(void*, void* face, uint32_t gid, float scale, const TvgGwPathSink* sink)
{
	if (!sink) return 0;
	tTVPGwOutlineAdapter adapter(sink, scale);
	return AsFace(face)->Face->glyphOutline(gid, adapter) ? 1 : 0;
}

uint32_t GwShapeRun(void*, void* face, const char* utf8, uint32_t len,
	const char* locale,
	void (*emit)(void* emitCtx, const TvgGwShapedGlyph* g), void* emitCtx)
{
	auto* f = AsFace(face);
	if (!utf8 || len == 0 || !emit) return 0;

	// フォントユニット出力の前提 (pixel size = upem) を張り直す (共有 face 対応)
	f->Face->setPixelSize(f->Upem);

	glyphware::ShapeOptions opts;
	if (locale && *locale) opts.language = locale;
	opts.guessSegmentProperties = true;   // FT ローダとパリティ

	f->ShapeBuf.clear();
	glyphware::shapeRun(*f->Face, std::string_view(utf8, len), opts, f->ShapeBuf);

	for (const auto& g : f->ShapeBuf) {
		TvgGwShapedGlyph out;
		out.gid = g.gid;
		out.xAdvance = g.xAdvance;
		out.xOffset = g.xOffset;
		out.yOffset = -g.yOffset;   // hb (y-up) → y-down
		out.cluster = g.cluster;
		emit(emitCtx, &out);
	}
	return static_cast<uint32_t>(f->ShapeBuf.size());
}

int GwIsVariable(void*, void* face)
{
	auto* f = AsFace(face);
	return (f->Face && !f->Face->descriptor().axes.empty()) ? 1 : 0;
}

// 本体側のブリッジ実体。exe 内 thorvg へは tvgGwSetBridge で登録し、独自に
// thorvg を静的リンクするプラグイン (layerExVector 等) へは
// TVPGetFontTvgBridge (FontServiceIntf) 経由でこのポインタを渡す
TvgGwBridge gTVPGwBridge = {};

} // namespace

extern void TVPSetFontTvgBridgePointer(const void * bridge);

namespace {

// 静的初期化でブリッジを登録する (tvgGwSetBridge は POD グローバルへの代入
// だけなので初期化順序に依存しない。最初の tvg::Text 使用より前なら良い)
struct tTVPGwBridgeInstaller
{
	tTVPGwBridgeInstaller()
	{
		gTVPGwBridge.ctx = nullptr;
		gTVPGwBridge.openFace = GwOpenFace;
		gTVPGwBridge.openFaceByKey = GwOpenFaceByKey;
		gTVPGwBridge.closeFace = GwCloseFace;
		gTVPGwBridge.faceFamily = GwFaceFamily;
		gTVPGwBridge.faceStyle = GwFaceStyle;
		gTVPGwBridge.glyphIndex = GwGlyphIndex;
		gTVPGwBridge.unitsPerEm = GwUnitsPerEm;
		gTVPGwBridge.glyphAdvance = GwGlyphAdvance;
		gTVPGwBridge.ascender = GwAscender;
		gTVPGwBridge.descender = GwDescender;
		gTVPGwBridge.height = GwHeight;
		gTVPGwBridge.glyphOutline = GwGlyphOutline;
		gTVPGwBridge.shapeRun = GwShapeRun;
		gTVPGwBridge.isVariable = GwIsVariable;
		tvgGwSetBridge(&gTVPGwBridge);
		TVPSetFontTvgBridgePointer(&gTVPGwBridge);
	}
} gTVPGwBridgeInstaller;

} // namespace

#endif // KRKRZ_USE_GLYPHWARE
