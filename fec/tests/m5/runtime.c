#include <stdlib.h>
#include <stddef.h>

#undef malloc
#undef free

extern long fe_m5_runtime_run(long mode);
extern void fe_m5_runtime_conditional(unsigned char flag);
extern void fe_m5_runtime_argument_cleanup(void);

static void *live_ptrs[64];
static unsigned live_count;
static unsigned alloc_count;
static unsigned free_count;
static unsigned double_free_count;

void *m5_malloc(size_t size)
{
    void *p = malloc(size);
    if (p && live_count < 64) live_ptrs[live_count++] = p;
    if (p) ++alloc_count;
    return p;
}

void m5_free(void *p)
{
    unsigned i;
    if (!p) return;
    for (i = 0; i < live_count; ++i) {
        if (live_ptrs[i] == p) {
            live_ptrs[i] = live_ptrs[--live_count];
            ++free_count;
            free(p);
            return;
        }
    }
    ++double_free_count;
}

int main(void)
{
    if (fe_m5_runtime_run(0) != 0) return 1;
    if (fe_m5_runtime_run(1) != 9) return 2;
    if (fe_m5_runtime_run(2) != 0) return 3;
    fe_m5_runtime_conditional(0);
    fe_m5_runtime_conditional(1);
    fe_m5_runtime_argument_cleanup();
    if (double_free_count != 0) return 4;
    if (live_count != 0) return 5;
    if (alloc_count != free_count) return 6;
    return 0;
}
