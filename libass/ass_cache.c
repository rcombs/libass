/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <inttypes.h>
#include <ft2build.h>
#include FT_OUTLINE_H
#include <assert.h>

#if CONFIG_PTHREAD
#include <stdatomic.h>
#include <pthread.h>
#endif

#include "ass_utils.h"
#include "ass_font.h"
#include "ass_outline.h"
#include "ass_cache.h"

// Always enable native-endian mode, since we don't care about cross-platform consistency of the hash
#define WYHASH_LITTLE_ENDIAN 1
#include "wyhash.h"

// With wyhash any arbitrary 64 bit value will suffice
#define ASS_HASH_INIT 0xb3e46a540bd36cd4ULL

static inline ass_hashcode ass_hash_buf(const void *buf, size_t len, ass_hashcode hval)
{
    return wyhash(buf, len, hval, _wyp);
}

// type-specific functions
// create hash/compare functions for bitmap, outline and composite cache
#define CREATE_HASH_FUNCTIONS
#include "ass_cache_template.h"
#define CREATE_COMPARISON_FUNCTIONS
#include "ass_cache_template.h"

// font cache
static bool font_key_move(void *dst, void *src)
{
    ASS_FontDesc *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->family.str = ass_copy_string(s->family);
    return d->family.str;
}

static void font_destruct(void *key, void *value)
{
    ass_font_clear(value);
}

size_t ass_font_construct(void *key, void *value, void *priv);

const CacheDesc font_cache_desc = {
    .hash_func = font_hash,
    .compare_func = font_compare,
    .key_move_func = font_key_move,
    .construct_func = ass_font_construct,
    .destruct_func = font_destruct,
    .key_size = sizeof(ASS_FontDesc),
    .value_size = sizeof(ASS_Font)
};


// bitmap cache
static bool bitmap_key_move(void *dst, void *src)
{
    BitmapHashKey *d = dst, *s = src;
    if (d)
        *d = *s;
    else
        ass_cache_dec_ref(s->outline);
    return true;
}

static void bitmap_destruct(void *key, void *value)
{
    BitmapHashKey *k = key;
    ass_free_bitmap(value);
    ass_cache_dec_ref(k->outline);
}

size_t ass_bitmap_construct(void *key, void *value, void *priv);

const CacheDesc bitmap_cache_desc = {
    .hash_func = bitmap_hash,
    .compare_func = bitmap_compare,
    .key_move_func = bitmap_key_move,
    .construct_func = ass_bitmap_construct,
    .destruct_func = bitmap_destruct,
    .key_size = sizeof(BitmapHashKey),
    .value_size = sizeof(Bitmap)
};


// composite cache
static ass_hashcode composite_hash(void *key, ass_hashcode hval)
{
    CompositeHashKey *k = key;
    hval = filter_hash(&k->filter, hval);
    for (size_t i = 0; i < k->bitmap_count; i++)
        hval = bitmap_ref_hash(&k->bitmaps[i], hval);
    return hval;
}

static bool composite_compare(void *a, void *b)
{
    CompositeHashKey *ak = a;
    CompositeHashKey *bk = b;
    if (!filter_compare(&ak->filter, &bk->filter))
        return false;
    if (ak->bitmap_count != bk->bitmap_count)
        return false;
    for (size_t i = 0; i < ak->bitmap_count; i++)
        if (!bitmap_ref_compare(&ak->bitmaps[i], &bk->bitmaps[i]))
            return false;
    return true;
}

static bool composite_key_move(void *dst, void *src)
{
    CompositeHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        return true;
    }

    for (size_t i = 0; i < s->bitmap_count; i++) {
        ass_cache_dec_ref(s->bitmaps[i].bm);
        ass_cache_dec_ref(s->bitmaps[i].bm_o);
    }
    free(s->bitmaps);
    return true;
}

static void composite_destruct(void *key, void *value)
{
    CompositeHashValue *v = value;
    CompositeHashKey *k = key;
    ass_free_bitmap(&v->bm);
    ass_free_bitmap(&v->bm_o);
    ass_free_bitmap(&v->bm_s);
    for (size_t i = 0; i < k->bitmap_count; i++) {
        ass_cache_dec_ref(k->bitmaps[i].bm);
        ass_cache_dec_ref(k->bitmaps[i].bm_o);
    }
    free(k->bitmaps);
}

