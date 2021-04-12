// __USE_UNIX98 is needed for sttype / gettype definition
#define __USE_UNIX98
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/mman.h>

#include "debug.h"
#include "box64context.h"
#include "threads.h"
#include "emu/x64emu_private.h"
#include "tools/bridge_private.h"
#include "x64run.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "custommem.h"
#include "khash.h"
#include "emu/x64run_private.h"
#include "x64trace.h"
#include "dynarec.h"
#include "bridge.h"
#ifdef DYNAREC
#include "dynablock.h"
#endif

void _pthread_cleanup_push_defer(void* buffer, void* routine, void* arg);	// declare hidden functions
void _pthread_cleanup_pop_restore(void* buffer, int exec);
void _pthread_cleanup_push(void* buffer, void* routine, void* arg);	// declare hidden functions
void _pthread_cleanup_pop(void* buffer, int exec);

typedef struct threadstack_s {
	void* 	stack;
	size_t 	stacksize;
} threadstack_t;

// longjmp / setjmp
typedef struct jump_buff_x64_s {
	uint64_t save_reg[8];
} jump_buff_x64_t;

typedef struct __jmp_buf_tag_s {
    jump_buff_x64_t  __jmpbuf;
    int              __mask_was_saved;
    __sigset_t       __saved_mask;
} __jmp_buf_tag_t;

typedef struct x64_unwind_buff_s {
	struct {
		jump_buff_x64_t		__cancel_jmp_buf;	
		int					__mask_was_saved;
	} __cancel_jmp_buf[1];
	void *__pad[4];
} x64_unwind_buff_t __attribute__((__aligned__));

typedef void(*vFv_t)();

KHASH_MAP_INIT_INT64(threadstack, threadstack_t*)
KHASH_MAP_INIT_INT64(cancelthread, __pthread_unwind_buf_t*)

void CleanStackSize(box64context_t* context)
{
	threadstack_t *ts;
	if(!context || !context->stacksizes)
		return;
	pthread_mutex_lock(&context->mutex_thread);
	kh_foreach_value(context->stacksizes, ts, free(ts));
	kh_destroy(threadstack, context->stacksizes);
	context->stacksizes = NULL;
	pthread_mutex_unlock(&context->mutex_thread);
}

void FreeStackSize(kh_threadstack_t* map, uintptr_t attr)
{
	pthread_mutex_lock(&my_context->mutex_thread);
	khint_t k = kh_get(threadstack, map, attr);
	if(k!=kh_end(map)) {
		free(kh_value(map, k));
		kh_del(threadstack, map, k);
	}
	pthread_mutex_unlock(&my_context->mutex_thread);
}

void AddStackSize(kh_threadstack_t* map, uintptr_t attr, void* stack, size_t stacksize)
{
	khint_t k;
	int ret;
	pthread_mutex_lock(&my_context->mutex_thread);
	k = kh_put(threadstack, map, attr, &ret);
	threadstack_t* ts = kh_value(map, k) = (threadstack_t*)calloc(1, sizeof(threadstack_t));
	ts->stack = stack;
	ts->stacksize = stacksize;
	pthread_mutex_unlock(&my_context->mutex_thread);
}

// return stack from attr (or from current emu if attr is not found..., wich is wrong but approximate enough?)
int GetStackSize(x64emu_t* emu, uintptr_t attr, void** stack, size_t* stacksize)
{
	if(emu->context->stacksizes && attr) {
		pthread_mutex_lock(&my_context->mutex_thread);
		khint_t k = kh_get(threadstack, emu->context->stacksizes, attr);
		if(k!=kh_end(emu->context->stacksizes)) {
			threadstack_t* ts = kh_value(emu->context->stacksizes, k);
			*stack = ts->stack;
			*stacksize = ts->stacksize;
			pthread_mutex_unlock(&my_context->mutex_thread);
			return 1;
		}
		pthread_mutex_unlock(&my_context->mutex_thread);
	}
	// should a Warning be emited?
	*stack = emu->init_stack;
	*stacksize = emu->size_stack;
	return 0;
}

static void InitCancelThread()
{
}

static void FreeCancelThread(box64context_t* context)
{
	if(!context)
		return;
}
static __pthread_unwind_buf_t* AddCancelThread(x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t* r = (__pthread_unwind_buf_t*)calloc(1, sizeof(__pthread_unwind_buf_t));
	buff->__pad[3] = r;
	return r;
}

static __pthread_unwind_buf_t* GetCancelThread(x64_unwind_buff_t* buff)
{
	return (__pthread_unwind_buf_t*)buff->__pad[3];
}

