#include "debug.h"

/*
 * Skip unit if not debugging memory.
 */
#ifdef MEM_DEBUG

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>


/*
 * ((destructor)) attribute executes this function after main().
 * \see http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
 */
void __attribute__ ((destructor)) usage_dump()
{
	/* Get resource usage. */
	struct rusage usage;
	if (getrusage(RUSAGE_SELF, &usage) < 0) {
		memset(&usage, 0, sizeof(struct rusage));
	}

	fprintf(stderr, "\nMemory statistics:");
	fprintf(stderr, "\n==================\n");

	fprintf(stderr, "User time: %.03lf ms\nSystem time: %.03lf ms\n",
	        usage.ru_utime.tv_sec * (double) 1000.0
	        + usage.ru_utime.tv_usec / (double)1000.0,
	        usage.ru_stime.tv_sec * (double) 1000.0
	        + usage.ru_stime.tv_usec / (double)1000.0);
	fprintf(stderr, "Major page faults: %lu (required I/O)\nMinor page faults: %lu\n",
	        usage.ru_majflt, usage.ru_minflt);
	fprintf(stderr, "Number of swaps: %lu\n",
	        usage.ru_nswap);
	fprintf(stderr, "Voluntary context switches: %lu\nInvoluntary context switches: %lu\n",
	        usage.ru_nvcsw,
	        usage.ru_nivcsw);
	fprintf(stderr, "==================\n");
}

#endif

/*
#ifndef MEM_NOSLAB
#include "slab.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

void *malloc(size_t size)
{
	void* mem = slab_alloc_g(size);
	fprintf(stderr, "%s(%lu) = %p\n", __func__, size, mem);
	return mem;
}

void *calloc(size_t nmemb, size_t size)
{
	const size_t nsz = nmemb * size;
	void* mem = slab_alloc_g(nsz);
	memset(mem, 0, nsz);
	fprintf(stderr, "%s(%lu, %lu) = %p\n", __func__, nmemb, size, mem);
	return mem;
}

void *realloc(void *ptr, size_t size)
{
	if (ptr < sbrk(0)) {
		fprintf(stderr, "!!!! %p under sbrk\n", ptr);
		void (*libc_realloc)(void*) = dlsym(RTLD_NEXT, "realloc");
		libc_realloc(ptr);
	}
	void* mem = slab_realloc_g(ptr, size);
	fprintf(stderr, "%s(%p, %lu) = %p\n", __func__, ptr, size, mem);
	return mem;
}

void free(void *ptr)
{
	fprintf(stderr, "%s(%p)\n", __func__, ptr);
	slab_free(ptr);
}

#endif
*/
