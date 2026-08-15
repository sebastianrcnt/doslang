#ifndef FE_ARENA_H
#define FE_ARENA_H

#include <stddef.h>

typedef struct FeArenaBlock FeArenaBlock;
typedef struct FeArena {
    FeArenaBlock *blocks;
    size_t block_size;
} FeArena;

void fe_arena_init(FeArena *a, size_t block_size);
void fe_arena_destroy(FeArena *a);
void *fe_arena_alloc(FeArena *a, size_t size);
char *fe_arena_strdup(FeArena *a, const char *s, size_t n);

#endif
