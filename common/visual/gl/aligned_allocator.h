

#ifndef __ALIGNED_ALLOCATOR_H__
#define __ALIGNED_ALLOCATOR_H__

#ifdef _WIN32
#include <intrin.h>
#else
#include <stdlib.h>
#endif
#include <memory>		// std::allocator
#include <new>			// std::bad_alloc


// STL allocator
template< class T, int TAlign=16 >
struct aligned_allocator : public std::allocator<T>
{
	static const int ALIGN_SIZE = TAlign;
	template <class U> struct rebind    { typedef aligned_allocator<U,TAlign> other; };
	aligned_allocator() throw() {}
	aligned_allocator(const aligned_allocator&) throw () {}
	template <class U> aligned_allocator(const aligned_allocator<U, TAlign>&) throw() {}
	template <class U> aligned_allocator& operator=(const aligned_allocator<U, TAlign>&) throw()  {}
	// allocate
#ifdef _WIN32
	T* allocate(std::size_t c, const void* /*hint*/ = 0) {
		return static_cast<T*>( _mm_malloc( sizeof(T)*c, TAlign ) );
	}
#else
	T* allocate( std::size_t c ) {
		// posix_memalign は alignment が sizeof(void*) 以上の 2 冪でないと EINVAL を
		// 返し、出力ポインタは未設定のまま残る。TAlign=4 (AxisParam<> 既定) のとき
		// 未初期化ポインタをそのまま返してヒープ破壊するバグがあった
		// (NX/PS5 の TVPResampleImage クラッシュの原因)。alignment を下駄上げし、
		// 失敗は bad_alloc にする。
		std::size_t align = TAlign < sizeof(void*) ? sizeof(void*) : TAlign;
		void* ret = nullptr;
		if( posix_memalign( &ret, align, sizeof(T)*c ) != 0 ) throw std::bad_alloc();
		return static_cast<T*>( ret );
	}
#endif
	// deallocate
#ifdef _WIN32
	void deallocate(T* p, std::size_t /*n*/) {
		_mm_free( p );
	}
#else
	void deallocate( T* p, std::size_t n ) {
		free( p );
	}
#endif
};


#endif // __ALIGNED_ALLOCATOR_H__


