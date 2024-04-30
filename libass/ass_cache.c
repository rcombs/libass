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

// We have some asserts on highly-contended atomics in here,
// so they're disabled if not requested
#if !defined(DEBUG) && !defined(NDEBUG)
#define NDEBUG 1
#endif

#include "config.h"
#include "ass_compat.h"

#include <inttypes.h>
#include <ft2build.h>
#include FT_OUTLINE_H
#include <assert.h>

#include "ass_utils.h"
#include "ass_font.h"
#include "ass_outline.h"
#include "ass_shaper.h"
#include "ass_cache.h"
#include "ass_threading.h"

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

static void font_key_destruct(void *k)
{
    ASS_FontDesc *key = k;
    free((void *) key->family.str);
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
    .key_destruct_func = font_key_destruct,
    .construct_func = ass_font_construct,
    .destruct_func = font_destruct,
    .key_size = sizeof(ASS_FontDesc),
    .value_size = sizeof(ASS_Font)
};


// bitmap cache
static bool bitmap_key_move(void *dst, void *src)
{
    BitmapHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        ass_cache_inc_ref(d->outline);
    }
    return true;
}

static void bitmap_key_destruct(void *key)
{
    BitmapHashKey *k = key;
    ass_cache_dec_ref(k->outline);
}

static void bitmap_destruct(void *key, void *value)
{
    bitmap_key_destruct(key);
    ass_free_bitmap(value);
}

size_t ass_bitmap_construct(void *key, void *value, void *priv);

const CacheDesc bitmap_cache_desc = {
    .hash_func = bitmap_hash,
    .compare_func = bitmap_compare,
    .key_move_func = bitmap_key_move,
    .key_destruct_func = bitmap_key_destruct,
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
        for (size_t i = 0; i < d->bitmap_count; i++) {
            ass_cache_inc_ref(d->bitmaps[i].bm);
            ass_cache_inc_ref(d->bitmaps[i].bm_o);
        }
        return true;
    }

    free(s->bitmaps);
    return true;
}

static void composite_key_destruct(void *key)
{
    CompositeHashKey *k = key;
    for (size_t i = 0; i < k->bitmap_count; i++) {
        ass_cache_dec_ref(k->bitmaps[i].bm);
        ass_cache_dec_ref(k->bitmaps[i].bm_o);
    }
    free(k->bitmaps);
}

static void composite_destruct(void *key, void *value)
{
    CompositeHashValue *v = value;
    ass_free_bitmap(&v->bm);
    ass_free_bitmap(&v->bm_o);
    ass_free_bitmap(&v->bm_s);
    composite_key_destruct(key);
}

size_t ass_composite_construct(void *key, void *value, void *priv);

const CacheDesc composite_cache_desc = {
    .hash_func = composite_hash,
    .compare_func = composite_compare,
    .key_move_func = composite_key_move,
    .key_destruct_func = composite_key_destruct,
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
        return true;
    }

    *d = *s;
    if (s->type == OUTLINE_DRAWING) {
        d->u.drawing.text.str = ass_copy_string(s->u.drawing.text);
        return d->u.drawing.text.str;
    }
    if (s->type == OUTLINE_BORDER)
        ass_cache_inc_ref(s->u.border.outline);
    else if (s->type == OUTLINE_GLYPH)
        ass_cache_inc_ref(s->u.glyph.font);
    return true;
}

