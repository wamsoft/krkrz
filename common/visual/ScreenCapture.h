//---------------------------------------------------------------------------
//!@file 画面キャプチャ要求の受け渡し (エージェント駆動 / 自動テスト用、 SDL3)
//
// Agent.captureScreen() が「次フレームの present 直前に overlay 込みの実画面を
// 読み戻して PNG 保存する」要求を立て、 DrawDevice の Show() がその要求を消費
// して実際の読み戻し + 保存を行うための、 デバイス非依存な受け渡し層。
//
// 読み戻し自体 (SDL_RenderReadPixels / glReadPixels) はデバイス側で行い、 得た
// ARGB8888 (メモリ上 BGRA) バッファをこのヘッダの TVPSaveCapturedImage に渡す。
//---------------------------------------------------------------------------
#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#include "tjsCommHead.h"

//! @brief キャプチャ要求。 w<=0 / h<=0 は「全画面」を意味する。
struct tTVPScreenCaptureReq
{
	ttstr path;
	int x = 0, y = 0, w = 0, h = 0;
};

//! @brief キャプチャを要求する (Agent.captureScreen から)。 実際の保存は次フレーム。
void TVPRequestScreenCapture(const ttstr& path, int x, int y, int w, int h);

//! @brief 保留中の要求があるか (DrawDevice::Show の頭で確認)。
bool TVPHasPendingScreenCapture();

//! @brief 保留中の要求を取り出してクリアする。 無ければ false。
bool TVPTakeScreenCaptureRequest(tTVPScreenCaptureReq& out);

//! @brief ARGB8888 (メモリ上 B,G,R,A 並び) バッファを画像ファイルに保存する。
//!        pitch_bytes は 1 行のバイト数。 mode は TVPSaveImage のモード ("png" 等)。
//! @return 成功なら true。
bool TVPSaveCapturedImage(const ttstr& path, const void* pixels,
                          int w, int h, int pitch_bytes,
                          const ttstr& mode);

//! @brief 直近のキャプチャ結果を記録 / 取得する (Agent が完了とパスを知るため)。
void TVPSetScreenCaptureResult(const ttstr& path, int w, int h, bool ok);
bool TVPGetLastScreenCapture(ttstr& path, int& w, int& h, bool& ok);

//! @brief GL の glReadPixels(GL_RGBA, bottom-up) 結果から保留中の要求を充足する。
//!        上下反転 + RGBA→BGRA(ARGB8888) 変換 + 要求矩形クロップ + 保存 + 結果記録
//!        までを行う。 GL デバイスの Show() が Swap 直前にフルサーフェスを読んで
//!        このヘルパに渡す。 req は TVPTakeScreenCaptureRequest で取得済みのもの。
//! @param rgba_bottomup  glReadPixels(0,0,fullw,fullh,GL_RGBA,GL_UNSIGNED_BYTE)
//! @return 保存成功なら true。
bool TVPSaveGLReadback(const tTVPScreenCaptureReq& req,
                       const void* rgba_bottomup, int fullw, int fullh);

#endif