static void DelCancelThread(x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t* r = (__pthread_unwind_buf_t*)buff->__pad[3];
	free(r);
	buff->__pad[3] = NULL;
}

typedef struct emuthread_s {
	uintptr_t 	fnc;
	void*		arg;
	x64emu_t*	emu;
} emuthread_t;

static void emuthread_destroy(void* p)
{
	emuthread_t *et = (emuthread_t*)p;
	if(et) {
		FreeX64Emu(&et->emu);
		free(et);
	}
}

static pthread_key_t thread_key;
static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;

static void thread_key_alloc() {
	pthread_key_create(&thread_key, emuthread_destroy);
}

void thread_set_emu(x64emu_t* emu)
{
	// create the key
	pthread_once(&thread_key_once, thread_key_alloc);
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(!et) {
		et = (emuthread_t*)calloc(1, sizeof(emuthread_t));
	} else {
		if(et->emu != emu)
			FreeX64Emu(&et->emu);
	}
	et->emu = emu;
	et->emu->type = EMUTYPE_MAIN;
	pthread_setspecific(thread_key, et);
}

x64emu_t* thread_get_emu()
{
	// create the key
	pthread_once(&thread_key_once, thread_key_alloc);
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(!et) {
		int stacksize = 2*1024*1024;
		// try to get stack size of the thread
		pthread_attr_t attr;
		if(!pthread_getattr_np(pthread_self(), &attr)) {
			size_t stack_size;
        	void *stack_addr;
			if(!pthread_attr_getstack(&attr, &stack_addr, &stack_size))
				stacksize = stack_size;
			pthread_attr_destroy(&attr);
		}
		void* stack = mmap(NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
		x64emu_t *emu = NewX64Emu(my_context, 0, (uintptr_t)stack, stacksize, 1);
		SetupX64Emu(emu);
		thread_set_emu(emu);
		return emu;
	}
	return et->emu;
}

static void* pthread_routine(void* p)
{
	// create the key
	pthread_once(&thread_key_once, thread_key_alloc);
	// free current emuthread if it exist
	{
		void* t = pthread_getspecific(thread_key);
		if(t) {
			// not sure how this could happens
			printf_log(LOG_INFO, "Clean of an existing ET for Thread %04d\n", GetTID());
			emuthread_destroy(t);
		}
	}
	pthread_setspecific(thread_key, p);
	// call the function
	emuthread_t *et = (emuthread_t*)p;
	et->emu->type = EMUTYPE_MAIN;
	// setup callstack and run...
	x64emu_t* emu = et->emu;
	Push64(emu, 0);		// PUSH BP
	R_RBP = R_RSP;	// MOV BP, SP
	R_RSP -= 56;	// Gard zone
	PushExit(emu);
	R_RIP = et->fnc;
	R_RDI = (uintptr_t)et->arg;
	DynaRun(emu);
	void* ret = (void*)R_RAX;
	//void* ret = (void*)RunFunctionWithEmu(et->emu, 0, et->fnc, 1, et->arg);
	return ret;
}

#ifdef NOALIGN
pthread_attr_t* getAlignedAttr(pthread_attr_t* m) {
	return m;
}
void freeAlignedAttr(void* attr) {
}
#else
typedef struct aligned_attr_s {
	uint64_t sign;
	pthread_attr_t *at;
} aligned_attr_t;
#define SIGN_ATTR *(uint64_t*)"BOX64ATT"

pthread_attr_t* getAlignedAttrWithInit(pthread_attr_t* attr, int init)
{
	if(!attr)
		return attr;
	aligned_attr_t* at = (aligned_attr_t*)attr;
	if(init && at->sign==SIGN_ATTR)
		return at->at;
	pthread_attr_t* ret = (pthread_attr_t*)calloc(1, sizeof(pthread_attr_t));
	at->sign = SIGN_ATTR;
	at->at = ret;
	if(init)
		pthread_attr_init(ret);	// init?
	return ret;
}
pthread_attr_t* getAlignedAttr(pthread_attr_t* attr)
{
	return getAlignedAttrWithInit(attr, 1);
}
void freeAlignedAttr(void* attr)
{
	if(!attr)
		return;
	aligned_attr_t* at = (aligned_attr_t*)attr;
	if(at->sign==SIGN_ATTR) {
		free(at->at);
		at->sign = 0LL;
	}
}
#endif

EXPORT int my_pthread_attr_destroy(x64emu_t* emu, void* attr)
{
	if(emu->context->stacksizes)
		FreeStackSize(emu->context->stacksizes, (uintptr_t)attr);
	int ret = pthread_attr_destroy(getAlignedAttr(attr));
	freeAlignedAttr(attr);
	return ret;
}

EXPORT int my_pthread_attr_getstack(x64emu_t* emu, void* attr, void** stackaddr, size_t* stacksize)
{
	int ret = pthread_attr_getstack(getAlignedAttr(attr), stackaddr, stacksize);
	if (ret==0)
		GetStackSize(emu, (uintptr_t)attr, stackaddr, stacksize);
	return ret;
}

EXPORT int my_pthread_attr_setstack(x64emu_t* emu, void* attr, void* stackaddr, size_t stacksize)
{
	if(!emu->context->stacksizes) {
		emu->context->stacksizes = kh_init(threadstack);
	}
	AddStackSize(emu->context->stacksizes, (uintptr_t)attr, stackaddr, stacksize);
	//Don't call actual setstack...
	//return pthread_attr_setstack(attr, stackaddr, stacksize);
	return pthread_attr_setstacksize(getAlignedAttr(attr), stacksize);
}

EXPORT int my_pthread_attr_setstacksize(x64emu_t* emu, void* attr, size_t stacksize)
{
	//aarch64 have an PTHREAD_STACK_MIN of 131072 instead of 16384 on x86_64!
	if(stacksize<PTHREAD_STACK_MIN)
		stacksize = PTHREAD_STACK_MIN;
	return pthread_attr_setstacksize(getAlignedAttr(attr), stacksize);
}

#ifndef NOALIGN
EXPORT int my_pthread_attr_getdetachstate(x64emu_t* emu, pthread_attr_t* attr, int *state)
{
	return pthread_attr_getdetachstate(getAlignedAttr(attr), state);
}
EXPORT int my_pthread_attr_getguardsize(x64emu_t* emu, pthread_attr_t* attr, size_t* size)
{
	return pthread_attr_getguardsize(getAlignedAttr(attr), size);
}
EXPORT int my_pthread_attr_getinheritsched(x64emu_t* emu, pthread_attr_t* attr, int* sched)
{
	return pthread_attr_getinheritsched(getAlignedAttr(attr), sched);
}
EXPORT int my_pthread_attr_getschedparam(x64emu_t* emu, pthread_attr_t* attr, void* param)
{
	return pthread_attr_getschedparam(getAlignedAttr(attr), param);
}
EXPORT int my_pthread_attr_getschedpolicy(x64emu_t* emu, pthread_attr_t* attr, int* policy)
{
	return pthread_attr_getschedpolicy(getAlignedAttr(attr), policy);
}
EXPORT int my_pthread_attr_getscope(x64emu_t* emu, pthread_attr_t* attr, int* scope)
{
	return pthread_attr_getscope(getAlignedAttr(attr), scope);
}
EXPORT int my_pthread_attr_getstackaddr(x64emu_t* emu, pthread_attr_t* attr, void* addr)
{
	size_t size;
	return pthread_attr_getstack(getAlignedAttr(attr), addr, &size);
	//return pthread_attr_getstackaddr(getAlignedAttr(attr), addr);
}
EXPORT int my_pthread_attr_getstacksize(x64emu_t* emu, pthread_attr_t* attr, size_t* size)
{
	void* addr;
	return pthread_attr_getstack(getAlignedAttr(attr), &addr, size);
	//return pthread_attr_getstacksize(getAlignedAttr(attr), size);
}
EXPORT int my_pthread_attr_init(x64emu_t* emu, pthread_attr_t* attr)
{
	return pthread_attr_init(getAlignedAttrWithInit(attr, 0));
}
EXPORT int my_pthread_attr_setaffinity_np(x64emu_t* emu, pthread_attr_t* attr, size_t cpusize, void* cpuset)
{
	return pthread_attr_setaffinity_np(getAlignedAttr(attr), cpusize, cpuset);
}
EXPORT int my_pthread_attr_setdetachstate(x64emu_t* emu, pthread_attr_t* attr, int state)
{
	return pthread_attr_setdetachstate(getAlignedAttr(attr), state);
}
EXPORT int my_pthread_attr_setguardsize(x64emu_t* emu, pthread_attr_t* attr, size_t size)
{
	return pthread_attr_setguardsize(getAlignedAttr(attr), size);
}
EXPORT int my_pthread_attr_setinheritsched(x64emu_t* emu, pthread_attr_t* attr, int sched)
{
	return pthread_attr_setinheritsched(getAlignedAttr(attr), sched);
}
EXPORT int my_pthread_attr_setschedparam(x64emu_t* emu, pthread_attr_t* attr, void* param)
{
	return pthread_attr_setschedparam(getAlignedAttr(attr), param);
}
EXPORT int my_pthread_attr_setschedpolicy(x64emu_t* emu, pthread_attr_t* attr, int policy)
{
	return pthread_attr_setschedpolicy(getAlignedAttr(attr), policy);
}
EXPORT int my_pthread_attr_setscope(x64emu_t* emu, pthread_attr_t* attr, int scope)
{
	return pthread_attr_setscope(getAlignedAttr(attr), scope);
}
EXPORT int my_pthread_attr_setstackaddr(x64emu_t* emu, pthread_attr_t* attr, void* addr)
{
	size_t size = 2*1024*1024;
	my_pthread_attr_getstacksize(emu, attr, &size);
	return pthread_attr_setstack(getAlignedAttr(attr), addr, size);
	//return pthread_attr_setstackaddr(getAlignedAttr(attr), addr);
}
#endif

EXPORT int my_pthread_create(x64emu_t *emu, void* t, void* attr, void* start_routine, void* arg)
{
	int stacksize = 2*1024*1024;	//default stack size is 2Mo
	void* attr_stack;
	size_t attr_stacksize;
	int own;
	void* stack;

	if(attr) {
		size_t stsize;
		if(pthread_attr_getstacksize(getAlignedAttr(attr), &stsize)==0)
			stacksize = stsize;
	}
	if(GetStackSize(emu, (uintptr_t)attr, &attr_stack, &attr_stacksize))
	{
		stack = attr_stack;
		stacksize = attr_stacksize;
		own = 0;
	} else {
		stack = mmap(NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
		own = 1;
	}

	emuthread_t *et = (emuthread_t*)calloc(1, sizeof(emuthread_t));
    x64emu_t *emuthread = NewX64Emu(my_context, (uintptr_t)start_routine, (uintptr_t)stack, stacksize, own);
	SetupX64Emu(emuthread);
	//SetFS(emuthread, GetFS(emu));
	et->emu = emuthread;
	et->fnc = (uintptr_t)start_routine;
	et->arg = arg;
	#ifdef DYNAREC
	if(box64_dynarec) {
		// pre-creation of the JIT code for the entry point of the thread
		dynablock_t *current = NULL;
		DBGetBlock(emu, (uintptr_t)start_routine, 1, &current);
	}
	#endif
	// create thread
	return pthread_create((pthread_t*)t, getAlignedAttr(attr), 
		pthread_routine, et);
}

void* my_prepare_thread(x64emu_t *emu, void* f, void* arg, int ssize, void** pet)
{
	int stacksize = (ssize)?ssize:(2*1024*1024);	//default stack size is 2Mo
	void* stack = mmap(NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
	emuthread_t *et = (emuthread_t*)calloc(1, sizeof(emuthread_t));
    x64emu_t *emuthread = NewX64Emu(emu->context, (uintptr_t)f, (uintptr_t)stack, stacksize, 1);
	SetupX64Emu(emuthread);
	SetFS(emuthread, GetFS(emu));
	et->emu = emuthread;
	et->fnc = (uintptr_t)f;
	et->arg = arg;
	#ifdef DYNAREC
	// pre-creation of the JIT code for the entry point of the thread
	dynablock_t *current = NULL;
	DBGetBlock(emu, (uintptr_t)f, 1, &current);
	#endif
	*pet =  et;
	return pthread_routine;
}

void my_longjmp(x64emu_t* emu, /*struct __jmp_buf_tag __env[1]*/void *p, int32_t __val);

#define CANCEL_MAX 8
static __thread x64emu_t* cancel_emu[CANCEL_MAX] = {0};
static __thread x64_unwind_buff_t* cancel_buff[CANCEL_MAX] = {0};
static __thread int cancel_deep = 0;
EXPORT void my___pthread_register_cancel(void* E, void* B)
{
	// get a stack local copy of the args, as may be live in some register depending the architecture (like ARM)
	if(cancel_deep<0) {
		printf_log(LOG_NONE/*LOG_INFO*/, "BOX64: Warning, inconsistant value in __pthread_register_cancel (%d)\n", cancel_deep);
		cancel_deep = 0;
	}
	if(cancel_deep!=CANCEL_MAX-1) 
		++cancel_deep;
	else
		{printf_log(LOG_NONE/*LOG_INFO*/, "BOX64: Warning, calling __pthread_register_cancel(...) too many time\n");}
		
	cancel_emu[cancel_deep] = (x64emu_t*)E;

	x64_unwind_buff_t* buff = cancel_buff[cancel_deep] = (x64_unwind_buff_t*)B;
	__pthread_unwind_buf_t * pbuff = AddCancelThread(buff);
	if(__sigsetjmp((struct __jmp_buf_tag*)(void*)pbuff->__cancel_jmp_buf, 0)) {
		//DelCancelThread(cancel_buff);	// no del here, it will be delete by unwind_next...
		int i = cancel_deep--;
		x64emu_t* emu = cancel_emu[i];
		my_longjmp(emu, cancel_buff[i]->__cancel_jmp_buf, 1);
		DynaRun(emu);	// resume execution // TODO: Use ejb instead?
		return;
	}

	__pthread_register_cancel(pbuff);
}

EXPORT void my___pthread_unregister_cancel(x64emu_t* emu, x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t * pbuff = GetCancelThread(buff);
	__pthread_unregister_cancel(pbuff);

	--cancel_deep;
	DelCancelThread(buff);
}

EXPORT void my___pthread_unwind_next(x64emu_t* emu, x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t pbuff = *GetCancelThread(buff);
	DelCancelThread(buff);
	// function is noreturn, putting stuff on the stack to have it auto-free (is that correct?)
	__pthread_unwind_next(&pbuff);
	// just in case it does return
	emu->quit = 1;
}

KHASH_MAP_INIT_INT(once, int)

#define SUPER() \
GO(0)			\
GO(1)			\
GO(2)			\
GO(3)			\
GO(4)			\
GO(5)			\
GO(6)			\
GO(7)			\
GO(8)			\
GO(9)			\
GO(10)			\
GO(11)			\
GO(12)			\
GO(13)			\
GO(14)			\
GO(15)			\
GO(16)			\
GO(17)			\
GO(18)			\
GO(19)			\
GO(20)			\
GO(21)			\
GO(22)			\
GO(23)			\
GO(24)			\
GO(25)			\
GO(26)			\
GO(27)			\
GO(28)			\
GO(29)			

// key_destructor
#define GO(A)   \
static uintptr_t my_key_destructor_fct_##A = 0;  \
static void my_key_destructor_##A(void* a)    			\
{                                       		\
    RunFunction(my_context, my_key_destructor_fct_##A, 1, a);\
}
SUPER()
#undef GO
static void* findkey_destructorFct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_key_destructor_fct_##A == (uintptr_t)fct) return my_key_destructor_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_key_destructor_fct_##A == 0) {my_key_destructor_fct_##A = (uintptr_t)fct; return my_key_destructor_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for pthread key_destructor callback\n");
    return NULL;
}
// cleanup_routine
#define GO(A)   \
static uintptr_t my_cleanup_routine_fct_##A = 0;  \
static void my_cleanup_routine_##A(void* a)    			\
{                                       		\
    RunFunction(my_context, my_cleanup_routine_fct_##A, 1, a);\
}
SUPER()
#undef GO
static void* findcleanup_routineFct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_cleanup_routine_fct_##A == (uintptr_t)fct) return my_cleanup_routine_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_cleanup_routine_fct_##A == 0) {my_cleanup_routine_fct_##A = (uintptr_t)fct; return my_cleanup_routine_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for pthread cleanup_routine callback\n");
    return NULL;
}

#undef SUPER

// once_callback
// Don't use a "GO" scheme, once callback are only called once by definition
static __thread uintptr_t my_once_callback_fct = 0;
static void my_once_callback()
{
	if(my_once_callback_fct) {
	    if(GetNativeFnc((uintptr_t)my_once_callback_fct))  {
			vFv_t f = (vFv_t)GetNativeFnc((uintptr_t)my_once_callback_fct);
			f();
			return;
		}
    	RunFunction(my_context, my_once_callback_fct, 0, 0);
	}
}

int EXPORT my_pthread_once(x64emu_t* emu, void* once, void* cb)
{
	my_once_callback_fct = (uintptr_t)cb;
	return pthread_once(once, my_once_callback);
}
EXPORT int my___pthread_once(x64emu_t* emu, void* once, void* cb) __attribute__((alias("my_pthread_once")));

EXPORT int my_pthread_key_create(x64emu_t* emu, void* key, void* dtor)
{
	return pthread_key_create(key, findkey_destructorFct(dtor));
}
EXPORT int my___pthread_key_create(x64emu_t* emu, void* key, void* dtor) __attribute__((alias("my_pthread_key_create")));

pthread_mutex_t* getAlignedMutex(pthread_mutex_t* m);


EXPORT int my_pthread_cond_timedwait(x64emu_t* emu, pthread_cond_t* cond, void* mutex, void* abstime)
{
	return pthread_cond_timedwait(cond, getAlignedMutex((pthread_mutex_t*)mutex), (const struct timespec*)abstime);
}
EXPORT int my_pthread_cond_wait(x64emu_t* emu, pthread_cond_t* cond, void* mutex)
{
	return pthread_cond_wait(cond, getAlignedMutex((pthread_mutex_t*)mutex));
}

//EXPORT int my_pthread_attr_setscope(x64emu_t* emu, void* attr, int scope)
//{
//    if(scope!=PTHREAD_SCOPE_SYSTEM) printf_log(LOG_INFO, "Warning, scope of call to pthread_attr_setscope(...) changed from %d to PTHREAD_SCOPE_SYSTEM\n", scope);
//	return pthread_attr_setscope(attr, PTHREAD_SCOPE_SYSTEM);
//    //The scope is either PTHREAD_SCOPE_SYSTEM or PTHREAD_SCOPE_PROCESS
//    // but PTHREAD_SCOPE_PROCESS doesn't seem supported on ARM linux, and PTHREAD_SCOPE_SYSTEM is default
//}

EXPORT void my__pthread_cleanup_push_defer(x64emu_t* emu, void* buffer, void* routine, void* arg)
{
	_pthread_cleanup_push_defer(buffer, findcleanup_routineFct(routine), arg);
}

EXPORT void my__pthread_cleanup_push(x64emu_t* emu, void* buffer, void* routine, void* arg)
{
	_pthread_cleanup_push(buffer, findcleanup_routineFct(routine), arg);
}

EXPORT void my__pthread_cleanup_pop_restore(x64emu_t* emu, void* buffer, int exec)
{
	_pthread_cleanup_pop_restore(buffer, exec);
}

EXPORT void my__pthread_cleanup_pop(x64emu_t* emu, void* buffer, int exec)
{
	_pthread_cleanup_pop(buffer, exec);
}

//EXPORT int my_pthread_getaffinity_np(x64emu_t* emu, pthread_t thread, int cpusetsize, void* cpuset)
//{
//	int ret = pthread_getaffinity_np(thread, cpusetsize, cpuset);
//	if(ret<0) {
//		printf_log(LOG_INFO, "Warning, pthread_getaffinity_np(%p, %d, %p) errored, with errno=%d\n", (void*)thread, cpusetsize, cpuset, errno);
//	}
//
//    return ret;
//}

//EXPORT int my_pthread_setaffinity_np(x64emu_t* emu, pthread_t thread, int cpusetsize, void* cpuset)
//{
//	int ret = pthread_setaffinity_np(thread, cpusetsize, cpuset);
//	if(ret<0) {
//		printf_log(LOG_INFO, "Warning, pthread_setaffinity_np(%p, %d, %p) errored, with errno=%d\n", (void*)thread, cpusetsize, cpuset, errno);
//	}
//
//    return ret;
//}

//EXPORT int my_pthread_attr_setaffinity_np(x64emu_t* emu, void* attr, uint32_t cpusetsize, void* cpuset)
//{
//
//	int ret = pthread_attr_setaffinity_np(attr, cpusetsize, cpuset);
//	if(ret<0) {
//		printf_log(LOG_INFO, "Warning, pthread_attr_setaffinity_np(%p, %d, %p) errored, with errno=%d\n", attr, cpusetsize, cpuset, errno);
//	}
//
//    return ret;
//}

EXPORT int my_pthread_kill(x64emu_t* emu, void* thread, int sig)
{
    // check for old "is everything ok?"
    if(thread==NULL && sig==0)
        return pthread_kill(pthread_self(), 0);
    return pthread_kill((pthread_t)thread, sig);
}

//EXPORT void my_pthread_exit(x64emu_t* emu, void* retval)
//{
//	pthread_exit(retval);
//}

#ifdef NOALIGN
pthread_mutex_t* getAlignedMutex(pthread_mutex_t* m) {
	return m;
}
#else
//#define TRACK_MUTEX
// mutex alignment
#ifdef TRACK_MUTEX
KHASH_MAP_INIT_INT(mutex, pthread_mutex_t*)

static kh_mutex_t* unaligned_mutex = NULL;
#endif

typedef struct aligned_mutex_s {
	uint64_t sign;
	pthread_mutex_t *m;
} aligned_mutex_t;
#define SIGN *(uint64_t*)"BOX64MTX"

pthread_mutex_t* getAlignedMutexWithInit(pthread_mutex_t* m, int init)
{
	aligned_mutex_t* am = (aligned_mutex_t*)m;
	if(init && am->sign==SIGN)
		return am->m;
	#ifdef TRACK_MUTEX
	khint_t k = kh_get(mutex, unaligned_mutex, (uintptr_t)m);
	if(k!=kh_end(unaligned_mutex))
		return kh_value(unaligned_mutex, k);
	int r;
	k = kh_put(mutex, unaligned_mutex, (uintptr_t)m, &r);
	pthread_mutex_t* ret = kh_value(unaligned_mutex, k) = (pthread_mutex_t*)calloc(1, sizeof(pthread_mutex_t));
	#else
	pthread_mutex_t* ret = (pthread_mutex_t*)calloc(1, sizeof(pthread_mutex_t));
	#endif
	am->sign = SIGN;
	am->m = ret;
	if(init)
		pthread_mutex_init(ret, NULL);	// default init, same as with static constructor
	return ret;
}
pthread_mutex_t* getAlignedMutex(pthread_mutex_t* m)
{
	return getAlignedMutexWithInit(m, 1);
}

EXPORT int my_pthread_mutex_destroy(pthread_mutex_t *m)
{
	aligned_mutex_t* am = (aligned_mutex_t*)m;
	#ifdef TRACK_MUTEX
	if(am->sign==SIGN) {
		am->sign = 0;
		am->m = NULL;
	}
	khint_t k = kh_get(mutex, unaligned_mutex, (uintptr_t)m);
	if(k!=kh_end(unaligned_mutex)) {
		pthread_mutex_t *n = kh_value(unaligned_mutex, k);
		kh_del(mutex, unaligned_mutex, k);
		int ret = pthread_mutex_destroy(n);
		free(n);
		return ret;
	}
	return pthread_mutex_destroy(m);
	#else
	if(am->sign!=SIGN) {
		return 1;	//???
	}
	int ret = pthread_mutex_destroy(am->m);
	free(am->m);
	return ret;
	#endif
}
typedef union my_mutexattr_s {
	int					x86;
	pthread_mutexattr_t nat;
} my_mutexattr_t;
// mutexattr
EXPORT int my_pthread_mutexattr_destroy(x64emu_t* emu, my_mutexattr_t *attr)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_destroy(&mattr.nat);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_destroy(x64emu_t* emu, my_mutexattr_t *attr) __attribute__((alias("my_pthread_mutexattr_destroy")));
EXPORT int my_pthread_mutexattr_getkind_np(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	//int ret = pthread_mutexattr_getkind_np(&mattr.nat, p);
	int ret = pthread_mutexattr_gettype(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_gettype(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_gettype(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_init(x64emu_t* emu, my_mutexattr_t *attr)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_init(&mattr.nat);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_init(x64emu_t* emu, my_mutexattr_t *attr) __attribute__((alias("my_pthread_mutexattr_init")));
EXPORT int my_pthread_mutexattr_setkind_np(x64emu_t* emu, my_mutexattr_t *attr, int k)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	//int ret = pthread_mutexattr_setkind_np(&mattr.nat, k);
	int ret = pthread_mutexattr_settype(&mattr.nat, k);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_setprotocol(x64emu_t* emu, my_mutexattr_t *attr, int p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_setprotocol(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_setpshared(x64emu_t* emu, my_mutexattr_t *attr, int p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_setpshared(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_settype(x64emu_t* emu, my_mutexattr_t *attr, int t)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_settype(&mattr.nat, t);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_settype(x64emu_t* emu, my_mutexattr_t *attr, int t) __attribute__((alias("my_pthread_mutexattr_settype")));

// mutex
int my___pthread_mutex_destroy(pthread_mutex_t *m) __attribute__((alias("my_pthread_mutex_destroy")));

EXPORT int my_pthread_mutex_init(pthread_mutex_t *m, my_mutexattr_t *att)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = att->x86;
	return pthread_mutex_init(getAlignedMutexWithInit(m, 0), &mattr.nat);
}
EXPORT int my___pthread_mutex_init(pthread_mutex_t *m, my_mutexattr_t *att) __attribute__((alias("my_pthread_mutex_init")));

EXPORT int my_pthread_mutex_lock(pthread_mutex_t *m)
{
	return pthread_mutex_lock(getAlignedMutex(m));
}
EXPORT int my___pthread_mutex_lock(pthread_mutex_t *m) __attribute__((alias("my_pthread_mutex_lock")));

EXPORT int my_pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec * t)
{
	return pthread_mutex_timedlock(getAlignedMutex(m), t);
}
EXPORT int my___pthread_mutex_trylock(pthread_mutex_t *m, const struct timespec * t) __attribute__((alias("my_pthread_mutex_timedlock")));

EXPORT int my_pthread_mutex_trylock(pthread_mutex_t *m)
{
	return pthread_mutex_trylock(getAlignedMutex(m));
}
EXPORT int my___pthread_mutex_unlock(pthread_mutex_t *m) __attribute__((alias("my_pthread_mutex_trylock")));

EXPORT int my_pthread_mutex_unlock(pthread_mutex_t *m)
{
	return pthread_mutex_unlock(getAlignedMutex(m));
}

typedef union my_condattr_s {
	int					x86;
	pthread_condattr_t 	nat;
} my_condattr_t;
// condattr
EXPORT int my_pthread_condattr_destroy(x64emu_t* emu, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_destroy(&cond.nat);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_getclock(x64emu_t* emu, my_condattr_t* c, void* cl)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_getclock(&cond.nat, cl);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_getpshared(x64emu_t* emu, my_condattr_t* c, void* p)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_getpshared(&cond.nat, p);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_init(x64emu_t* emu, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_init(&cond.nat);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_setclock(x64emu_t* emu, my_condattr_t* c, int cl)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_setclock(&cond.nat, cl);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_setpshared(x64emu_t* emu, my_condattr_t* c, int p)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_setpshared(&cond.nat, p);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_cond_init(x64emu_t* emu, pthread_cond_t *pc, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_cond_init(pc, &cond.nat);
	c->x86 = cond.x86;
	return ret;
}

typedef union my_barrierattr_s {
	int						x86;
	pthread_barrierattr_t 	nat;
} my_barrierattr_t;
// barrierattr
EXPORT int my_pthread_barrierattr_destroy(x64emu_t* emu, my_barrierattr_t* b)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_destroy(&battr.nat);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_getpshared(x64emu_t* emu, my_barrierattr_t* b, void* p)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_getpshared(&battr.nat, p);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_init(x64emu_t* emu, my_barrierattr_t* b)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_init(&battr.nat);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_setpshared(x64emu_t* emu, my_barrierattr_t* b, int p)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_setpshared(&battr.nat, p);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrier_init(x64emu_t* emu, pthread_barrier_t* bar, my_barrierattr_t* b, uint32_t count)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrier_init(bar, &battr.nat, count);
	b->x86 = battr.x86;
	return ret;
}

#endif

static void emujmpbuf_destroy(void* p)
{
	emu_jmpbuf_t *ej = (emu_jmpbuf_t*)p;
	if(ej) {
		free(ej->jmpbuf);
		free(ej);
	}
}

static pthread_key_t jmpbuf_key;

emu_jmpbuf_t* GetJmpBuf()
{
	emu_jmpbuf_t *ejb = (emu_jmpbuf_t*)pthread_getspecific(jmpbuf_key);
	if(!ejb) {
		ejb = (emu_jmpbuf_t*)calloc(1, sizeof(emu_jmpbuf_t));
		ejb->jmpbuf = calloc(1, sizeof(struct __jmp_buf_tag));
		pthread_setspecific(jmpbuf_key, ejb);
	}
	return ejb;
}

void init_pthread_helper()
{
	InitCancelThread();
	pthread_key_create(&jmpbuf_key, emujmpbuf_destroy);
#ifndef NOALIGN
	#ifdef TRACK_MUTEX
	unaligned_mutex = kh_init(mutex);
	#endif
#endif
}

void fini_pthread_helper(box64context_t* context)
{
	FreeCancelThread(context);
	CleanStackSize(context);
#ifndef NOALIGN
	#ifdef TRACK_MUTEX
	pthread_mutex_t *m;
	kh_foreach_value(unaligned_mutex, m, 
		pthread_mutex_destroy(m);
		free(m);
	);
	kh_destroy(mutex, unaligned_mutex);
	#endif
#endif
	emu_jmpbuf_t *ejb = (emu_jmpbuf_t*)pthread_getspecific(jmpbuf_key);
	if(ejb) {
		emujmpbuf_destroy(ejb);
		pthread_setspecific(jmpbuf_key, NULL);
	}
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(et) {
		emuthread_destroy(et);
		pthread_setspecific(thread_key, NULL);
	}
}

int checkMutex(void* m)
{
	pthread_mutex_t* mutex = (pthread_mutex_t*)m;
	int ret = pthread_mutex_trylock(mutex);
	if(ret==0) {
		pthread_mutex_unlock(mutex);
		return 0;
	}
	if(ret==EDEADLK)
		return 1;
	return 0;
}
