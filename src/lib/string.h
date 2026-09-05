/* Freestanding stand-ins for the string.h routines the compiler and the
 * rest of the tree assume exist. Standard names and signatures, because
 * clang lowers struct copies and loop idioms into calls to these.
 */
#ifndef LIB_STRING_H
#define LIB_STRING_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

#endif