size_t ass_composite_construct(void *key, void *value, void *priv);

const CacheDesc composite_cache_desc = {
    .hash_func = composite_hash,
    .compare_func = composite_compare,
    .key_move_func = composite_key_move,
    .construct_func = ass_composite_construct,
    .destruct_func = composite_destruct,
    .key_size = sizeof(CompositeHashKey),
    .value_size = sizeof(CompositeHashValue)
};


// outline cache
static ass_hashcode outline_hash(void *key, ass_hashcode hval)
{
    OutlineHashKey *k = key;
    switch (k->type) {
    case OUTLINE_GLYPH:
        return glyph_hash(&k->u, hval);
    case OUTLINE_DRAWING:
        return drawing_hash(&k->u, hval);
    case OUTLINE_BORDER:
        return border_hash(&k->u, hval);
    default:  // OUTLINE_BOX
        return hval;
    }
}

static bool outline_compare(void *a, void *b)
{
    OutlineHashKey *ak = a;
    OutlineHashKey *bk = b;
    if (ak->type != bk->type)
        return false;
    switch (ak->type) {
    case OUTLINE_GLYPH:
        return glyph_compare(&ak->u, &bk->u);
    case OUTLINE_DRAWING:
        return drawing_compare(&ak->u, &bk->u);
    case OUTLINE_BORDER:
        return border_compare(&ak->u, &bk->u);
    default:  // OUTLINE_BOX
        return true;
    }
}

static bool outline_key_move(void *dst, void *src)
{
    OutlineHashKey *d = dst, *s = src;
    if (!d) {
        if (s->type == OUTLINE_GLYPH)
            ass_cache_dec_ref(s->u.glyph.font);
        return true;
    }

    *d = *s;
    if (s->type == OUTLINE_DRAWING) {
        d->u.drawing.text.str = ass_copy_string(s->u.drawing.text);
        return d->u.drawing.text.str;
    }
    if (s->type == OUTLINE_BORDER)
        ass_cache_inc_ref(s->u.border.outline);
    return true;
}

static void outline_destruct(void *key, void *value)
{
    OutlineHashValue *v = value;
    OutlineHashKey *k = key;
    outline_free(&v->outline[0]);
    outline_free(&v->outline[1]);
    switch (k->type) {
    case OUTLINE_GLYPH:
        ass_cache_dec_ref(k->u.glyph.font);
        break;
    case OUTLINE_DRAWING:
        free((char *) k->u.drawing.text.str);
        break;
    case OUTLINE_BORDER:
        ass_cache_dec_ref(k->u.border.outline);
        break;
    default:  // OUTLINE_BOX
        break;
    }
}

size_t ass_outline_construct(void *key, void *value, void *priv);

const CacheDesc outline_cache_desc = {
    .hash_func = outline_hash,
    .compare_func = outline_compare,
    .key_move_func = outline_key_move,
    .construct_func = ass_outline_construct,
    .destruct_func = outline_destruct,
    .key_size = sizeof(OutlineHashKey),
    .value_size = sizeof(OutlineHashValue)
};


