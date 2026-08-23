//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// フォントサービス (共有フォントストリーム + glyphware) プラグイン公開 API 実装
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "FontServiceIntf.h"

#include "FontStream.h"
#include "StorageIntf.h"         // TVPIsExistentStorage
#include "StorageCache.h"        // TVPCreateSharedMemoryStream
#include "BinaryStreamBuffer.h"
#include "CharacterSet.h"        // TVPUtf8ToUtf16 / TVPUtf16ToUtf8
#include "FontSystem.h"
#include "MsgIntf.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef KRKRZ_USE_GLYPHWARE
#include "GlyphwareHost.h"
#include "glyphware/Face.h"
#include "glyphware/Layout.h"
#include "glyphware/Manifest.h"
#include "glyphware/Registry.h"
#endif

//---------------------------------------------------------------------------
// ThorVG "gw" ローダ用ブリッジポインタの保管 (実体は GlyphwareTvgBridge.cpp が
// 静的初期化時に登録する。Elements/glyphware 無効ビルドでは nullptr のまま)
//---------------------------------------------------------------------------

static const void * TVPFontTvgBridgePointer = nullptr;

void TVPSetFontTvgBridgePointer(const void * bridge)
{
	TVPFontTvgBridgePointer = bridge;
}

const void * TVPGetFontTvgBridge()
{
	return TVPFontTvgBridgePointer;
}

//---------------------------------------------------------------------------
// 共有フォントバイト供給 (FontStream)
//---------------------------------------------------------------------------

iTJSBinaryStream * TVPCreateFontStream(const ttstr & storage)
{
	// TVPGetFontStreamBuffer は共有バッファ (キャッシュ対象外は全読み) を返す
	auto buf = TVPGetFontStreamBuffer(storage);
	return TVPCreateSharedMemoryStream(std::move(buf));
}

tTVPFontBufferHandle TVPAcquireFontBuffer(const ttstr & storage,
	const tjs_uint8 ** data, tjs_uint64 * size)
{
	std::shared_ptr<tTJSBinaryStreamBuffer> buf;
	try { buf = TVPGetFontStreamBuffer(storage); }
	catch(...) { return nullptr; }
	if (!buf) return nullptr;
	if (data) *data = buf->buffer();
	if (size) *size = static_cast<tjs_uint64>(buf->size());
	return new std::shared_ptr<tTJSBinaryStreamBuffer>(std::move(buf));
}

void TVPReleaseFontBuffer(tTVPFontBufferHandle buffer)
{
	delete static_cast<std::shared_ptr<tTJSBinaryStreamBuffer> *>(buffer);
}

//---------------------------------------------------------------------------
#ifdef KRKRZ_USE_GLYPHWARE

