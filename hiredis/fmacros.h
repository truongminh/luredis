#ifndef __HIREDIS_FMACRO_H
#define __HIREDIS_FMACRO_H

#if !defined(_BSD_SOURCE)
#define _BSD_SOURCE
#endif

#if defined(__sun__)
#define _POSIX_C_SOURCE 200112L
#elif defined(__linux__)
    #ifndef _XOPEN_SOURCE
        #define _XOPEN_SOURCE 600
    #endif
#else
    #define _XOPEN_SOURCE
#endif

#endif