// glyph metric cache
static bool glyph_metrics_key_move(void *dst, void *src)
{
    GlyphMetricsHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void glyph_metrics_destruct(void *key, void *value)
{
    GlyphMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

size_t ass_glyph_metrics_construct(void *key, void *value, void *priv);

const CacheDesc glyph_metrics_cache_desc = {
    .hash_func = glyph_metrics_hash,
    .compare_func = glyph_metrics_compare,
    .key_move_func = glyph_metrics_key_move,
    .construct_func = ass_glyph_metrics_construct,
    .destruct_func = glyph_metrics_destruct,
    .key_size = sizeof(GlyphMetricsHashKey),
    .value_size = sizeof(FT_Glyph_Metrics)
};


#if CONFIG_PTHREAD

#define ATOMIC _Atomic
#define EXCHANGE_STRONG(a, b, c) atomic_compare_exchange_strong(a, b, c)
#define EXCHANGE_WEAK(a, b, c)   atomic_compare_exchange_weak(a, b, c)

#else

#define ATOMIC

static inline bool compare_exchange(void* obj, void* expected, void* desired, size_t size)
{
    if (memcmp(obj, expected, size) == 0) {
        memcpy(obj, desired, size);
        return true;
    } else {
        memcpy(expected, obj, size);
        return false;
    }
}

#define EXCHANGE_STRONG(a, b, c) compare_exchange(a, b, &c, sizeof(c))
#define EXCHANGE_WEAK(a, b, c)   compare_exchange(a, b, &c, sizeof(c))

#endif

// Cache data
typedef struct cache_item {
    Cache *cache;
    const CacheDesc *desc;
    struct cache_item * ATOMIC next, * ATOMIC *prev;
    struct cache_item *queue_next, **queue_prev;
    struct cache_item * ATOMIC frame_next;
    struct cache_item * ATOMIC free_next;
    size_t ATOMIC size;
    size_t ATOMIC ref_count;
} CacheItem;

#define N_MTX_BUCKETS 16

struct cache {
    unsigned buckets;
    CacheItem * ATOMIC *map;
    CacheItem *queue_first, **queue_last;
    CacheItem * ATOMIC frame_first;
    CacheItem * ATOMIC free_first;

    const CacheDesc *desc;

    size_t ATOMIC cache_size;
    unsigned ATOMIC hits;
    unsigned ATOMIC misses;
    unsigned ATOMIC items;
    unsigned ATOMIC refs;

#if CONFIG_PTHREAD
    pthread_mutex_t mutex;
    pthread_cond_t cond;

    pthread_mutex_t bmtx[N_MTX_BUCKETS];
    pthread_cond_t bcond[N_MTX_BUCKETS];
#endif
};

#define CACHE_ALIGN 8
#define CACHE_ITEM_SIZE ((sizeof(CacheItem) + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1))

static inline size_t align_cache(size_t size)
{
    return (size + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1);
}

static inline CacheItem *value_to_item(void *value)
{
    return (CacheItem *) ((char *) value - CACHE_ITEM_SIZE);
}


// Create a cache with type-specific hash/compare/destruct/size functions
Cache *ass_cache_create(const CacheDesc *desc)
{
    Cache *cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    cache->buckets = 0xFFFF;
    cache->queue_last = &cache->queue_first;
    cache->desc = desc;
    cache->map = calloc(cache->buckets, sizeof(CacheItem *));
    cache->frame_first = (void*)-1;
    if (!cache->map) {
        goto fail;
    }

#if CONFIG_PTHREAD
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        goto fail;

    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
        goto faila;

    if (pthread_mutex_init(&cache->mutex, &attr) != 0)
        goto faila;

    for (int i = 0; i < N_MTX_BUCKETS; i++) {
        pthread_mutex_init(&cache->bmtx[i], &attr);
        pthread_cond_init(&cache->bcond[i], NULL);
    }

    if (pthread_cond_init(&cache->cond, NULL) != 0)
        goto failm;

    pthread_mutexattr_destroy(&attr);
#endif

    cache->refs = 1;

    return cache;

#if CONFIG_PTHREAD
failm:
    pthread_mutex_destroy(&cache->mutex);
faila:
    pthread_mutexattr_destroy(&attr);
#endif

fail:
    free(cache->map);
    free(cache);
    return NULL;
}

void *ass_cache_get(Cache *cache, void *key, void *priv)
{
    const CacheDesc *desc = cache->desc;
    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    unsigned bucket = desc->hash_func(key, ASS_HASH_INIT) % cache->buckets;

#if CONFIG_PTHREAD
    pthread_mutex_t *mtx = &cache->bmtx[bucket % N_MTX_BUCKETS];
    pthread_cond_t *cond = &cache->bcond[bucket % N_MTX_BUCKETS];
    bool locked = false;
#endif

    CacheItem *item = cache->map[bucket];
    CacheItem *stopat = NULL;
#if CONFIG_PTHREAD
    CacheItem *prev = NULL;
    do {
        if (locked)
            pthread_mutex_unlock(mtx);
        stopat = prev;
        prev = item;
#endif
        while (item != stopat) {
            bool matched = desc->compare_func(key, (char *) item + key_offs);
            if (matched) {
                if (item->ref_count++ == 0)
                    cache->refs++;

                if (!item->frame_next) {
                    bool inc = !item->queue_prev;
                    CacheItem *next;
                    CacheItem *expected = NULL;
                    do {
                        next = cache->frame_first;
                        if (!EXCHANGE_STRONG(&item->frame_next, &expected, next)) {
                            inc = false;
                            break;
                        }
                        expected = next;
                    } while (!EXCHANGE_WEAK(&cache->frame_first, &next, item));

                    if (inc)
                        item->ref_count++;
                }
                cache->hits++;

#if CONFIG_PTHREAD
                if (!item->size) {
                    pthread_mutex_lock(mtx);

                    while (!item->size)
                        pthread_cond_wait(cond, mtx);

                    pthread_mutex_unlock(mtx);
                }
#endif

                desc->key_move_func(NULL, key);

                return (char *) item + CACHE_ITEM_SIZE;
            }
            item = item->next;
        }
#if CONFIG_PTHREAD
        pthread_mutex_lock(mtx);
        locked = true;
    } while ((item = cache->map[bucket]) != prev);
#endif

    item = malloc(key_offs + desc->key_size);
    if (!item) {
#if CONFIG_PTHREAD
        pthread_mutex_unlock(mtx);
#endif
        desc->key_move_func(NULL, key);
        return NULL;
    }
    void *new_key = (char *) item + key_offs;
    if (!desc->key_move_func(new_key, key)) {
#if CONFIG_PTHREAD
        pthread_mutex_unlock(mtx);
#endif
        free(item);
        return NULL;
    }

    item->cache = cache;
    item->desc = desc;
    item->size = 0;
    item->ref_count = 2;
    item->queue_next = NULL;
    item->queue_prev = (void*)-1;
    item->free_next = NULL;

    CacheItem *next;
    do {
        next = cache->frame_first;
        item->frame_next = next;
    } while (!EXCHANGE_WEAK(&cache->frame_first, &next, item));

    CacheItem * ATOMIC *bucketptr = &cache->map[bucket];
    if (*bucketptr)
        (*bucketptr)->prev = &item->next;
    item->prev = bucketptr;
    item->next = *bucketptr;
    *bucketptr = item;

#if CONFIG_PTHREAD
    pthread_mutex_unlock(mtx);
#endif

    void *value = (char *) item + CACHE_ITEM_SIZE;
    size_t size = desc->construct_func(new_key, value, priv);
    assert(size);

    cache->cache_size += size;
    cache->items++;
    cache->refs++;
    cache->misses++;

#if CONFIG_PTHREAD
    pthread_mutex_lock(mtx);
#endif

    item->size = size;

#if CONFIG_PTHREAD
    pthread_mutex_unlock(mtx);
    pthread_cond_broadcast(cond);
#endif

    return value;
}

void *ass_cache_key(void *value)
{
    CacheItem *item = value_to_item(value);
    return (char *) value + align_cache(item->desc->value_size);
}

static inline void destroy_item(const CacheDesc *desc, CacheItem *item)
{
    assert(item->desc == desc);
    char *value = (char *) item + CACHE_ITEM_SIZE;
    desc->destruct_func(value + align_cache(desc->value_size), value);
    free(item);
}

void ass_cache_inc_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    assert(item->size && item->ref_count);
    item->ref_count++;
}

void process_frame_list(Cache *cache)
{
    CacheItem *next;
    for (CacheItem *item = cache->frame_first; item && item != (void*)-1; item = next) {
        if (!item->queue_prev || item->queue_prev == (void*)-1 || item->queue_next) {
            if (item->queue_prev && item->queue_prev != (void*)-1) {
                item->queue_next->queue_prev = item->queue_prev;
                *item->queue_prev = item->queue_next;
            }
            *cache->queue_last = item;
            item->queue_prev = cache->queue_last;
            cache->queue_last = &item->queue_next;
            item->queue_next = NULL;
        }

        next = item->frame_next;
        item->frame_next = NULL;
    }

    cache->frame_first = (void*)-1;
}

void process_free_list(Cache *cache)
{
    CacheItem *item;
    while ((item = cache->free_first)) {
        CacheItem *next = item->free_next;

        if (!EXCHANGE_WEAK(&cache->free_first, &item, next))
            continue;

        item->free_next = NULL;

        // The item may have been re-ref'd before we got to it
        if (item->ref_count)
            continue;

        // Remove from cache bucket
        if (item->next)
            item->next->prev = item->prev;
        if (item->prev)
            *item->prev = item->next;

        if (item->queue_next)
            item->queue_next->queue_prev = item->queue_prev;
        if (item->queue_prev)
            *item->queue_prev = item->queue_next;

        item->next = NULL;
        item->prev = NULL;
        item->queue_next = NULL;
        item->queue_prev = NULL;

        cache->cache_size -= item->size;
        cache->items--;

        // Delete
        destroy_item(item->desc, item);
    }
}

static void cache_dec_ref(Cache *cache)
{
    if (--cache->refs == 0) {
        process_free_list(cache);

#if CONFIG_PTHREAD
        pthread_mutex_destroy(&cache->mutex);
        pthread_cond_destroy(&cache->cond);
#endif

        free(cache);
    }
}

void ass_cache_dec_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    assert(item->size && item->ref_count);
    if (--item->ref_count)
        return;

    Cache *cache = item->cache;

    if (!cache->map) {
        cache->cache_size -= item->size;
        cache->items--;

        destroy_item(item->desc, item);
    } else {
        CacheItem *next;
        CacheItem *expected = NULL;
        do {
            next = cache->free_first;
            if (!EXCHANGE_STRONG(&item->free_next, &expected, next))
                break;
            expected = next;
        } while (!EXCHANGE_WEAK(&cache->free_first, &next, item));
    }

    cache_dec_ref(cache);
}

