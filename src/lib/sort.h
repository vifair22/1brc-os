/* Heapsort over an array of pointers. Unlike qsort, the comparator
 * receives the element pointers themselves, not pointers to elements;
 * n log n, no recursion, no allocation, which is all a kernel wants
 * from a sort.
 */
#ifndef LIB_SORT_H
#define LIB_SORT_H

#include <stddef.h>

void ksort_ptrs(const void **a, size_t n,
                int (*cmp)(const void *, const void *));

#endif
