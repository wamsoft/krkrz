#ifndef GLFrameBufferObjectH
#define GLFrameBufferObjectH

#include "OpenGLHeader.h"
#include "ComplexRect.h"
#include "GLTexture.h"

class GLFrameBufferObject {
	
protected:
	GLuint texture_id_;
	GLint glformat_;
	GLuint framebuffer_id_;
	GLuint renderbuffer_id_;
	GLuint pbo_;
	GLuint width_;
	GLuint height_;
	// format は GL_RGBA でないと問題が出る GPU があるようなのでそれのみ。

public:
	GLFrameBufferObject() : texture_id_(0), glformat_(0), framebuffer_id_(0), renderbuffer_id_(0), pbo_(0), width_(0), height_(0) {}
	~GLFrameBufferObject() {
		destory();
	}

	// GL ハンドル (texture / framebuffer / renderbuffer / pbo) を raw GLuint で
	// 握っているため、誤コピーすると同じハンドルを二度 glDelete* してしまう。
	// 明示的に禁止する。
	GLFrameBufferObject( const GLFrameBufferObject& ) = delete;
	GLFrameBufferObject& operator=( const GLFrameBufferObject& ) = delete;
	GLFrameBufferObject( GLFrameBufferObject&& ) = delete;
	GLFrameBufferObject& operator=( GLFrameBufferObject&& ) = delete;
	bool create( GLuint w, GLuint h);
	void destory() {
		if( texture_id_ != 0 ) {
			glDeleteTextures( 1, &texture_id_ );
			texture_id_ = 0;
		}
		if (pbo_) {
			glDeleteBuffers(1, &pbo_);
			pbo_ = 0;
		}
		if( renderbuffer_id_ != 0 ) {
			glDeleteRenderbuffers( 1, &renderbuffer_id_ );
			renderbuffer_id_ = 0;
		}
		if( framebuffer_id_ != 0 ) {
			glDeleteFramebuffers( 1, &framebuffer_id_ );
			framebuffer_id_ = 0;
		}
		width_ = height_ = 0;
	}
	void bindFramebuffer();
	bool exchangeTexture( GLuint tex_id );

	GLuint textureId() const { return texture_id_; }
	GLuint width() const { return width_; }
	GLuint height() const { return height_; }
	GLint pbo() const { return pbo_; }

	tTVPTextureColorFormat format() const { return tTVPTextureColorFormat::RGBA; }
	GLint glformat() const { return glformat_; }

};


#endif