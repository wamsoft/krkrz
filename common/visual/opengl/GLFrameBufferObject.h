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
	mutable GLuint pbo_; //!< 遅延確保するので const な pbo() からも触れるようにする
	GLuint width_;
	GLuint height_;
	mutable std::uint64_t mem_bytes_ = 0; //!< 統計に計上済みのバイト数 (PBO の遅延確保で増える)
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
	// with_pbo は互換のために残しているが、 現在はどちらでも PBO を先に確保しない。
	// PBO は pbo() が実際に呼ばれた時点で確保する (遅延確保)。
	// FBO は「カラーテクスチャ + D24S8 レンダーバッファ」だけで w*h*8 使うので、
	// 誰も使わない PBO を常時持つと 1.5 倍になってしまう。
	bool create( GLuint w, GLuint h, bool with_pbo = true );
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
		if( mem_bytes_ != 0 ) {
			GLTexture::NoteFboMemory( -(std::int64_t)mem_bytes_, -1 );
			mem_bytes_ = 0;
		}
		width_ = height_ = 0;
	}
	void bindFramebuffer();
	bool exchangeTexture( GLuint tex_id );

	GLuint textureId() const { return texture_id_; }
	GLuint width() const { return width_; }
	GLuint height() const { return height_; }

	//! 読み戻し/転送用 PBO。 呼ばれた時点で確保する (通常は誰も呼ばないので確保されない)
	GLint pbo() const { return ensurePBO(); }

	tTVPTextureColorFormat format() const { return tTVPTextureColorFormat::RGBA; }
	GLint glformat() const { return glformat_; }

private:
	//! PBO を必要になった時点で確保する
	GLuint ensurePBO() const;
};


#endif