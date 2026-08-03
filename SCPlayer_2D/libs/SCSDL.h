//
//  SCSDL.h
//  SCFFmpeg
//
//  Created by stan on 2024/7/9.
//  Copyright © 2024 石川. All rights reserved.
//



#ifndef SCSDL_h
#define SCSDL_h
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/// 定义 SDL 调用约定宏
#ifndef SDLCALL
#if (defined(__WIN32__) || defined(__WINRT__) || defined(__GDK__)) && !defined(__GNUC__)
#define SDLCALL __cdecl
#elif defined(__OS2__) || defined(__EMX__)
#define SDLCALL _System
# if defined (__GNUC__) && !defined(_System)
#  define _System /* for old EMX/GCC compat.  */
# endif
#else
#define SDLCALL
#endif
#endif /* SDLCALL */

/* Removed DECLSPEC on Symbian OS because SDL cannot be a DLL in EPOC */
#ifdef __SYMBIAN32__
#undef DECLSPEC
#define DECLSPEC
#endif /* __SYMBIAN32__ */

#ifndef DECLSPEC
# if defined(__WIN32__) || defined(__WINRT__) || defined(__CYGWIN__) || defined(__GDK__)
#  ifdef DLL_EXPORT
#   define DECLSPEC __declspec(dllexport)
#  else
#   define DECLSPEC
#  endif
# elif defined(__OS2__)
#   ifdef BUILD_SDL
#    define DECLSPEC    __declspec(dllexport)
#   else
#    define DECLSPEC
#   endif
# else
#  if defined(__GNUC__) && __GNUC__ >= 4
#   define DECLSPEC __attribute__ ((visibility("default")))
#  else
#   define DECLSPEC
#  endif
# endif
#endif

// 定义 SDL 线程作为 pthread_t 的别名
typedef pthread_t SDL_Thread;

// 定义 SDL 条件变量和互斥锁作为 pthread_cond_t 和 pthread_mutex_t 的别名
typedef pthread_cond_t  SDL_cond;
typedef pthread_mutex_t SDL_mutex;

// 定义线程函数指针类型
typedef int (*SDL_ThreadFunction) (void *);

// SDL_CreateThread 的实现
extern DECLSPEC SDL_Thread* SDLCALL SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data) {
    pthread_t *thread = (pthread_t *)malloc(sizeof(pthread_t));
    if (!thread) {
        fprintf(stderr, "Error: Failed to allocate memory for thread\n");
        return NULL;
    }
    
    if (pthread_create(thread, NULL, (void *(*)(void *))fn, data) != 0) {
        fprintf(stderr, "Error: Failed to create thread\n");
        free(thread);
        return NULL;
    }

    return (SDL_Thread *)thread;
}

// SDL_WaitThread 的实现
extern DECLSPEC void SDLCALL SDL_WaitThread(SDL_Thread *thread, int *status) {
    if (thread) {
        void *retval;
        pthread_join(*thread, &retval);
        if (status) {
            *status = (int)(intptr_t)retval;
        }
        free(thread);
    }
}

// SDL_DestroyThread (如果需要，POSIX 线程在终止时会自动释放资源，因此通常不需要显式销毁)

// SDL_CreateMutex 的实现
extern DECLSPEC SDL_mutex *SDLCALL SDL_CreateMutex(void) {
    SDL_mutex *mutex = (SDL_mutex *)malloc(sizeof(SDL_mutex));
    if (mutex) {
        pthread_mutex_init(mutex, NULL);
    }
    return mutex;
}

// SDL_DestroyMutex 的实现
extern DECLSPEC void SDLCALL SDL_DestroyMutex(SDL_mutex *mutex) {
    if (mutex) {
        pthread_mutex_destroy(mutex);
        free(mutex);
    }
}

// SDL_LockMutex 的实现
extern DECLSPEC int SDLCALL SDL_LockMutex(SDL_mutex *mutex) {
    return pthread_mutex_lock(mutex);
}

// SDL_UnlockMutex 的实现
extern DECLSPEC int SDLCALL SDL_UnlockMutex(SDL_mutex *mutex) {
    return pthread_mutex_unlock(mutex);
}

// SDL_CreateCond 的实现
extern DECLSPEC SDL_cond *SDLCALL SDL_CreateCond(void) {
    SDL_cond *cond = (SDL_cond *)malloc(sizeof(SDL_cond));
    if (cond) {
        pthread_cond_init(cond, NULL);
    }
    return cond;
}

// SDL_DestroyCond 的实现
extern DECLSPEC void SDLCALL SDL_DestroyCond(SDL_cond *cond) {
    if (cond) {
        pthread_cond_destroy(cond);
        free(cond);
    }
}

// SDL_CondSignal 的实现
extern DECLSPEC int SDLCALL SDL_CondSignal(SDL_cond *cond) {
    return pthread_cond_signal(cond);
}

// SDL_CondWait 的实现
extern DECLSPEC int SDLCALL SDL_CondWait(SDL_cond *cond, SDL_mutex *mutex) {
    return pthread_cond_wait(cond, mutex);
}




// 定义 Uint32 类型，如果还未定义
typedef unsigned int Uint32;
// 提前声明 usleep 函数
extern int usleep(useconds_t usec);

// SDL_Delay 的实现
extern DECLSPEC void SDLCALL SDL_Delay(Uint32 ms) {
    usleep(ms * 1000); // 将毫秒转换为微秒
}








#endif /* SCSDL_h */