namespace {

// ttstr <-> UTF-8 変換
std::string ToU8(const ttstr & s)
{
	std::string out;
	TVPUtf16ToUtf8(out, s.AsStdString());
	return out;
}
ttstr ToTtstr(const std::string & u8)
{
	tjs_string out;
	TVPUtf8ToUtf16(out, u8);
	return ttstr(out.c_str());
}

// TVPFontAcquireFace が返した face の keep-alive (Face* -> {shared_ptr, refcount})
std::map<glyphware::Face*, std::pair<std::shared_ptr<glyphware::Face>, int>> FaceKeepAlive;

// フォールバック連鎖ハンドルの実体
struct tTVPFontFaceChain
{
	std::vector<std::shared_ptr<glyphware::Face>> Faces;
};

glyphware::Face * FaceFromHandle(tTVPFontFaceHandle h)
{
	return static_cast<glyphware::Face *>(h);
}

// fonts.json を glyphware マニフェストとして読み、宣言メタ (family/aliases/
// scripts/ranges/flags/weight 等) 込みでレジストリへ登録する。宣言 ranges が
// あれば収録文字クエリ (containsText) がフォントを開かずに判定できる。
void SyncFontManifestToRegistry()
{
	static bool done = false;
	if (done) return;
	done = true;

	ttstr metaname(TJS_W("fonts.json"));
	try {
		if (!TVPIsExistentStorage(metaname)) return;
		std::unique_ptr<iTJSBinaryStream> st(TVPCreateStream(metaname, TJS_BS_READ));
		if (!st) return;
		tjs_uint64 sz = st->GetSize();
		if (sz == 0 || sz >= 16u * 1024u * 1024u) return;
		std::string text(static_cast<size_t>(sz), '\0');
		st->Read(&text[0], static_cast<tjs_uint>(sz));

		for (auto & entry : glyphware::parseFontManifest(text)) {
			TVPGlyphwareAddDeclaredEntry(std::move(entry));
		}
	} catch (...) {
		// fonts.json 無し/読めない場合は宣言メタ無しで続行
	}
}

// FontSystem が知る name→storage (fonts.json 宣言 + 実行時登録) を glyphware
// レジストリへ同期する (検索用)。fonts.json 分は宣言メタ込みで先に登録され、
// ここでは実行時登録 (Font.addFont / registerFontFile) 分が拾われる。
void SyncDeclaredFontsToRegistry()
{
	if (!TVPFontSystem) return;
	TVPFontSystem->EnsureFontMetadataLoaded();
	SyncFontManifestToRegistry();
	static std::set<tjs_string> synced;
	std::vector<std::pair<tjs_string, tjs_string>> list;
	TVPFontSystem->EnumerateLazyFontStorages(list);
	for (const auto & kv : list) {
		if (synced.count(kv.second)) continue;
		synced.insert(kv.second);
		std::string storageU8;
		TVPUtf16ToUtf8(storageU8, kv.second);
		if (!storageU8.empty()) TVPGlyphwareEntryForKey(storageU8);
	}
}

// FontEntry の descriptor → tTVPFontFaceInfo
void FillFaceInfo(const glyphware::FontEntry & e, tTVPFontFaceInfo * out)
{
	const glyphware::FontDescriptor & d = e.descriptor;
	out->Key = ToTtstr(e.key);
	out->FaceIndex = e.faceIndex;
	out->Family = ToTtstr(!d.typographicFamily.empty() ? d.typographicFamily : d.family);
	out->Subfamily = ToTtstr(d.subfamily);
	out->FullName = ToTtstr(d.fullName);
	out->PostScriptName = ToTtstr(d.postScriptName);
	out->Weight = static_cast<tjs_int>(d.weight);
	out->Slant = static_cast<tjs_int>(d.slant);
	out->Bold = d.bold;
	out->Color = d.color;
	out->Monospace = d.monospace;
	out->Scalable = d.scalable;
}

void SyncDeclaredFontsToRegistry();

// トークンがストレージとしてそのまま開けるか
bool GlyphwareTokenLoadable(const std::string & tokenU8)
{
	tjs_string w;
	if (!TVPUtf8ToUtf16(w, tokenU8)) return false;
	try { return TVPIsExistentStorage(ttstr(w.c_str())); }
	catch (...) { return false; }
}

// 単一トークン → レジストリ entry id (-1 = 解決不能)
int EntryForToken(const ttstr & nameOrPath)
{
	if (TVPFontSystem) TVPFontSystem->EnsureFontMetadataLoaded();
	std::string token = ToU8(nameOrPath);
	if (token.empty()) return -1;
	std::string key = TVPGlyphwareResolveFontKey(token);
	if (key.empty()) return -1;
	if (key == token && !GlyphwareTokenLoadable(token)) {
		// 裸の名前が宣言名/GDI 名として解決できなかった: 登録済みフォントの
		// SFNT 実名 (family/fullName/PostScript 名) でも解決を試みる
		// (fonts.json の宣言 family と実 family の表記差を吸収する)
		SyncDeclaredFontsToRegistry();
		auto & reg = TVPGetGlyphwareRegistry();
		for (int i = 0; i < static_cast<int>(reg.size()); i++) reg.resolve(i);
		std::vector<int> ids = reg.findByName(token);
		if (!ids.empty()) return ids[0];
		return -1;
	}
	return TVPGlyphwareEntryForKey(key);
}

} // namespace

//---------------------------------------------------------------------------
// フォント名解決
//---------------------------------------------------------------------------

ttstr TVPFontResolveKey(const ttstr & nameOrPath)
{
	return ToTtstr(TVPGlyphwareResolveFontKey(ToU8(nameOrPath)));
}

bool TVPFontNameKnown(const ttstr & name)
{
	return TVPGlyphwareFontNameAvailable(ToU8(name));
}

//---------------------------------------------------------------------------
// face / フォールバック連鎖
//---------------------------------------------------------------------------

