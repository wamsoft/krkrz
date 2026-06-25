#define NOMINMAX
#include "tjsCommHead.h"
#include "ThreadIntf.h"
#include "MsgIntf.h"

#include <process.h>

class tTVPNativeThread : public tTVPNativeThreadIntf
{
public:
  tTVPNativeThread();
	virtual ~tTVPNativeThread();
	virtual void Start(tTVPThreadFunc func, void *arg, tTVPThreadPriority pri, const char *name);
	virtual void WaitFor();
	virtual void SetPriority(tTVPThreadPriority pri);
	virtual void SetProcessorNo(int no);

	virtual HANDLE GetHandle() const { return Handle; } 	/* win32 specific */

private:
	HANDLE Handle;
	tTVPThreadFunc func;
	void *arg;
	static std::vector<tjs_int> processor_ids;
	static unsigned __stdcall ThreadFunc(void *arg);
	static void ApplyThreadName(HANDLE h, const char *name);
};

std::vector<tjs_int> tTVPNativeThread::processor_ids;

unsigned __stdcall tTVPNativeThread::ThreadFunc(void *arg)
{
	tTVPNativeThread *self = (tTVPNativeThread*)arg;
	self->func(self->arg);
	return 0;
}

tTVPNativeThread::tTVPNativeThread() 
: Handle(nullptr)
{        
}

tTVPNativeThread::~tTVPNativeThread()
{
	if (Handle) {
		CloseHandle(Handle);
	}
};

void
tTVPNativeThread::Start(tTVPThreadFunc func, void *arg, tTVPThreadPriority pri, const char *name)
{
	if (!Handle) {
		this->func = func;
		this->arg  = arg;
		Handle = (HANDLE) _beginthreadex(NULL, 0, ThreadFunc, this, CREATE_SUSPENDED, NULL);
	}
	if(Handle == INVALID_HANDLE_VALUE) TVPThrowInternalError;
	SetPriority(pri);
	ApplyThreadName(Handle, name);
	::ResumeThread(Handle);
}

void
tTVPNativeThread::ApplyThreadName(HANDLE h, const char *name)
{
	// Windows 10 1607+ で利用可能な SetThreadDescription を動的解決
	if (!h || !name || !*name) return;
	typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
	static SetThreadDescriptionFn s_fn = []() -> SetThreadDescriptionFn {
		HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
		return k32 ? (SetThreadDescriptionFn)::GetProcAddress(k32, "SetThreadDescription") : nullptr;
	}();
	if (!s_fn) return;
	wchar_t wname[64];
	int n = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, (int)(sizeof(wname)/sizeof(wname[0])));
	if (n <= 0) {
		wname[0] = L'\0';
		// fallback: ASCII コピー
		size_t i = 0;
		for (; name[i] && i < (sizeof(wname)/sizeof(wname[0]) - 1); ++i) wname[i] = (wchar_t)(unsigned char)name[i];
		wname[i] = L'\0';
	}
	s_fn(h, wname);
}

void
tTVPNativeThread::WaitFor()
{
	if (Handle) {
		WaitForSingleObject(Handle, INFINITE);
	}
}

static int calcPriority(tTVPThreadPriority pri)
{
	int npri = THREAD_PRIORITY_NORMAL;
	switch(pri)
	{
	case ttpIdle:			npri = THREAD_PRIORITY_IDLE;			break;
	case ttpLowest:			npri = THREAD_PRIORITY_LOWEST;			break;
	case ttpLower:			npri = THREAD_PRIORITY_BELOW_NORMAL;	break;
	case ttpNormal:			npri = THREAD_PRIORITY_NORMAL;			break;
	case ttpHigher:			npri = THREAD_PRIORITY_ABOVE_NORMAL;	break;
	case ttpHighest:		npri = THREAD_PRIORITY_HIGHEST;			break;
	case ttpTimeCritical:	npri = THREAD_PRIORITY_TIME_CRITICAL;	break;
	}
	return npri;
}


void 
tTVPNativeThread::SetPriority(tTVPThreadPriority pri)
{
	if (Handle) {
		int npri = calcPriority(pri);
		::SetThreadPriority(Handle, npri);
	}
}

void
tTVPNativeThread::SetProcessorNo(int no)
{
	if( processor_ids.empty() ) {
#ifndef TJS_64BIT_OS
		DWORD processAffinityMask, systemAffinityMask;
		::GetProcessAffinityMask( GetCurrentProcess(), &processAffinityMask, &systemAffinityMask );
		for( tjs_int i = 0; i < MAXIMUM_PROCESSORS; i++ ) {
			if( processAffinityMask & ( 1 << i ) )
				processor_ids.push_back( i );
		}
#else
		ULONGLONG processAffinityMask, systemAffinityMask;
		::GetProcessAffinityMask( GetCurrentProcess(), (PDWORD_PTR)&processAffinityMask, (PDWORD_PTR)&systemAffinityMask );
		for( tjs_int i = 0; i < MAXIMUM_PROCESSORS; i++ ) {
			if( processAffinityMask & ( 1ULL << i ) )
				processor_ids.push_back( i );
		}
#endif
		if( processor_ids.empty() )
			processor_ids.push_back( MAXIMUM_PROCESSORS );
	}
	if (Handle) {
		::SetThreadIdealProcessor( Handle, processor_ids[no % processor_ids.size()] );
	}
}

void 
TVPYieldNativeThread(int Milliseconds)
{
	::SleepEx(Milliseconds,0);
}

tTVPNativeThreadIntf *
TVPCreateNativeThread()
{
    return new tTVPNativeThread();
}

tjs_int 
TVPGetProcessorNum( void )
{
	return std::thread::hardware_concurrency();
}
