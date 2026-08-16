#include <stdlib.h>
#include <stddef.h>

#undef malloc
#undef free

extern void *malloc(size_t size);
extern void free(void *p);

extern long fe_m5_runtime_run(long mode);
extern unsigned short fe_m5_runtime_conditional(unsigned char flag);
extern unsigned short fe_m5_runtime_argument_cleanup(void);
extern unsigned short fe_m5_runtime_owned_slice(unsigned long n);
extern unsigned short fe_m5_runtime_replace_field(void);
extern unsigned short fe_m5_runtime_loop_cleanup(void);
extern unsigned short fe_m5_runtime_try_cleanup(void);
extern unsigned short fe_m5_runtime_field_order(void);
extern unsigned short fe_m5_runtime_defer_order(void);
extern unsigned short fe_m5_runtime_match_cleanup(unsigned char flag);
extern unsigned short fe_m5_runtime_close_once(void);
extern unsigned short fe_m5_runtime_reassign_struct(void);

static void *live_ptrs[64];
static unsigned live_count;
static unsigned alloc_count;
static unsigned free_count;
static unsigned double_free_count;
static long fail_after = -1;
static unsigned malloc_attempts;
static int track_order;
static void *order_ptrs[2];
static unsigned order_allocs;
static unsigned order_frees;
static unsigned order_bad;

void *m5_malloc(size_t size)
{
    void *p;
    if (fail_after >= 0 && (long)malloc_attempts++ == fail_after) return 0;
    p = malloc(size);
    if (p && live_count < 64) live_ptrs[live_count++] = p;
    if (p) ++alloc_count;
    if (p && track_order && order_allocs < 2) order_ptrs[order_allocs++] = p;
    return p;
}

void m5_free(void *p)
{
    unsigned i;
    if (!p) return;
    for (i = 0; i < live_count; ++i) {
        if (live_ptrs[i] == p) {
            if (track_order && order_frees < 2 &&
                p != order_ptrs[1-order_frees]) ++order_bad;
            if (track_order && order_frees < 2) ++order_frees;
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
    if (fe_m5_runtime_conditional(0) != 0) return 4;
    if (fe_m5_runtime_conditional(1) != 0) return 5;
    if (fe_m5_runtime_argument_cleanup() != 0) return 6;
    if (fe_m5_runtime_owned_slice(17) != 0) return 7;
    if (fe_m5_runtime_replace_field() != 0) return 8;
    if (fe_m5_runtime_loop_cleanup() != 0) return 9;
    fail_after=1;
    malloc_attempts=0;
    if (fe_m5_runtime_try_cleanup() == 0) return 10;
    fail_after=-1;
    track_order=1;
    order_allocs=order_frees=order_bad=0;
    if (fe_m5_runtime_field_order() != 0) return 11;
    track_order=0;
    if (order_allocs != 2 || order_frees != 2 || order_bad != 0) return 12;
    track_order=1;
    order_allocs=order_frees=order_bad=0;
    if (fe_m5_runtime_defer_order() != 0) return 13;
    track_order=0;
    if (order_allocs != 2 || order_frees != 2 || order_bad != 0) return 14;
    if (fe_m5_runtime_match_cleanup(0) != 0) return 15;
    if (fe_m5_runtime_match_cleanup(1) != 0) return 16;
    if (fe_m5_runtime_close_once() != 0) return 17;
    if (fe_m5_runtime_reassign_struct() != 0) return 18;
    if (double_free_count != 0) return 19;
    if (live_count != 0) return 20;
    if (alloc_count != free_count) return 21;
    return 0;
}