tTVPFontFaceHandle TVPFontAcquireFace(const ttstr & nameOrPath)
{
	int id = EntryForToken(nameOrPath);
	if (id < 0) return nullptr;
	auto face = TVPGetGlyphwareRegistry().face(id);
	if (!face) return nullptr;
	glyphware::Face * raw = face.get();
	auto it = FaceKeepAlive.find(raw);
	if (it != FaceKeepAlive.end()) it->second.second++;
	else FaceKeepAlive.emplace(raw, std::make_pair(std::move(face), 1));
	return raw;
}

tTVPFontFaceHandle TVPFontAcquireFaceInstance(const ttstr & nameOrPath,
	const tTVPFontVarCoord * coords, tjs_int count)
{
	int id = EntryForToken(nameOrPath);
	if (id < 0) return nullptr;
	const glyphware::FontEntry & e = TVPGetGlyphwareRegistry().entry(id);
	// レジストリ共有 face ではなく専用 face を開く (軸座標は face の状態なので、
	// 共有 face に設定すると本体 drawText や他プラグインの描画まで変わる)
	auto face = TVPGlyphwareOpenPrivateFace(e.key, e.faceIndex);
	if (!face) return nullptr;
	if (coords && count > 0) {
		std::vector<glyphware::VarCoord> v;
		v.reserve(static_cast<std::size_t>(count));
		for (tjs_int i = 0; i < count; i++) v.push_back({coords[i].Tag, coords[i].Value});
		face->setVariations(v);
	}
	glyphware::Face * raw = face.get();
	// 専用 face なのでレジストリのキャッシュには載せず、keep-alive のみ登録する
	FaceKeepAlive.emplace(raw, std::make_pair(std::move(face), 1));
	return raw;
}

bool TVPFontGetFaceData(tTVPFontFaceHandle face, const tjs_uint8 ** data,
	tjs_uint64 * size, tjs_int * faceIndex)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !f->data() || f->size() == 0) return false;
	if (data) *data = f->data();
	if (size) *size = static_cast<tjs_uint64>(f->size());
	if (faceIndex) *faceIndex = static_cast<tjs_int>(f->faceIndex());
	return true;
}

void TVPFontReleaseFace(tTVPFontFaceHandle face)
{
	auto it = FaceKeepAlive.find(FaceFromHandle(face));
	if (it == FaceKeepAlive.end()) return;
	if (--it->second.second <= 0) FaceKeepAlive.erase(it);
}

tTVPFontFaceChainHandle TVPFontAcquireFaceChain(const ttstr & commaSeparatedNames)
{
	auto * chain = new tTVPFontFaceChain();
	std::string key = TVPGlyphwareEffectiveKey(ToU8(commaSeparatedNames));
	if (!key.empty()) TVPGlyphwareBuildChain(key, chain->Faces);
	return chain;
}

void TVPFontReleaseFaceChain(tTVPFontFaceChainHandle chain)
{
	delete static_cast<tTVPFontFaceChain *>(chain);
}

tjs_int TVPFontChainCount(tTVPFontFaceChainHandle chain)
{
	if (!chain) return 0;
	return static_cast<tjs_int>(static_cast<tTVPFontFaceChain *>(chain)->Faces.size());
}

tTVPFontFaceHandle TVPFontChainFaceAt(tTVPFontFaceChainHandle chain, tjs_int index)
{
	if (!chain) return nullptr;
	auto & faces = static_cast<tTVPFontFaceChain *>(chain)->Faces;
	if (index < 0 || index >= static_cast<tjs_int>(faces.size())) return nullptr;
	return faces[index].get();
}

tjs_int TVPFontChainFaceForChar(tTVPFontFaceChainHandle chain,
	tjs_uint32 codepoint, bool preferLast)
{
	if (!chain) return -1;
	auto & faces = static_cast<tTVPFontFaceChain *>(chain)->Faces;
	tjs_int n = static_cast<tjs_int>(faces.size());
	if (preferLast) {
		for (tjs_int i = n - 1; i >= 0; i--)
			if (faces[i]->covers(static_cast<char32_t>(codepoint))) return i;
	} else {
		for (tjs_int i = 0; i < n; i++)
			if (faces[i]->covers(static_cast<char32_t>(codepoint))) return i;
	}
	return -1;
}

//---------------------------------------------------------------------------
// メトリクス / グリフ供給
//---------------------------------------------------------------------------