void ass_cache_cut(Cache *cache, size_t max_size)
{
    process_frame_list(cache);
    process_free_list(cache);

    if (cache->cache_size <= max_size)
        return;

    do {
        CacheItem *item = cache->queue_first;
        if (!item)
            break;
        assert(item->size);

        cache->queue_first = item->queue_next;
        if (--item->ref_count) {
            item->queue_prev = NULL;
            continue;
        }

        if (item->next)
            item->next->prev = item->prev;
        *item->prev = item->next;

        cache->items--;
        cache->refs--;
        cache->cache_size -= item->size;
        destroy_item(cache->desc, item);
    } while (cache->cache_size > max_size);
    if (cache->queue_first)
        cache->queue_first->queue_prev = &cache->queue_first;
    else
        cache->queue_last = &cache->queue_first;
}

void ass_cache_stats(Cache *cache, size_t *size, unsigned *hits,
                     unsigned *misses, unsigned *count)
{
    if (size)
        *size = cache->cache_size;
    if (hits)
        *hits = cache->hits;
    if (misses)
        *misses = cache->misses;
    if (count)
        *count = cache->items;
}

void ass_cache_empty(Cache *cache)
{
    process_frame_list(cache);
    process_free_list(cache);

    for (int i = 0; i < cache->buckets; i++) {
        CacheItem *item = cache->map[i];
        while (item) {
            assert(item->size);
            CacheItem *next = item->next;

            if (item->queue_prev)
                item->ref_count--;

            if (item->next)
                item->next->prev = item->prev;
            *item->prev = item->next;

            item->next = NULL;
            item->prev = NULL;
            item->queue_next = NULL;
            item->queue_prev = NULL;

            if (!item->ref_count) {
                destroy_item(cache->desc, item);
                cache->items--;
                cache->refs--;
            }
            item = next;
        }
        cache->map[i] = NULL;
    }

    cache->queue_first = NULL;
    cache->queue_last = &cache->queue_first;

//    cache->hits = cache->misses = 0;
}

void ass_cache_done(Cache *cache)
{
    ass_cache_empty(cache);

    free(cache->map);
    cache->map = NULL;

    cache_dec_ref(cache);
}

// Type-specific creation function
Cache *ass_font_cache_create(void)
{
    return ass_cache_create(&font_cache_desc);
}

Cache *ass_outline_cache_create(void)
{
    return ass_cache_create(&outline_cache_desc);
}

Cache *ass_glyph_metrics_cache_create(void)
{
    return ass_cache_create(&glyph_metrics_cache_desc);
}

Cache *ass_bitmap_cache_create(void)
{
    return ass_cache_create(&bitmap_cache_desc);
}

Cache *ass_composite_cache_create(void)
{
    return ass_cache_create(&composite_cache_desc);
}
