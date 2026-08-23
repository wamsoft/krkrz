
#include "tjsCommHead.h"
#include "GLFrameBufferObject.h"
#include "DebugIntf.h"
#include "BitmapIntf.h"
#include "LayerBitmapIntf.h"
#include "tvpgl.h"
#include <memory>


bool GLFrameBufferObject::create( GLuint w, GLuint h, bool with_pbo ) {
	destory();

    int pixel_size = 4;
    GLuint fmt;
	if (GLTexture::SupportBGRAFormat()) {
		glformat_ = GL_BGRA_EXT;
		fmt = GL_BGRA8_EXT;
	} else {
		glformat_ = GL_RGBA;
		fmt = GL_RGBA8;
	}

	GLint fb;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb );
	glGenFramebuffers( 1, &framebuffer_id_ );
	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer_id_ );

	// ステンシル付きレンダーバッファ。
	// ステンシルマスク描画 (Emote 等のクリップ) を Offscreen 上でも有効にする。
	// D24S8 を DEPTH/STENCIL に個別アタッチする形は ES3 と
	// ES2 + OES_packed_depth_stencil の双方で有効。
	// 組めない環境では下の completeness チェック後にステンシル無しへ落とす。
	glGenRenderbuffers( 1, &renderbuffer_id_ );
	glBindRenderbuffer( GL_RENDERBUFFER, renderbuffer_id_ );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, renderbuffer_id_ );
	glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, renderbuffer_id_ );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );

	glGenTextures( 1, &texture_id_ );
	glBindTexture( GL_TEXTURE_2D, texture_id_ );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, fmt, w, h, 0, glformat_, GL_UNSIGNED_BYTE, nullptr );

	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id_, 0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( status != GL_FRAMEBUFFER_COMPLETE && renderbuffer_id_ != 0 ) {
		// ステンシル付きで組めない環境ではステンシル無しで再試行する
		// (この場合ステンシルマスク描画は Offscreen 上で機能しない)。
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0 );
		glDeleteRenderbuffers( 1, &renderbuffer_id_ );
		renderbuffer_id_ = 0;
		status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
		if( status == GL_FRAMEBUFFER_COMPLETE ) {
			TVPAddLog( TJS_W("Offscreen framebuffer created without stencil buffer (stencil attachment unsupported).") );
		}
	}
	switch( status ) {
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		TVPAddLog( TJS_W("Not all framebuffer attachment points are framebuffer attachment complete.") );
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
		TVPAddLog( TJS_W("Not all attached images have the same width and height.") );
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
		TVPAddLog( TJS_W("No images are attached to the framebuffer.") );
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		TVPAddLog( TJS_W("The combination of internal formats of the attached images violates an implementation-dependent set of restrictions. ") );
		break;
	}
	bool result = status == GL_FRAMEBUFFER_COMPLETE;
	if( result == false ) {
		// 失敗時は FBO 関連を解放してから framebuffer を復帰し、PBO は作らずに返す。
		destory();
		glBindFramebuffer( GL_FRAMEBUFFER, fb );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return false;
	}
	width_ = w;
	height_ = h;
	glBindFramebuffer( GL_FRAMEBUFFER, fb );

	// PBO はここでは作らない。 pbo() が呼ばれた時点で ensurePBO() が確保する。
	// (エンジン内に pbo() の利用箇所は無く、 常時確保すると FBO 1 枚あたり
	//  さらに w*h*4 を無駄に持つことになるため)
	(void)with_pbo;

    glBindTexture( GL_TEXTURE_2D, 0 );

	// 統計: カラーテクスチャ + D24S8 レンダーバッファ
	mem_bytes_ = (std::uint64_t)w * h * pixel_size;
	if( renderbuffer_id_ != 0 ) {
		mem_bytes_ += (std::uint64_t)w * h * 4; // D24S8
	}
	GLTexture::NoteFboMemory( (std::int64_t)mem_bytes_, 1 );

	return result;
}
//----------------------------------------------------------------------
GLuint GLFrameBufferObject::ensurePBO() const {
	if( pbo_ ) return pbo_;
	if( framebuffer_id_ == 0 || width_ == 0 || height_ == 0 ) return 0;

	const std::size_t size = (std::size_t)width_ * height_ * 4;
	glGenBuffers( 1, &pbo_ );
	if( pbo_ == 0 ) return 0;
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, pbo_ );
	glBufferData( GL_PIXEL_UNPACK_BUFFER, size, 0, GL_DYNAMIC_DRAW );
	glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );

	mem_bytes_ += size;
	GLTexture::NoteFboMemory( (std::int64_t)size, 0 );
	return pbo_;
}
bool GLFrameBufferObject::exchangeTexture( GLuint tex_id ) {
	GLint fb;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb );

	glBindFramebuffer( GL_FRAMEBUFFER, framebuffer_id_ );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_id, 0 );

	GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	switch( status ) {
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		TVPAddLog( TJS_W("Not all framebuffer attachment points are framebuffer attachment complete.") );
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
		TVPAddLog( TJS_W("Not all attached images have the same width and height.") );
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
		TVPAddLog( TJS_W("No images are attached to the framebuffer.") );
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		TVPAddLog( TJS_W("The combination of internal formats of the attached images violates an implementation-dependent set of restrictions. ") );
		break;
	}
	bool result = status == GL_FRAMEBUFFER_COMPLETE;
	if( result ) {
		texture_id_ = tex_id;
	}

	glBindFramebuffer( GL_FRAMEBUFFER, fb );

	return result;
}

void GLFrameBufferObject::bindFramebuffer() {
	if( framebuffer_id_ ) {
		glBindFramebuffer( GL_FRAMEBUFFER, framebuffer_id_ );
		glViewport( 0, 0, width_, height_ );
	}
}
