#include "lib/sort.h"

static void sift_down(const void **a, size_t start, size_t end,
                      int (*cmp)(const void *, const void *))
{
    size_t root = start;

    while (2 * root + 1 <= end) {
        size_t child = 2 * root + 1;
        if (child + 1 <= end && cmp(a[child], a[child + 1]) < 0)
            child++;
        if (cmp(a[root], a[child]) >= 0)
            return;
        const void *tmp = a[root];
        a[root] = a[child];
        a[child] = tmp;
        root = child;
    }
}

void ksort_ptrs(const void **a, size_t n,
                int (*cmp)(const void *, const void *))
{
    if (n < 2)
        return;
    for (size_t start = (n - 2) / 2;; start--) {
        sift_down(a, start, n - 1, cmp);
        if (start == 0)
            break;
    }
    for (size_t end = n - 1; end > 0; end--) {
        const void *tmp = a[0];
        a[0] = a[end];
        a[end] = tmp;
        sift_down(a, 0, end - 1, cmp);
    }
}
