#include "arena.h"
#include <stdlib.h>
#include <string.h>

struct FeArenaBlock {
    FeArenaBlock *next;
    size_t used;
    size_t size;
    unsigned char data[1];
};

void fe_arena_init(FeArena *a, size_t block_size)
{
    a->blocks = 0;
    a->block_size = block_size ? block_size : 16384;
}

void fe_arena_destroy(FeArena *a)
{
    FeArenaBlock *b = a->blocks;
    while (b) {
        FeArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->blocks = 0;
}

void *fe_arena_alloc(FeArena *a, size_t size)
{
    FeArenaBlock *b;
    size_t need;
    if (size == 0) size = 1;
    need = (size + 7u) & ~(size_t)7u;
    b = a->blocks;
    if (!b || b->used + need > b->size) {
        size_t bs = a->block_size > need ? a->block_size : need;
        b = (FeArenaBlock *)malloc(sizeof(FeArenaBlock) + bs - 1);
        if (!b) return 0;
        b->next = a->blocks;
        b->used = 0;
        b->size = bs;
        a->blocks = b;
    }
    b->used += need;
    return b->data + b->used - need;
}

char *fe_arena_strdup(FeArena *a, const char *s, size_t n)
{
    char *p = (char *)fe_arena_alloc(a, n + 1);
    if (!p) return 0;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}
