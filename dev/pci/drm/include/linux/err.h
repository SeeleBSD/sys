/* Public domain. */

#ifndef _LINUX_ERR_H
#define _LINUX_ERR_H

#include <sys/errno.h>
#include <linux/compiler.h>

#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-ELAST)

static inline void *
ERR_PTR(long error)
{
	return (void *) error;
}

void *BINDING_ERR_PTR(long error);

static inline long
PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

long BINDING_PTR_ERR(const void *ptr);

static inline bool
IS_ERR(const void *ptr)
{
        return IS_ERR_VALUE((unsigned long)ptr);
}

bool BINDING_IS_ERR(const void *ptr);

static inline bool
IS_ERR_OR_NULL(const void *ptr)
{
        return !ptr || IS_ERR_VALUE((unsigned long)ptr);
}

bool BINDING_IS_ERR_OR_NULL(const void *ptr);

static inline void *
ERR_CAST(const void *ptr)
{
	return (void *)ptr;
}

void *BINDING_ERR_CAST(const void *ptr);

static inline int
PTR_ERR_OR_ZERO(const void *ptr)
{
	return IS_ERR(ptr)? PTR_ERR(ptr) : 0;
}

int BINDING_PTR_ERR_OR_ZERO(const void *ptr);

#endif