bool TVPFontGetLineMetrics(tTVPFontFaceHandle face, tjs_int pixelSize,
	tTVPFontLineMetrics * out)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !out || pixelSize <= 0) return false;
	if (!f->setPixelSize(pixelSize)) return false;
	glyphware::LineMetrics m = f->lineMetrics();
	out->Ascent = m.ascent;
	out->Descent = m.descent;
	out->LineGap = m.lineGap;
	out->UnitsPerEm = m.unitsPerEm;
	out->UnderlineOffset = m.underlineOffset;
	out->UnderlineThickness = m.underlineThickness;
	out->StrikeoutOffset = m.strikeoutOffset;
	out->StrikeoutThickness = m.strikeoutThickness;
	return true;
}

tjs_uint32 TVPFontGetGlyphIndex(tTVPFontFaceHandle face, tjs_uint32 codepoint)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f) return 0;
	return f->glyphIndex(static_cast<char32_t>(codepoint));
}

bool TVPFontGetGlyphMetrics(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	tjs_int pixelSize, bool bold, bool italic, tTVPFontGlyphMetrics * out)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !out || pixelSize <= 0) return false;
	if (!f->setPixelSize(pixelSize)) return false;
	glyphware::GlyphMetrics m;
	if (!f->glyphMetrics(glyphId, m, bold, italic)) return false;
	out->AdvanceX = m.advanceX;
	out->AdvanceY = m.advanceY;
	out->BearingX = m.bearingX;
	out->BearingY = m.bearingY;
	out->Width = m.width;
	out->Height = m.height;
	return true;
}

bool TVPFontGetGlyphMetricsEx(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	tjs_int pixelSize, bool bold, bool italic, tjs_int mode,
	tTVPFontGlyphMetrics * out)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !out) return false;

	glyphware::GlyphMetrics m;
	if (mode == TVP_FONT_METRICS_UNSCALED) {
		// サイズ非依存 (フォントユニット)。pixelSize は使わない
		if (!f->glyphMetricsUnscaled(glyphId, m, bold, italic)) return false;
	} else {
		if (pixelSize <= 0) return false;
		if (!f->setPixelSize(pixelSize)) return false;
		const glyphware::Hinting hinting = (mode == TVP_FONT_METRICS_UNHINTED)
			? glyphware::Hinting::Unhinted : glyphware::Hinting::Hinted;
		if (!f->glyphMetrics(glyphId, m, bold, italic, hinting)) return false;
	}
	out->AdvanceX = m.advanceX;
	out->AdvanceY = m.advanceY;
	out->BearingX = m.bearingX;
	out->BearingY = m.bearingY;
	out->Width = m.width;
	out->Height = m.height;
	return true;
}

bool TVPFontRenderGlyphMask(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	const tTVPFontRenderParams * params, tTVPFontGlyphMask * out)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !params || !out) return false;

	glyphware::RenderParams rp;
	rp.transform.xx = params->Transform[0];
	rp.transform.xy = params->Transform[1];
	rp.transform.dx = params->Transform[2];
	rp.transform.yx = params->Transform[3];
	rp.transform.yy = params->Transform[4];
	rp.transform.dy = params->Transform[5];
	rp.bold = params->Bold;
	rp.italic = params->Italic;
	rp.strokeWidth = params->StrokeWidth;
	rp.join = (params->Join == TVP_FONT_JOIN_MITER) ? glyphware::StrokeJoin::Miter
		: (params->Join == TVP_FONT_JOIN_BEVEL) ? glyphware::StrokeJoin::Bevel
		: glyphware::StrokeJoin::Round;
	rp.cap = (params->Cap == TVP_FONT_CAP_BUTT) ? glyphware::StrokeCap::Butt
		: (params->Cap == TVP_FONT_CAP_SQUARE) ? glyphware::StrokeCap::Square
		: glyphware::StrokeCap::Round;
	rp.miterLimit = params->MiterLimit;

	glyphware::GlyphMask mask;
	if (!f->renderGlyphMask(glyphId, rp, mask)) return false;
	out->Left = mask.left;
	out->Top = mask.top;
	out->Width = mask.width;
	out->Height = mask.rows;
	out->Pitch = mask.pitch;
	out->Buffer = mask.buffer;
	return true;
}

