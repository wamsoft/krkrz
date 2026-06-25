#define NOMINMAX
#include "tjsCommHead.h"
#include "ThreadIntf.h"
#include "MsgIntf.h"

#include <SDL3/SDL.h>

class tTVPNativeThread : public tTVPNativeThreadIntf
{
public:
  tTVPNativeThread();
	virtual ~tTVPNativeThread();
	virtual void Start(tTVPThreadFunc func, void *arg, tTVPThreadPriority pri, const char *name);
	virtual void WaitFor();
	virtual void SetPriority(tTVPThreadPriority pri);
	virtual void SetProcessorNo(int no);
#if defined(SDL_PLATFORM_WINDOWS)
	virtual HANDLE GetHandle() const { return nullptr; } /* SDL3 doesn't provide native handle */
#endif
private:
	int calcPriority(tTVPThreadPriority pri);
	SDL_Thread* Thread;
	static int ThreadFunc(void *arg);
	static std::vector<tjs_int> processor_ids;
	void* thread_arg;
	tTVPThreadFunc thread_func;
};

std::vector<tjs_int> tTVPNativeThread::processor_ids;

tTVPNativeThread::tTVPNativeThread() 
: Thread(nullptr), thread_arg(nullptr), thread_func(nullptr)
{        
}

tTVPNativeThread::~tTVPNativeThread()
{
	if ( Thread != nullptr ) {
		SDL_WaitThread(Thread, NULL);
		Thread = nullptr;
	}
};

void
tTVPNativeThread::Start(tTVPThreadFunc func, void *arg, tTVPThreadPriority pri, const char *name)
{
	if( Thread == nullptr ) {
		thread_func = func;
		thread_arg = arg;
	    Thread = SDL_CreateThread(ThreadFunc, (name && *name) ? name : "TVPThread", this);
    	SetPriority(pri);
	}
}

void
tTVPNativeThread::WaitFor()
{
	if (Thread) { 
		SDL_WaitThread(Thread, NULL);
		Thread = nullptr;
	}    
}

int tTVPNativeThread::calcPriority(tTVPThreadPriority pri)
{
	// SDL3 doesn't provide direct thread priority control
	// Return a simplified mapping for compatibility
	switch(pri)
	{
	case ttpIdle:			return 0;
	case ttpLowest:			return 1;
	case ttpLower:			return 2;
	case ttpNormal:			return 3;
	case ttpHigher:			return 4;
	case ttpHighest:		return 5;
	case ttpTimeCritical:	return 6;
	}
	return 3; // Default to normal
}


void 
tTVPNativeThread::SetPriority(tTVPThreadPriority pri)
{
	if (Thread) {
		// SDL3 doesn't provide direct thread priority control
		// This is a placeholder for compatibility
		int npri = calcPriority(pri);
		// Note: SDL3 doesn't have SDL_SetThreadPriority, so this is effectively a no-op
	}
}

void
tTVPNativeThread::SetProcessorNo(int no)
{
	// SDL3 doesn't provide CPU affinity control
	// This is a placeholder for compatibility
}

int tTVPNativeThread::ThreadFunc(void *arg)
{
	tTVPNativeThread *self = (tTVPNativeThread*)arg;
	if (self->thread_func) {
		self->thread_func(self->thread_arg);
	}
	return 0;
}

void 
TVPYieldNativeThread(int Milliseconds)
{
	if (Milliseconds == 0) {
		SDL_Delay(0);
	} else if (Milliseconds > 0) {
		SDL_Delay(Milliseconds);
	}
}

tTVPNativeThreadIntf *
TVPCreateNativeThread()
{
    return new tTVPNativeThread();
}

tjs_int TVPGetProcessorNum( void )
{
	return SDL_GetNumLogicalCPUCores();
}