static void outline_key_destruct(void *key)
{
    OutlineHashKey *k = key;
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

static void outline_destruct(void *key, void *value)
{
    OutlineHashValue *v = value;
    ass_outline_free(&v->outline[0]);
    ass_outline_free(&v->outline[1]);
    outline_key_destruct(key);
}

size_t ass_outline_construct(void *key, void *value, void *priv);

const CacheDesc outline_cache_desc = {
    .hash_func = outline_hash,
    .compare_func = outline_compare,
    .key_move_func = outline_key_move,
    .construct_func = ass_outline_construct,
    .key_destruct_func = outline_key_destruct,
    .destruct_func = outline_destruct,
    .key_size = sizeof(OutlineHashKey),
    .value_size = sizeof(OutlineHashValue)
};


// font-face size metric cache
static bool sized_shaper_font_key_move(void *dst, void *src)
{
    SizedShaperFontHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void sized_shaper_font_key_destruct(void *key)
{
    SizedShaperFontHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

static void sized_shaper_font_destruct(void *key, void *value)
{
    sized_shaper_font_key_destruct(key);
    hb_font_destroy(*(hb_font_t**)value);
}

size_t ass_sized_shaper_font_construct(void *key, void *value, void *priv);

const CacheDesc sized_shaper_font_cache_desc = {
    .hash_func = sized_shaper_font_hash,
    .compare_func = sized_shaper_font_compare,
    .key_move_func = sized_shaper_font_key_move,
    .key_destruct_func = sized_shaper_font_key_destruct,
    .construct_func = ass_sized_shaper_font_construct,
    .destruct_func = sized_shaper_font_destruct,
    .key_size = sizeof(SizedShaperFontHashKey),
    .value_size = sizeof(hb_font_t*)
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

static void glyph_metrics_key_destruct(void *key)
{
    GlyphMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

static void glyph_metrics_destruct(void *key, void *value)
{
    glyph_metrics_key_destruct(key);
}

size_t ass_glyph_metrics_construct(void *key, void *value, void *priv);

const CacheDesc glyph_metrics_cache_desc = {
    .hash_func = glyph_metrics_hash,
    .compare_func = glyph_metrics_compare,
    .key_move_func = glyph_metrics_key_move,
    .key_destruct_func = glyph_metrics_key_destruct,
    .construct_func = ass_glyph_metrics_construct,
    .destruct_func = glyph_metrics_destruct,
    .key_size = sizeof(GlyphMetricsHashKey),
    .value_size = sizeof(FT_Glyph_Metrics)
};



// Cache data
typedef struct cache_item {
    const CacheDesc *desc;
    struct cache_item *_Atomic next, *_Atomic *prev;
    struct cache_item *_Atomic queue_next, *_Atomic *queue_prev;
    struct cache_item *promote_next;
    _Atomic uintptr_t size, ref_count;
    ass_hashcode hash;

    _Atomic uintptr_t last_used_frame;

#if ENABLE_THREADS
    struct cache_client *creating_client;
#endif
} CacheItem;

struct cache {
    unsigned buckets;
    CacheItem *_Atomic *map;
    CacheItem *_Atomic queue_first, *_Atomic *_Atomic queue_last;

    const CacheDesc *desc;

    _Atomic uintptr_t cache_size;

    uintptr_t cur_frame;

    size_t n_clients;
    struct cache_client **clients;

#if ENABLE_THREADS
    pthread_mutex_t mutex;
#endif
};

struct cache_client {
    Cache *cache;
    CacheItem *promote_first;
    size_t idx;

#if ENABLE_THREADS
    pthread_mutex_t mutex;
    pthread_cond_t cond;
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
    if (!cache->map) {
        free(cache);
        return NULL;
    }

#if ENABLE_THREADS
    if (pthread_mutex_init(&cache->mutex, NULL) != 0) {
        free(cache->map);
        free(cache);
        return NULL;
    }
#endif

    return cache;
}

CacheClient *ass_cache_client_create(Cache *cache)
{
    CacheClient *client = NULL;

#if ENABLE_THREADS
    pthread_mutex_lock(&cache->mutex);
#endif

    size_t idx;

    for (idx = 0; idx < cache->n_clients; idx++) {
        if (!cache->clients[idx])
            break;
    }

    if (idx >= cache->n_clients) {
        if (!ASS_REALLOC_ARRAY(cache->clients, idx + 1))
            goto fail;
    }

    client = calloc(1, sizeof(*client));
    if (!client)
        goto fail;

    client->cache = cache;
    client->idx = idx;

#if ENABLE_THREADS
    if (pthread_mutex_init(&client->mutex, NULL) != 0) {
        free(client);
        goto fail;
    }

    if (pthread_cond_init(&client->cond, NULL) != 0) {
        pthread_cond_destroy(&client->cond);
        free(client);
        goto fail;
    }
#endif

    cache->clients[idx] = client;
    if (idx >= cache->n_clients)
        cache->n_clients = idx + 1;

fail:
#if ENABLE_THREADS
    pthread_mutex_unlock(&cache->mutex);
#endif

    return client;
}

void ass_cache_client_done(CacheClient *client)
{
    if (!client)
        return;

    Cache *cache = client->cache;

#if ENABLE_THREADS
    pthread_mutex_lock(&cache->mutex);
#endif

    cache->clients[client->idx] = NULL;

#if ENABLE_THREADS
    pthread_mutex_destroy(&client->mutex);
    pthread_cond_destroy(&client->cond);

    pthread_mutex_unlock(&cache->mutex);
#endif

    free(client);
}

void *ass_cache_get(CacheClient *client, void *key, void *priv)
{
    Cache *cache = client->cache;
    const CacheDesc *desc = cache->desc;
    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    ass_hashcode hash = desc->hash_func(key, ASS_HASH_INIT);
    unsigned bucket = hash % cache->buckets;

    CacheItem *_Atomic *bucketptr = &cache->map[bucket];
    CacheItem *stop_at = NULL;
    CacheItem *item, *new_item = NULL;
    void *new_key = NULL;
    CacheItem *start = *bucketptr;

retry:
    for (item = start; item && item != stop_at; item = item->next) {
        if (item->hash == hash && desc->compare_func(key, (char *)item + key_offs))
            break;
    }

    if (item != NULL && item != stop_at) {
        if (atomic_load_explicit(&item->last_used_frame, memory_order_consume) != cache->cur_frame) {
            uintptr_t last_used = atomic_exchange_explicit(&item->last_used_frame, cache->cur_frame, memory_order_consume);

            if (last_used != cache->cur_frame) {
                item->promote_next = client->promote_first;
                client->promote_first = item;
            }
        }

        if (new_item && desc->key_destruct_func)
            desc->key_destruct_func(key);
        else
            desc->key_move_func(NULL, key);

#if ENABLE_THREADS
        if (!atomic_load_explicit(&item->size, memory_order_acquire)) {
            pthread_mutex_lock(&item->creating_client->mutex);

            while (!item->size)
                pthread_cond_wait(&item->creating_client->cond, &item->creating_client->mutex);

            pthread_mutex_unlock(&item->creating_client->mutex);
        }

        if (new_item) {
            free(new_item);
        }
#endif

        return (char *) item + CACHE_ITEM_SIZE;
    }

    stop_at = start;

    if (!new_item) {
        // Risk of cache miss. Set up a new item to insert if we win the race.
        new_item = malloc(key_offs + desc->key_size);
        if (!new_item) {
        fail_item:
            desc->key_move_func(NULL, key);
            return NULL;
        }

        new_key = (char *) new_item + key_offs;
        if (!desc->key_move_func(new_key, key)) {
            free(new_item);
            goto fail_item;
        }

        key = new_key;

        new_item->desc = desc;
        new_item->size = 0;
        new_item->hash = hash;
        new_item->last_used_frame = cache->cur_frame;
        new_item->ref_count = 1;
        new_item->queue_next = NULL;
        new_item->queue_prev = NULL;
#if ENABLE_THREADS
        new_item->creating_client = client;
#endif
    }

    new_item->next = start;
    new_item->prev = bucketptr;

    if (!atomic_compare_exchange_weak(bucketptr, &start, new_item))
        goto retry;

    // We won the race; finish inserting our new item
    if (start)
        start->prev = &new_item->next;

    CacheItem *_Atomic *old_last = atomic_exchange_explicit(&cache->queue_last, &new_item->queue_next, memory_order_acq_rel);
    new_item->queue_prev = old_last;
    *old_last = new_item;

    item = new_item;

    void *value = (char *) item + CACHE_ITEM_SIZE;
    size_t size = desc->construct_func(new_key, value, priv);
    assert(size);

    atomic_fetch_add_explicit(&cache->cache_size, size + (size == 1 ? 0 : CACHE_ITEM_SIZE), memory_order_relaxed);

#if ENABLE_THREADS
    pthread_mutex_lock(&client->mutex);
#endif

    item->size = size;

#if ENABLE_THREADS
    pthread_mutex_unlock(&client->mutex);
    pthread_cond_broadcast(&client->cond);
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
    assert(!item->next && !item->prev);
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
    inc_ref(&item->ref_count);
}

static void dec_ref_item(CacheItem *item)
{
    assert(item->size && item->ref_count);
    if (dec_ref(&item->ref_count) == 0)
        destroy_item(item->desc, item);
}

void ass_cache_dec_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    dec_ref_item(item);
}

void ass_cache_cut(Cache *cache, size_t max_size)
{
    for (size_t i = 0; i < cache->n_clients; i++) {
        CacheClient *client = cache->clients[i];
        if (!client)
            continue;

        while (client->promote_first) {
            CacheItem *item = client->promote_first;

            *item->queue_prev = item->queue_next;
            if (item->queue_next)
                item->queue_next->queue_prev = item->queue_prev;
            item->queue_next = NULL;

            item->queue_prev = cache->queue_last;
            *cache->queue_last = item;
            cache->queue_last = &item->queue_next;

            client->promote_first = item->promote_next;
        }
    }

    while (cache->cache_size > max_size && cache->queue_first) {
        CacheItem *item = cache->queue_first;
        assert(item->size);

        if (item->last_used_frame == cache->cur_frame) {
            // everything after this must have been last used this frame
            break;
        }

        if (item->queue_next) {
            item->queue_next->queue_prev = item->queue_prev;
            *item->queue_prev = item->queue_next;
        } else {
            cache->queue_last = item->queue_prev;
        }

        cache->queue_first = item->queue_next;

        if (item->next)
            item->next->prev = item->prev;
        *item->prev = item->next;

        item->next = NULL;
        item->prev = NULL;
        item->queue_prev = NULL;
        item->queue_next = NULL;
        assert(!item->promote_next);

        cache->cache_size -= item->size + (item->size == 1 ? 0 : CACHE_ITEM_SIZE);

        dec_ref_item(item);
    }

    cache->cur_frame++;
}

void ass_cache_empty(Cache *cache)
{
    for (int i = 0; i < cache->buckets; i++) {
        CacheItem *item = cache->map[i];
        while (item) {
            assert(item->size);
            CacheItem *next = item->next;

            item->next = NULL;
            item->prev = NULL;
            item->queue_prev = NULL;
            item->queue_next = NULL;
            item->promote_next = NULL;

            cache->cache_size -= item->size + (item->size == 1 ? 0 : CACHE_ITEM_SIZE);

            dec_ref_item(item);

            item = next;
        }
        cache->map[i] = NULL;
    }

    assert(!cache->cache_size);

    cache->queue_first = NULL;
    cache->queue_last = &cache->queue_first;
    cache->cache_size = 0;

    for (size_t i = 0; i < cache->n_clients; i++) {
        CacheClient *client = cache->clients[i];
        if (client)
            client->promote_first = NULL;
    }
}

void ass_cache_done(Cache *cache)
{
    ass_cache_empty(cache);
    free(cache->map);
    free(cache->clients);
#if ENABLE_THREADS
    pthread_mutex_destroy(&cache->mutex);
#endif
    free(cache);
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

Cache *ass_sized_shaper_font_cache_create(void)
{
    return ass_cache_create(&sized_shaper_font_cache_desc);
}

Cache *ass_bitmap_cache_create(void)
{
    return ass_cache_create(&bitmap_cache_desc);
}

Cache *ass_composite_cache_create(void)
{
    return ass_cache_create(&composite_cache_desc);
}