tjs_int TVPFontGetColorLayers(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	tjs_int pixelSize, iTVPFontColorLayerSink * sink, float * clipBox)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !sink || pixelSize <= 0) return 0;
	if (!f->setPixelSize(pixelSize)) return 0;

	std::vector<glyphware::ColorLayer> layers;
	glyphware::ColorGlyphBox box;
	if (!f->colorLayers(glyphId, layers, &box)) return 0;

	if (clipBox) {
		clipBox[0] = box.valid ? box.xMin : 0.f;
		clipBox[1] = box.valid ? box.yMin : 0.f;
		clipBox[2] = box.valid ? box.xMax : 0.f;
		clipBox[3] = box.valid ? box.yMax : 0.f;
	}

	std::vector<tTVPFontColorStop> stops;
	for (const glyphware::ColorLayer & l : layers) {
		tTVPFontColorLayer out;
		out.GlyphId = l.gid;
		for (int i = 0; i < 6; i++) out.Transform[i] = l.transform[i];
		out.PaintKind = (l.paint.kind == glyphware::PaintKind::LinearGradient) ? TVP_FONT_PAINT_LINEAR
			: (l.paint.kind == glyphware::PaintKind::RadialGradient) ? TVP_FONT_PAINT_RADIAL
			: TVP_FONT_PAINT_SOLID;
		out.R = l.paint.r; out.G = l.paint.g; out.B = l.paint.b; out.A = l.paint.a;
		out.X0 = l.paint.x0; out.Y0 = l.paint.y0;
		out.X1 = l.paint.x1; out.Y1 = l.paint.y1;
		out.R0 = l.paint.r0; out.R1 = l.paint.r1;

		stops.clear();
		stops.reserve(l.paint.stops.size());
		for (const glyphware::ColorStop & s : l.paint.stops) {
			tTVPFontColorStop cs;
			cs.Offset = s.offset;
			cs.R = s.r; cs.G = s.g; cs.B = s.b; cs.A = s.a;
			stops.push_back(cs);
		}
		out.StopCount = static_cast<tjs_int>(stops.size());
		out.Stops = stops.empty() ? nullptr : stops.data();

		sink->Layer(out);
	}
	return static_cast<tjs_int>(layers.size());
}

//---------------------------------------------------------------------------
// バリアブルフォント
//---------------------------------------------------------------------------

tjs_int TVPFontGetVarAxes(tTVPFontFaceHandle face, tTVPFontVarAxis * out, tjs_int maxCount)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f) return 0;
	const std::vector<glyphware::VarAxis> & axes = f->descriptor().axes;
	const tjs_int total = static_cast<tjs_int>(axes.size());
	if (out && maxCount > 0) {
		const tjs_int n = total < maxCount ? total : maxCount;
		for (tjs_int i = 0; i < n; i++) {
			out[i].Tag = axes[i].tag;
			out[i].MinValue = axes[i].minValue;
			out[i].DefaultValue = axes[i].defaultValue;
			out[i].MaxValue = axes[i].maxValue;
		}
	}
	return total;
}

bool TVPFontSetVariations(tTVPFontFaceHandle face, const tTVPFontVarCoord * coords,
	tjs_int count)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !coords || count <= 0) return false;
	std::vector<glyphware::VarCoord> v;
	v.reserve(static_cast<std::size_t>(count));
	for (tjs_int i = 0; i < count; i++) v.push_back({coords[i].Tag, coords[i].Value});
	return f->setVariations(v);
}

namespace {
// glyphware OutlineSink → iTVPFontOutlineSink アダプタ
class tTVPOutlineSinkAdapter : public glyphware::OutlineSink
{
	iTVPFontOutlineSink * Sink;
public:
	explicit tTVPOutlineSinkAdapter(iTVPFontOutlineSink * sink) : Sink(sink) {}
	void moveTo(float x, float y) override { Sink->MoveTo(x, y); }
	void lineTo(float x, float y) override { Sink->LineTo(x, y); }
	void quadTo(float cx, float cy, float x, float y) override { Sink->QuadTo(cx, cy, x, y); }
	void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) override {
		Sink->CubicTo(c1x, c1y, c2x, c2y, x, y);
	}
	void close() override { Sink->ClosePath(); }
};
} // namespace

bool TVPFontGetGlyphOutline(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	bool bold, bool italic, iTVPFontOutlineSink * sink)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !sink) return false;
	tTVPOutlineSinkAdapter adapter(sink);
	return f->glyphOutline(glyphId, adapter, bold, italic);
}

