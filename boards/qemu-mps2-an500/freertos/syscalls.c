/*
 * Minimal syscall stubs for newlib on bare-metal QEMU (FreeRTOS).
 *
 * rdimon's _sbrk checks heap_end against SP, which fails on FreeRTOS
 * because task stacks are allocated separately from the C heap.
 * This _sbrk uses __HeapLimit instead.
 */

#include <errno.h>
#include <stdint.h>

extern char end;           /* Set by linker: start of heap */
extern char __HeapLimit;   /* Set by linker: end of heap */

void *_sbrk(int incr)
{
	static char *heap_end = 0;

	if (heap_end == 0) {
		heap_end = &end;
	}

	char *prev = heap_end;
	if (heap_end + incr > &__HeapLimit) {
		errno = ENOMEM;
		return (void *)-1;
	}

	heap_end += incr;
	return prev;
}
