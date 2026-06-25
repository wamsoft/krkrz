
#ifndef __GL_VERTEX_BUFFER_OBJECT_H__
#define __GL_VERTEX_BUFFER_OBJECT_H__

#include "OpenGLHeader.h"


class GLVertexBufferObject {
	GLuint vbo_id_;
	GLenum target_;
	GLenum usage_;
	tjs_int size_;

public:
	GLVertexBufferObject() : vbo_id_(0), target_(0), usage_(GL_STATIC_DRAW), size_(0) {}

	~GLVertexBufferObject() {
		destory();
	}

	// GL ハンドルを raw GLuint で握っているため、誤コピー / ムーブで
	// vbo_id_ が共有されると ~GLVertexBufferObject で同じ ID を二度
	// glDeleteBuffers してしまう。明示的に禁止する。
	GLVertexBufferObject( const GLVertexBufferObject& ) = delete;
	GLVertexBufferObject& operator=( const GLVertexBufferObject& ) = delete;
	GLVertexBufferObject( GLVertexBufferObject&& ) = delete;
	GLVertexBufferObject& operator=( GLVertexBufferObject&& ) = delete;
	void destory() {
		if( vbo_id_ ) {
			glDeleteBuffers( 1, &vbo_id_ );
			vbo_id_ = 0;
		}
	}

	void createStaticVertex( const GLfloat* vtx, tjs_int byteSize ) {
		destory();

		glGenBuffers( 1, &vbo_id_ );
		glBindBuffer( GL_ARRAY_BUFFER, vbo_id_ );
		glBufferData( GL_ARRAY_BUFFER, byteSize, (const void *)vtx, GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		target_ = GL_ARRAY_BUFFER;
		usage_ = GL_STATIC_DRAW;
		size_ = byteSize;
	}
	void createVertexBuffer( tjs_int byteSize, GLenum usage, bool isIndex = false, const void* data = nullptr ) {
		destory();
		if( isIndex ) {
			target_ = GL_ELEMENT_ARRAY_BUFFER;
		} else {
			target_ = GL_ARRAY_BUFFER;
		}

		glGenBuffers( 1, &vbo_id_ );
		glBindBuffer( target_, vbo_id_ );
		glBufferData( target_, byteSize, data, usage );
		glBindBuffer( target_, 0 );
		usage_ = usage;
		size_ = byteSize;
	}
	void copyBuffer( GLintptr offset, GLsizeiptr size,  const GLvoid * data) {
		if( vbo_id_ ) {
			glBindBuffer( target_, vbo_id_ );
			glBufferSubData( target_, offset, size, data );
			glBindBuffer( target_, 0 );
		}
	}

	void bindBuffer() const {
		if( vbo_id_ ) {
			glBindBuffer( target_, vbo_id_ );
		}
	}
	void unbindBuffer() const {
		glBindBuffer( target_, 0 );
	}
	void* mapBuffer() {
		void* result = nullptr;
		if( vbo_id_ ) {
			glBindBuffer( target_, vbo_id_ );
			result = glMapBufferRange( target_, 0, size_, GL_MAP_READ_BIT|GL_MAP_WRITE_BIT );
			// マップ中もこの VBO を target_ にバインドしたままにする。
			// 旧実装は即 unbind していたが、glUnmapBuffer は currently bound
			// buffer を対象に動くため、unmap 時に再度 bind しないと
			// GL_INVALID_OPERATION で unmap が効かず buffer がマップされたまま残る。
		}
		return result;
	}
	void unmapBuffer() {
		if( vbo_id_ ) {
			// mapBuffer 後に他のコードが target_ を上書きしていても安全なように
			// ここでもう一度 bind しなおしてから unmap する。
			glBindBuffer( target_, vbo_id_ );
			glUnmapBuffer( target_ );
			glBindBuffer( target_, 0 );
		}
	}
	bool isCreated() const { return vbo_id_ != 0; }

	GLuint id() const { return vbo_id_; }
	GLenum usage() const { return usage_; }
	GLenum target() const { return target_; }
	tjs_int size() const { return  size_; }

	bool isIndex() const { return target_ == GL_ELEMENT_ARRAY_BUFFER; }
};


#endif