bool TVPFontGetGlyphBitmap(tTVPFontFaceHandle face, tjs_uint32 glyphId,
	tjs_int pixelSize, bool color, bool bold, bool italic,
	tTVPFontGlyphBitmap * out)
{
	glyphware::Face * f = FaceFromHandle(face);
	if (!f || !out || pixelSize <= 0) return false;
	if (!f->setPixelSize(pixelSize)) return false;
	glyphware::GlyphBitmap bmp;
	if (!f->glyphBitmap(glyphId, color, bmp, bold, italic)) return false;
	// Mono は Face 側で 8bit グレー正規化済みなので GRAY として返す
	out->Format = (bmp.format == glyphware::BitmapFormat::BGRA) ?
		TVP_FONT_BITMAP_BGRA : TVP_FONT_BITMAP_GRAY;
	out->Left = bmp.left;
	out->Top = bmp.top;
	out->Width = bmp.width;
	out->Height = bmp.rows;
	out->Pitch = bmp.pitch;
	out->Buffer = bmp.buffer;
	return true;
}

//---------------------------------------------------------------------------
// 1 行レイアウト
//---------------------------------------------------------------------------

bool TVPFontShapeLine(tTVPFontFaceChainHandle chain, const ttstr & text,
	tjs_int pixelSize, tjs_int baseDirection, iTVPFontShapeSink * sink)
{
	if (!chain || !sink || pixelSize <= 0) return false;
	auto * c = static_cast<tTVPFontFaceChain *>(chain);
	if (c->Faces.empty()) return false;

	glyphware::BaseDirection base = glyphware::BaseDirection::Auto;
	if (baseDirection == TVP_FONT_BASEDIR_LTR) base = glyphware::BaseDirection::LTR;
	else if (baseDirection == TVP_FONT_BASEDIR_RTL) base = glyphware::BaseDirection::RTL;

	std::string utf8 = ToU8(text);
	glyphware::LineLayout layout =
		glyphware::layoutLine(utf8, base, c->Faces, pixelSize);

	sink->Begin(static_cast<tjs_int>(layout.glyphs.size()),
		layout.width, layout.ascent, layout.descent);
	for (const auto & g : layout.glyphs) {
		tTVPFontShapedGlyph sg;
		sg.GlyphId = g.gid;
		sg.FaceIndexInChain = -1;
		for (std::size_t i = 0; i < c->Faces.size(); i++) {
			if (c->Faces[i].get() == g.face) { sg.FaceIndexInChain = static_cast<tjs_int>(i); break; }
		}
		sg.X = g.x;
		sg.Y = g.y;
		sg.XOffset = g.xOffset;
		sg.YOffset = g.yOffset;
		sg.Advance = g.advance;
		sg.Cluster = g.cluster;
		sg.RTL = g.rtl;
		sink->Glyph(sg);
	}
	return true;
}

//---------------------------------------------------------------------------
// リッチ検索 / メタデータ
//---------------------------------------------------------------------------

tjs_int TVPFontQueryFaces(const tTVPFontQueryParams & params, iTVPFontQuerySink * sink)
{
	if (!sink) return 0;
	SyncDeclaredFontsToRegistry();
	auto & reg = TVPGetGlyphwareRegistry();

	glyphware::FontQuery q;
	if (!params.Name.IsEmpty()) q.name = ToU8(params.Name);
	if (params.Weight >= 0) q.weight = static_cast<glyphware::Weight>(params.Weight);
	if (params.Slant >= 0) q.slant = static_cast<glyphware::Slant>(params.Slant);
	if (!params.Script.IsEmpty()) q.script = ToU8(params.Script);
	if (!params.ContainsText.IsEmpty()) q.containsText = ToU8(params.ContainsText);
	if (params.Monospace >= 0) q.monospace = (params.Monospace != 0);
	if (params.Color >= 0) q.color = (params.Color != 0);

	std::vector<int> ids = reg.query(q);
	// 同一 (key, faceIndex) の重複エントリを除外しつつ通知。
	// 宣言メタ (fonts.json) がある entry はフォントを開かず宣言値のまま返す
	// (開かず判定)。宣言名も無い裸キーのみ SFNT を解決する。
	std::set<std::pair<std::string, int>> seen;
	tjs_int count = 0;
	for (int id : ids) {
		const glyphware::FontEntry & e = reg.entry(id);
		if (!seen.insert(std::make_pair(e.key, e.faceIndex)).second) continue;
		if (e.descriptor.family.empty()) reg.resolve(id);
		tTVPFontFaceInfo info;
		FillFaceInfo(reg.entry(id), &info);
		sink->Found(info);
		count++;
	}
	return count;
}

