/*
 * lwIP architecture definitions for GCC / ARM Cortex-M.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/* Types — lwIP expects these to be defined by the port. */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

/* Critical section protection type */
typedef int sys_prot_t;

/* Compiler hints */
#define LWIP_NO_STDDEF_H  0
#define LWIP_NO_STDINT_H  0
#define LWIP_NO_INTTYPES_H 0

/* Printf formatters */
#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"

/* Byte order — ARM is little-endian */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* Diagnostics */
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { printf("ASSERT: %s\n", x); while(1); } while(0)

/* Random number for initial TCP sequence */
#define LWIP_RAND() ((u32_t)rand())

#endif /* LWIP_ARCH_CC_H */
