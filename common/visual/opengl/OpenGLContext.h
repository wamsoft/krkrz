#pragma once

// GL操作用 (for OGLDrawDevice)
class iTVPGLContext {
public:
    virtual int Release() = 0;
    virtual void *NativeWindow() = 0;
    virtual void GetSurfaceSize(int *width, int *height) = 0;
    virtual void MakeCurrent() = 0;
    virtual void Swap() = 0;
	virtual void SetWaitVSync( bool b ) = 0;

    // nativeWindow 用の GL コンテキストを取得する。
    // separateShared=false (既定): 画面描画デバイス用。ウィンドウの主コンテキストを返す。
    // separateShared=true: オフスクリーン合成 (GLCompositor) 用。画面デバイスと
    //   FBO/GL ステートを競合させないよう専用コンテキストを生成する。現在のコンテキストが
    //   あればそれと共有グループにして (テクスチャ/シェーダ等は共有・FBO/VAO は独立)、
    //   呼び出し側が Release() で破棄する。WINVER (画面 D3D11) では主コンテキストが
    //   そもそも GL 画面と競合しないためこのフラグは無視される。
    static iTVPGLContext *GetContext(void *nativeWindow, bool separateShared = false);
};

// gles初期化用
void InitGLES();