bool TVPFontGetFaceInfo(const ttstr & nameOrPath, tTVPFontFaceInfo * out)
{
	if (!out) return false;
	int id = EntryForToken(nameOrPath);
	if (id < 0) return false;
	auto & reg = TVPGetGlyphwareRegistry();
	if (!reg.resolve(id)) return false;
	FillFaceInfo(reg.entry(id), out);
	return true;
}

#else // KRKRZ_USE_GLYPHWARE

//---------------------------------------------------------------------------
// glyphware 無効ビルド: face/グリフ/検索系は失敗を返すスタブ
//---------------------------------------------------------------------------

ttstr TVPFontResolveKey(const ttstr & nameOrPath) { return nameOrPath; }
bool TVPFontNameKnown(const ttstr & name)
{
	if (TVPFontSystem) {
		tjs_string storage;
		if (TVPFontSystem->GetLazyFontStorage(name.AsStdString(), storage)) return true;
	}
	return false;
}
tTVPFontFaceHandle TVPFontAcquireFace(const ttstr &) { return nullptr; }
tTVPFontFaceHandle TVPFontAcquireFaceInstance(const ttstr &, const tTVPFontVarCoord *,
	tjs_int) { return nullptr; }
bool TVPFontGetFaceData(tTVPFontFaceHandle, const tjs_uint8 **, tjs_uint64 *,
	tjs_int *) { return false; }
void TVPFontReleaseFace(tTVPFontFaceHandle) {}
tTVPFontFaceChainHandle TVPFontAcquireFaceChain(const ttstr &) { return nullptr; }
void TVPFontReleaseFaceChain(tTVPFontFaceChainHandle) {}
tjs_int TVPFontChainCount(tTVPFontFaceChainHandle) { return 0; }
tTVPFontFaceHandle TVPFontChainFaceAt(tTVPFontFaceChainHandle, tjs_int) { return nullptr; }
tjs_int TVPFontChainFaceForChar(tTVPFontFaceChainHandle, tjs_uint32, bool) { return -1; }
bool TVPFontGetLineMetrics(tTVPFontFaceHandle, tjs_int, tTVPFontLineMetrics *) { return false; }
tjs_uint32 TVPFontGetGlyphIndex(tTVPFontFaceHandle, tjs_uint32) { return 0; }
bool TVPFontGetGlyphMetrics(tTVPFontFaceHandle, tjs_uint32, tjs_int, bool, bool,
	tTVPFontGlyphMetrics *) { return false; }
bool TVPFontGetGlyphMetricsEx(tTVPFontFaceHandle, tjs_uint32, tjs_int, bool, bool,
	tjs_int, tTVPFontGlyphMetrics *) { return false; }
bool TVPFontRenderGlyphMask(tTVPFontFaceHandle, tjs_uint32,
	const tTVPFontRenderParams *, tTVPFontGlyphMask *) { return false; }
tjs_int TVPFontGetColorLayers(tTVPFontFaceHandle, tjs_uint32, tjs_int,
	iTVPFontColorLayerSink *, float *) { return 0; }
tjs_int TVPFontGetVarAxes(tTVPFontFaceHandle, tTVPFontVarAxis *, tjs_int) { return 0; }
bool TVPFontSetVariations(tTVPFontFaceHandle, const tTVPFontVarCoord *, tjs_int)
	{ return false; }
bool TVPFontGetGlyphOutline(tTVPFontFaceHandle, tjs_uint32, bool, bool,
	iTVPFontOutlineSink *) { return false; }
bool TVPFontGetGlyphBitmap(tTVPFontFaceHandle, tjs_uint32, tjs_int, bool, bool, bool,
	tTVPFontGlyphBitmap *) { return false; }
bool TVPFontShapeLine(tTVPFontFaceChainHandle, const ttstr &, tjs_int, tjs_int,
	iTVPFontShapeSink *) { return false; }
tjs_int TVPFontQueryFaces(const tTVPFontQueryParams &, iTVPFontQuerySink *) { return 0; }
bool TVPFontGetFaceInfo(const ttstr &, tTVPFontFaceInfo *) { return false; }

#endif // KRKRZ_USE_GLYPHWARE
