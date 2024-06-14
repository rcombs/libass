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

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <limits.h>
#include <ft2build.h>
#include <sys/types.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H

#include "ass_utils.h"
#include "ass.h"
#include "ass_library.h"
#include "ass_filesystem.h"
#include "ass_fontselect.h"
#include "ass_fontconfig.h"
#include "ass_coretext.h"
#include "ass_directwrite.h"
#include "ass_font.h"
#include "ass_string.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MAX_FULLNAME 100

// internal font database element
// all strings are utf-8
struct font_info {
    int uid;            // unique font face id

    // how to access this face
    char *path;            // absolute path
    int index;             // font index inside font collections

    // font source
    ASS_FontProvider *provider;

    // private data for callbacks
    void *priv;

    // Match properties
    ASS_FontProviderMetaData meta;

    // whether attempting to load properties of this font has failed previously
    bool failed;
};

struct font_selector {
    ASS_Library *library;
    FT_Library ftlibrary;

    // uid counter
    int uid;

    // fallbacks
    char *family_default;
    char *path_default;
    int index_default;

    // font database
    int n_font;
    int alloc_font;
    ASS_FontInfo *font_infos;

    ASS_FontProvider *default_provider;
    ASS_FontProvider *embedded_provider;
};

struct font_provider {
    ASS_FontSelector *parent;
    ASS_FontProviderFuncs funcs;
    void *priv;
};

typedef struct font_data_ft FontDataFT;
struct font_data_ft {
    ASS_Library *lib;
    FT_Face face;
    int idx;
};

static bool check_glyph_ft(void *data, uint32_t codepoint)
{
    FontDataFT *fd = (FontDataFT *)data;

    if (!codepoint)
        return true;

    return !!FT_Get_Char_Index(fd->face, codepoint);
}

static void destroy_font_ft(void *data)
{
    FontDataFT *fd = (FontDataFT *)data;

    FT_Done_Face(fd->face);
    free(fd);
}

static size_t
get_data_embedded(void *data, unsigned char *buf, size_t offset, size_t len)
{
    FontDataFT *ft = (FontDataFT *)data;
    ASS_Fontdata *fd = ft->lib->fontdata;
    int i = ft->idx;

    if (buf == NULL)
        return fd[i].size;

    if (offset >= fd[i].size)
        return 0;

    if (len > fd[i].size - offset)
        len = fd[i].size - offset;

    memcpy(buf, fd[i].data + offset, len);
    return len;
}

static ASS_FontProviderFuncs ft_funcs = {
    .get_data          = get_data_embedded,
    .check_glyph       = check_glyph_ft,
    .destroy_font      = destroy_font_ft,
};

static void load_fonts_from_dir(ASS_Library *library, const char *dir)
{
    ASS_Dir d;
    if (!ass_open_dir(&d, dir))
        return;
    while (true) {
        const char *name = ass_read_dir(&d);
        if (!name)
            break;
        if (name[0] == '.')
            continue;
        const char *path = ass_current_file_path(&d);
        if (!path)
            continue;
        ass_msg(library, MSGL_INFO, "Loading font file '%s'", path);
        size_t size = 0;
        void *data = ass_load_file(library, path, FN_DIR_LIST, &size);
        if (data) {
            ass_add_font(library, name, data, size);
            free(data);
        }
    }
    ass_close_dir(&d);
}

/**
 * \brief Create a bare font provider.
 * \param selector parent selector. The provider will be attached to it.
 * \param funcs callback/destroy functions
 * \param data private data of the provider
 * \return the font provider
 */
ASS_FontProvider *
ass_font_provider_new(ASS_FontSelector *selector, ASS_FontProviderFuncs *funcs,
                      void *data)
{
    assert(funcs->check_glyph && funcs->destroy_font);

    ASS_FontProvider *provider = calloc(1, sizeof(ASS_FontProvider));
    if (provider == NULL)
        return NULL;

    provider->parent   = selector;
    provider->funcs    = *funcs;
    provider->priv     = data;

    return provider;
}

/**
 * \brief Read basic metadata (names, weight, slant) from a FreeType face,
 * as required for the FontSelector for matching and sorting.
 * \param lib FreeType library
 * \param face FreeType face
 * \param fallback_family_name family name from outside source, used as last resort
 * \param info metadata, returned here
 * \return success
 */
static bool
get_font_info(FT_Library lib, FT_Face face, const char *fallback_family_name,
              ASS_FontProviderMetaData *info)
{
    int i;
    int num_fullname = 0;
    int num_family   = 0;
    int num_names = FT_Get_Sfnt_Name_Count(face);
    char *fullnames[MAX_FULLNAME];
    char *families[MAX_FULLNAME];

    // we're only interested in outlines
    if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
        return false;

    for (i = 0; i < num_names; i++) {
        FT_SfntName name;

        if (FT_Get_Sfnt_Name(face, i, &name))
            continue;

        if (name.platform_id == TT_PLATFORM_MICROSOFT &&
                (name.name_id == TT_NAME_ID_FULL_NAME ||
                 name.name_id == TT_NAME_ID_FONT_FAMILY)) {
            char buf[1024];
            ass_utf16be_to_utf8(buf, sizeof(buf), (uint8_t *)name.string,
                                name.string_len);

            if (name.name_id == TT_NAME_ID_FULL_NAME && num_fullname < MAX_FULLNAME) {
                fullnames[num_fullname] = strdup(buf);
                if (fullnames[num_fullname] == NULL)
                    goto error;
                num_fullname++;
            }

            if (name.name_id == TT_NAME_ID_FONT_FAMILY && num_family < MAX_FULLNAME) {
                families[num_family] = strdup(buf);
                if (families[num_family] == NULL)
                    goto error;
                num_family++;
            }
        }

    }

    // check if we got a valid family - if not, use
    // whatever the font provider or FreeType gives us
    if (num_family == 0 && (fallback_family_name || face->family_name)) {
        families[0] =
            strdup(fallback_family_name ? fallback_family_name : face->family_name);
        if (families[0] == NULL)
            goto error;
        num_family++;
    }

    // we absolutely need a name
    if (num_family == 0)
        goto error;

    // calculate sensible weight
    info->weight = ass_face_get_weight(face);
    info->style_flags = ass_face_get_style_flags(face);

    info->postscript_name = (char *)FT_Get_Postscript_Name(face);
    info->is_postscript = ass_face_is_postscript(face);

    if (num_family) {
        info->families = calloc(num_family, sizeof(char *));
        if (info->families == NULL)
            goto error;
        memcpy(info->families, &families, sizeof(char *) * num_family);
        info->n_family = num_family;
    }

    if (num_fullname) {
        info->fullnames = calloc(num_fullname, sizeof(char *));
        if (info->fullnames == NULL)
            goto error;
        memcpy(info->fullnames, &fullnames, sizeof(char *) * num_fullname);
    } else {
        info->fullnames = NULL;
    }
    info->n_fullname = num_fullname;

    info->loaded_from_file = true;

    return true;

error:
    for (i = 0; i < num_family; i++)
        free(families[i]);

    for (i = 0; i < num_fullname; i++)
        free(fullnames[i]);

    free(info->families);
    free(info->fullnames);

    info->families = info->fullnames = NULL;
    info->n_family = info->n_fullname = 0;

    return false;
}

/**
 * \brief Free the dynamically allocated fields of metadata
 * created by get_font_info.
 * \param meta metadata created by get_font_info
 */
static void free_font_info(ASS_FontProviderMetaData *meta)
{
    if (meta->families) {
        for (int i = 0; i < meta->n_family; i++)
            free(meta->families[i]);
        free(meta->families);
    }

    if (meta->fullnames) {
        for (int i = 0; i < meta->n_fullname; i++)
            free(meta->fullnames[i]);
        free(meta->fullnames);
    }
}

/**
 * \brief Free all data associated with a FontInfo struct.
 * Handles FontInfo structs with incomplete allocations well.
 *
 * \param info FontInfo struct to free associated data from
 */
static void ass_font_provider_free_fontinfo(ASS_FontInfo *info)
{
    free_font_info(&info->meta);
    free(info->meta.postscript_name);
    free(info->meta.extended_family);

    free(info->path);
}

/**
 * \brief Fill a partly-populated ASS_FontInfo with GDI-compatible metadata.
 * Opens the font file, reads in its metadata, and populates font->meta appropriately.
 * On success, font->meta.loaded_from_file is set to true.
 * On failure, font->failed is set to true and font->meta is unmodified.
 *
 * \param font FontInfo struct to fill
 * \return whether the operation completed successfully
 */
static bool fill_font_info(ASS_FontInfo *font)
{
    int index = font->index;
    ASS_FontProviderMetaData *meta = &font->meta;
    ASS_FontProvider *provider = font->provider;
    ASS_FontSelector *selector = provider->parent;
    FT_Face face;
    bool success = false;

    if (provider->funcs.get_font_index)
        index = provider->funcs.get_font_index(font->priv);

    if (!font->path) {
        assert(provider->funcs.get_data);
        ASS_FontStream stream = {
            .func = provider->funcs.get_data,
            .priv = font->priv,
        };
        // This name is only used in an error message, so use
        // our best name but don't panic if we don't have any.
        // Prefer PostScript name because it is unique.
        const char *name = meta->postscript_name ?
            meta->postscript_name : meta->extended_family;
        face = ass_face_stream(selector->library, selector->ftlibrary,
                               name, &stream, index);
    } else {
        face = ass_face_open(selector->library, selector->ftlibrary,
                             font->path, meta->postscript_name, index);
    }

    if (!face)
        return false;

    ASS_FontProviderMetaData local_meta = *meta;

    if (!get_font_info(selector->ftlibrary, face, meta->extended_family,
                       &local_meta))
        goto cleanup;

    free_font_info(meta);
    free(meta->postscript_name);
    local_meta.postscript_name = local_meta.postscript_name ? strdup(local_meta.postscript_name) : NULL;

    *meta = local_meta;

    success = true;

cleanup:
    FT_Done_Face(face);
    font->failed = !success;
    return success;
}

/**
 * \brief Add a font to a font provider.
 * \param provider the font provider
 * \param meta basic metadata of the font
 * \param path path to the font file, or NULL
 * \param index face index inside the file (-1 to look up by PostScript name)
 * \param data private data for the font
 * \return success
 */
bool
ass_font_provider_add_font(ASS_FontProvider *provider,
                           ASS_FontProviderMetaData *meta, const char *path,
                           int index, void *data)
{
    int i;
    ASS_FontSelector *selector = provider->parent;
    ASS_FontInfo *info = NULL;

    // check size
    if (selector->n_font >= selector->alloc_font) {
        selector->alloc_font = FFMAX(1, 2 * selector->alloc_font);
        selector->font_infos = realloc(selector->font_infos,
                selector->alloc_font * sizeof(ASS_FontInfo));
    }

    // copy over metadata
    info = selector->font_infos + selector->n_font;
    memset(info, 0, sizeof(ASS_FontInfo));

    // set uid
    info->uid = selector->uid++;

    memcpy(&info->meta, meta, sizeof(*meta));

    info->index = index;
    info->priv  = data;
    info->provider = provider;

    info->meta.families = meta->n_family > 0 ? calloc(meta->n_family, sizeof(char *)) : NULL;
    info->meta.fullnames = meta->n_fullname > 0 ? calloc(meta->n_fullname, sizeof(char *)) : NULL;
    info->meta.postscript_name = meta->postscript_name ? strdup(meta->postscript_name) : NULL;
    info->meta.extended_family = meta->extended_family ? strdup(meta->extended_family) : NULL;

    if (meta->n_family > 0 && !info->meta.families)
        goto error;
    if (meta->n_fullname > 0 && !info->meta.fullnames)
        goto error;
    if (meta->postscript_name && !info->meta.postscript_name)
        goto error;
    if (meta->extended_family && !info->meta.extended_family)
        goto error;

    if (path) {
        info->path = strdup(path);
        if (info->path == NULL)
            goto error;
    }

    if (meta->n_family) {
        for (i = 0; i < meta->n_family; i++) {
            info->meta.families[i] = strdup(meta->families[i]);
            if (info->meta.families[i] == NULL)
                goto error;
        }

        for (i = 0; i < meta->n_fullname; i++) {
            info->meta.fullnames[i] = strdup(meta->fullnames[i]);
            if (info->meta.fullnames[i] == NULL)
                goto error;
        }
    } else {
        assert(!meta->n_fullname);

        if (!fill_font_info(info))
            goto error;
    }

    selector->n_font++;

    return true;

error:
    if (info)
        ass_font_provider_free_fontinfo(info);

    provider->funcs.destroy_font(data);

    return false;
}

/**
 * \brief Clean up font database. Deletes all fonts that have an invalid
 * font provider (NULL).
 * \param selector the font selector
 */
static void ass_fontselect_cleanup(ASS_FontSelector *selector)
{
    int i, w;

    for (i = 0, w = 0; i < selector->n_font; i++) {
        ASS_FontInfo *info = selector->font_infos + i;

        // update write pointer
        if (info->provider != NULL) {
            // rewrite, if needed
            if (w != i)
                memcpy(selector->font_infos + w, selector->font_infos + i,
                        sizeof(ASS_FontInfo));
            w++;
        }

    }

    selector->n_font = w;
}

void ass_font_provider_free(ASS_FontProvider *provider)
{
    int i;
    ASS_FontSelector *selector = provider->parent;

    // free all fonts and mark their entries
    for (i = 0; i < selector->n_font; i++) {
        ASS_FontInfo *info = selector->font_infos + i;

        if (info->provider == provider) {
            ass_font_provider_free_fontinfo(info);
            info->provider->funcs.destroy_font(info->priv);
            info->provider = NULL;
        }

    }

    // delete marked entries
    ass_fontselect_cleanup(selector);

    // free private data of the provider
    if (provider->funcs.destroy_provider)
        provider->funcs.destroy_provider(provider->priv);

    free(provider);
}

static bool check_postscript(ASS_FontInfo *fi)
{
    ASS_FontProvider *provider = fi->provider;
    assert(provider);

    if (provider->funcs.check_postscript)
        return provider->funcs.check_postscript(fi->priv);
    else
        return fi->meta.is_postscript;
}

/**
 * \brief Return whether the given font is in the given family.
 */
static bool matches_family_name(ASS_FontInfo *f, const char *family,
                                bool match_extended_family)
{
    for (int i = 0; i < f->meta.n_family; i++) {
        if (ass_strcasecmp(f->meta.families[i], family) == 0)
            return true;
    }
    if (match_extended_family && f->meta.extended_family) {
        if (ass_strcasecmp(f->meta.extended_family, family) == 0)
            return true;
    }
    return false;
}

/**
 * \brief Return whether the given font has the given fullname or
 * PostScript name depending on whether it has PostScript outlines.
 */
static bool matches_full_or_postscript_name(ASS_FontInfo *f,
                                            const char *fullname)
{
    bool matches_fullname = false;
    bool matches_postscript_name = false;

    for (int i = 0; i < f->meta.n_fullname; i++) {
        if (ass_strcasecmp(f->meta.fullnames[i], fullname) == 0) {
            matches_fullname = true;
            break;
        }
    }

    if (f->meta.postscript_name != NULL &&
        ass_strcasecmp(f->meta.postscript_name, fullname) == 0)
        matches_postscript_name = true;

    if (matches_fullname == matches_postscript_name)
        return matches_fullname;

    if (check_postscript(f))
        return matches_postscript_name;
    else
        return matches_fullname;
}

/**
 * \brief Compare attributes of font (a) against a font request (req). Returns
 * a matching score - the lower the better.
 * Ignores font names/families!
 * \param a font
 * \param b font request
 * \return matching score
 */
static unsigned font_attributes_similarity(ASS_FontProviderMetaData *a, ASS_FontProviderMetaData *req)
{
    unsigned score = 0;

    // Assign score for italics mismatch
    if ((req->style_flags & FT_STYLE_FLAG_ITALIC) &&
        !(a->style_flags & FT_STYLE_FLAG_ITALIC))
        score += 1;
    else if (!(req->style_flags & FT_STYLE_FLAG_ITALIC) &&
             (a->style_flags & FT_STYLE_FLAG_ITALIC))
        score += 4;

    int a_weight = a->weight;

    // Offset effective weight for faux-bold (only if font isn't flagged as bold)
    if ((req->weight > a->weight + 150) && !(a->style_flags & FT_STYLE_FLAG_BOLD))
        a_weight += 120;

    // Assign score for weight mismatch
    score += (73 * ABS(a_weight - req->weight)) / 256;

    return score;
}

#if 0
// dump font information
static void font_info_dump(ASS_FontInfo *font_infos, size_t len)
{
    int i, j;

    // dump font infos
    for (i = 0; i < len; i++) {
        printf("font %d\n", i);
        printf("  families: ");
        for (j = 0; j < font_infos[i].n_family; j++)
            printf("'%s' ", font_infos[i].families[j]);
        printf("  fullnames: ");
        for (j = 0; j < font_infos[i].n_fullname; j++)
            printf("'%s' ", font_infos[i].fullnames[j]);
        printf("\n");
        printf("  slant: %d\n", font_infos[i].slant);
        printf("  weight: %d\n", font_infos[i].weight);
        printf("  path: %s\n", font_infos[i].path);
        printf("  index: %d\n", font_infos[i].index);
        printf("  score: %d\n", font_infos[i].score);

    }
}
#endif

static bool check_glyph(ASS_FontInfo *fi, uint32_t code)
{
    ASS_FontProvider *provider = fi->provider;
    assert(provider && provider->funcs.check_glyph);

    return provider->funcs.check_glyph(fi->priv, code);
}

static char *
find_font(ASS_FontSelector *priv,
          ASS_FontProviderMetaData meta, bool match_extended_family,
          unsigned bold, unsigned italic,
          int *index, char **postscript_name, int *uid, ASS_FontStream *stream,
          uint32_t code, bool *name_match)
{
    ASS_FontProviderMetaData req = {0};
    ASS_FontInfo *selected = NULL;

    // do we actually have any fonts?
    if (!priv->n_font)
        return NULL;

    // fill font request
    req.style_flags = (italic ? FT_STYLE_FLAG_ITALIC : 0);
    req.weight      = bold;

    // Match font family name against font list
    unsigned score_min = UINT_MAX;
    for (int i = 0; i < meta.n_fullname; i++) {
        const char *fullname = meta.fullnames[i];

        for (int x = 0; x < priv->n_font; x++) {
            ASS_FontInfo *font = &priv->font_infos[x];
            bool do_score;

            if (font->failed)
                continue;

retry:
            if (matches_family_name(font, fullname, match_extended_family)) {
                // If there's a family match, compare font attributes
                // to determine best match in that particular family
                do_score = true;
            } else if (matches_full_or_postscript_name(font, fullname)) {
                // If we don't have any match, compare fullnames against request
                // if there is a match now, assign lowest score possible. This means
                // the font should be chosen instantly, without further search.
                do_score = false;
            } else {
                continue;
            }

            if (!font->meta.loaded_from_file) {
                if (!fill_font_info(font))
                    continue;
                goto retry;
            }

            *name_match = true;
            unsigned score = do_score ? font_attributes_similarity(&font->meta, &req) : 0;

            // Consider updating idx if score is better than current minimum
            if (score < score_min) {
                // Check if the font has the requested glyph.
                // We are doing this here, for every font face, because
                // coverage might differ between the variants of a font
                // family. In practice, it is common that the regular
                // style has the best coverage while bold/italic/etc
                // variants cover less (e.g. FreeSans family).
                // We want to be able to match even if the closest variant
                // does not have the requested glyph, but another member
                // of the family has the glyph.
                if (!check_glyph(font, code))
                    continue;

                score_min = score;
                selected = font;
            }

            // Lowest possible score instantly matches; this is typical
            // for fullname matches, but can also occur with family matches.
            if (score == 0)
                break;
        }

        // The list of names is sorted by priority. If we matched anything,
        // we can and should stop.
        if (selected != NULL)
            break;
    }

    // found anything?
    char *result = NULL;
    if (selected) {
        ASS_FontProvider *provider = selected->provider;

        // successfully matched, set up return values
        *postscript_name = selected->meta.postscript_name;
        *uid   = selected->uid;

        // use lazy evaluation for index if applicable
        if (provider->funcs.get_font_index) {
            *index = provider->funcs.get_font_index(selected->priv);
        } else
            *index = selected->index;

        // set up memory stream if there is no path
        if (selected->path == NULL) {
            stream->func = provider->funcs.get_data;
            stream->priv = selected->priv;
            // Prefer PostScript name because it is unique. This is only
            // used for display purposes so it doesn't matter that much,
            // though.
            if (selected->meta.postscript_name)
                result = selected->meta.postscript_name;
            else
                result = selected->meta.families[0];
        } else
            result = selected->path;

    }

    return result;
}

static char *select_font(ASS_FontSelector *priv,
                         const char *family, bool match_extended_family,
                         unsigned bold, unsigned italic,
                         int *index, char **postscript_name, int *uid,
                         ASS_FontStream *stream, uint32_t code)
{
    ASS_FontProvider *default_provider = priv->default_provider;
    ASS_FontProviderMetaData meta = {0};
    char *result = NULL;
    bool name_match = false;

    if (family == NULL)
        return NULL;

    ASS_FontProviderMetaData default_meta = {
        .n_fullname = 1,
        .fullnames  = (char **)&family,
    };

    // Get a list of substitutes if applicable, and use it for matching.
    if (default_provider && default_provider->funcs.get_substitutions) {
        default_provider->funcs.get_substitutions(default_provider->priv,
                                                  family, &meta);
    }

    if (!meta.n_fullname) {
        free(meta.fullnames);
        meta = default_meta;
    }

    result = find_font(priv, meta, match_extended_family,
                       bold, italic, index, postscript_name, uid,
                       stream, code, &name_match);

    // If no matching font was found, it might not exist in the font list
    // yet. Call the match_fonts callback to fill in the missing fonts
    // on demand, and retry the search for a match.
    if (result == NULL && name_match == false && default_provider &&
            default_provider->funcs.match_fonts) {
        // TODO: consider changing the API to make more efficient
        // implementations possible.
        for (int i = 0; i < meta.n_fullname; i++) {
            default_provider->funcs.match_fonts(default_provider->priv,
                                                priv->library, default_provider,
                                                meta.fullnames[i]);
        }
        result = find_font(priv, meta, match_extended_family,
                           bold, italic, index, postscript_name, uid,
                           stream, code, &name_match);
    }

    // cleanup
    if (meta.fullnames != default_meta.fullnames) {
        for (int i = 0; i < meta.n_fullname; i++)
            free(meta.fullnames[i]);
        free(meta.fullnames);
    }

    return result;
}


/**
 * \brief Find a font. Use default family or path if necessary.
 * \param family font family
 * \param treat_family_as_pattern treat family as fontconfig pattern
 * \param bold font weight value
 * \param italic font slant value
 * \param index out: font index inside a file
 * \param code: the character that should be present in the font, can be 0
 * \return font file path
*/
char *ass_font_select(ASS_FontSelector *priv,
                      const ASS_Font *font, int *index, char **postscript_name,
                      int *uid, ASS_FontStream *data, uint32_t code)
{
    char *res = 0;
    const char *family = font->desc.family.str;  // always zero-terminated
    unsigned bold = font->desc.bold;
    unsigned italic = font->desc.italic;
    ASS_FontProvider *default_provider = priv->default_provider;

    if (family && *family)
        res = select_font(priv, family, false, bold, italic, index,
                postscript_name, uid, data, code);

    if (!res && priv->family_default) {
        res = select_font(priv, priv->family_default, false, bold,
                italic, index, postscript_name, uid, data, code);
        if (res)
            ass_msg(priv->library, MSGL_WARN, "fontselect: Using default "
                    "font family: (%s, %d, %d) -> %s, %d, %s",
                    family, bold, italic, res, *index,
                    *postscript_name ? *postscript_name : "(none)");
    }

    if (!res && default_provider && default_provider->funcs.get_fallback) {
        const char *search_family = family;
        if (!search_family || !*search_family)
            search_family = "Arial";
        char *fallback_family = default_provider->funcs.get_fallback(
                default_provider->priv, priv->library, search_family, code);

        if (fallback_family) {
            res = select_font(priv, fallback_family, true, bold, italic,
                    index, postscript_name, uid, data, code);
            free(fallback_family);
        }
    }

    if (!res && priv->path_default) {
        res = priv->path_default;
        *index = priv->index_default;
        ass_msg(priv->library, MSGL_WARN, "fontselect: Using default font: "
                "(%s, %d, %d) -> %s, %d, %s", family, bold, italic,
                priv->path_default, *index,
                *postscript_name ? *postscript_name : "(none)");
    }

    if (res)
        ass_msg(priv->library, MSGL_INFO,
                "fontselect: (%s, %d, %d) -> %s, %d, %s", family, bold,
                italic, res, *index, *postscript_name ? *postscript_name : "(none)");
    else
        ass_msg(priv->library, MSGL_WARN,
                "fontselect: failed to find any fallback with glyph 0x%X for font: "
                "(%s, %d, %d)", code, family, bold, italic);

    return res;
}


/**
 * \brief Process memory font.
 * \param priv private data
 * \param idx index of the processed font in priv->library->fontdata
 *
 * Builds a FontInfo with FreeType and some table reading.
*/
static void process_fontdata(ASS_FontProvider *priv, int idx)
{
    ASS_FontSelector *selector = priv->parent;
    ASS_Library *library = selector->library;

    int rc;
    const char *name = library->fontdata[idx].name;
    const char *data = library->fontdata[idx].data;
    int data_size = library->fontdata[idx].size;

    FT_Face face;
    int face_index, num_faces = 1;

    for (face_index = 0; face_index < num_faces; ++face_index) {
        ASS_FontProviderMetaData info;
        FontDataFT *ft;

        rc = FT_New_Memory_Face(selector->ftlibrary, (unsigned char *) data,
                                data_size, face_index, &face);
        if (rc) {
            ass_msg(library, MSGL_WARN, "Error opening memory font '%s'",
                   name);
            continue;
        }

        num_faces = face->num_faces;

        ass_charmap_magic(library, face);

        memset(&info, 0, sizeof(ASS_FontProviderMetaData));
        if (!get_font_info(selector->ftlibrary, face, NULL, &info)) {
            ass_msg(library, MSGL_WARN,
                    "Error getting metadata for embedded font '%s'", name);
            FT_Done_Face(face);
            continue;
        }

        ft = calloc(1, sizeof(FontDataFT));

        if (ft == NULL) {
            free_font_info(&info);
            FT_Done_Face(face);
            continue;
        }

        ft->lib  = library;
        ft->face = face;
        ft->idx  = idx;

        if (!ass_font_provider_add_font(priv, &info, NULL, face_index, ft)) {
            ass_msg(library, MSGL_WARN, "Failed to add embedded font '%s'",
                    name);
            free(ft);
        }

        free_font_info(&info);
    }
}

/**
 * \brief Create font provider for embedded fonts. This parses the fonts known
 * to the current ASS_Library and adds them to the selector.
 * \param selector font selector
 * \return font provider
 */
static ASS_FontProvider *
ass_embedded_fonts_add_provider(ASS_FontSelector *selector, size_t *num_emfonts)
{
    ASS_FontProvider *priv = ass_font_provider_new(selector, &ft_funcs, NULL);
    if (priv == NULL)
        return NULL;

    ASS_Library *lib = selector->library;

    if (lib->fonts_dir && lib->fonts_dir[0]) {
        load_fonts_from_dir(lib, lib->fonts_dir);
    }

    for (size_t i = 0; i < lib->num_fontdata; i++)
        process_fontdata(priv, i);
    *num_emfonts = lib->num_fontdata;

    return priv;
}

struct font_constructors {
    ASS_DefaultFontProvider id;
    ASS_FontProvider *(*constructor)(ASS_Library *, ASS_FontSelector *,
                                     const char *, FT_Library);
    const char *name;
};

struct font_constructors font_constructors[] = {
#ifdef CONFIG_CORETEXT
    { ASS_FONTPROVIDER_CORETEXT,        &ass_coretext_add_provider,     "coretext"},
#endif
#ifdef CONFIG_DIRECTWRITE
    { ASS_FONTPROVIDER_DIRECTWRITE,     &ass_directwrite_add_provider,  "directwrite"
#if ASS_WINAPI_DESKTOP
        " (with GDI)"
#else
        " (without GDI)"
#endif
    },
#endif
#ifdef CONFIG_FONTCONFIG
    { ASS_FONTPROVIDER_FONTCONFIG,      &ass_fontconfig_add_provider,   "fontconfig"},
#endif
    { ASS_FONTPROVIDER_NONE, NULL, NULL },
};

/**
 * \brief Init font selector.
 * \param library libass library object
 * \param ftlibrary freetype library object
 * \param family default font family
 * \param path default font path
 * \return newly created font selector
 */
ASS_FontSelector *
ass_fontselect_init(ASS_Library *library, FT_Library ftlibrary, size_t *num_emfonts,
                    const char *family, const char *path, const char *config,
                    ASS_DefaultFontProvider dfp)
{
    ASS_FontSelector *priv = calloc(1, sizeof(ASS_FontSelector));
    if (priv == NULL)
        return NULL;

    priv->library = library;
    priv->ftlibrary = ftlibrary;
    priv->uid = 1;
    priv->family_default = family ? strdup(family) : NULL;
    priv->path_default = path ? strdup(path) : NULL;
    priv->index_default = 0;

    if (family && !priv->family_default)
        goto fail;
    if (path && !priv->path_default)
        goto fail;

    priv->embedded_provider = ass_embedded_fonts_add_provider(priv, num_emfonts);

    if (priv->embedded_provider == NULL) {
        ass_msg(library, MSGL_WARN, "failed to create embedded font provider");
        goto fail;
    }

    if (dfp >= ASS_FONTPROVIDER_AUTODETECT) {
        for (int i = 0; font_constructors[i].constructor; i++ )
            if (dfp == font_constructors[i].id ||
                dfp == ASS_FONTPROVIDER_AUTODETECT) {
                priv->default_provider =
                    font_constructors[i].constructor(library, priv,
                                                     config, ftlibrary);
                if (priv->default_provider) {
                    ass_msg(library, MSGL_INFO, "Using font provider %s",
                            font_constructors[i].name);
                    break;
                }
            }

        if (!priv->default_provider)
            ass_msg(library, MSGL_WARN, "can't find selected font provider");

    }

    return priv;

fail:
    if (priv->default_provider)
        ass_font_provider_free(priv->default_provider);
    if (priv->embedded_provider)
        ass_font_provider_free(priv->embedded_provider);

    free(priv->family_default);
    free(priv->path_default);

    free(priv);

    return NULL;
}

void ass_get_available_font_providers(ASS_Library *priv,
                                      ASS_DefaultFontProvider **providers,
                                      size_t *size)
{
    size_t offset = 2;

    *size = offset;
    for (int i = 0; font_constructors[i].constructor; i++)
        (*size)++;

    *providers = calloc(*size, sizeof(ASS_DefaultFontProvider));

    if (*providers == NULL) {
        *size = (size_t)-1;
        return;
    }

    (*providers)[0] = ASS_FONTPROVIDER_NONE;
    (*providers)[1] = ASS_FONTPROVIDER_AUTODETECT;

    for (int i = offset; i < *size; i++)
        (*providers)[i] = font_constructors[i-offset].id;
}

/**
 * \brief Free font selector and release associated data
 * \param the font selector
 */
void ass_fontselect_free(ASS_FontSelector *priv)
{
    if (priv->default_provider)
        ass_font_provider_free(priv->default_provider);
    if (priv->embedded_provider)
        ass_font_provider_free(priv->embedded_provider);

    free(priv->font_infos);
    free(priv->path_default);
    free(priv->family_default);

    free(priv);
}

void ass_map_font(const ASS_FontMapping *map, int len, const char *name,
                  ASS_FontProviderMetaData *meta)
{
    for (int i = 0; i < len; i++) {
        if (ass_strcasecmp(map[i].from, name) == 0) {
            meta->fullnames = calloc(1, sizeof(char *));
            if (meta->fullnames) {
                meta->fullnames[0] = strdup(map[i].to);
                if (meta->fullnames[0])
                    meta->n_fullname = 1;
            }
            return;
        }
    }
}

size_t ass_update_embedded_fonts(ASS_FontSelector *selector, size_t num_loaded)
{
    if (!selector->embedded_provider)
        return num_loaded;

    size_t num_fontdata = selector->library->num_fontdata;
    for (size_t i = num_loaded; i < num_fontdata; i++)
        process_fontdata(selector->embedded_provider, i);
    return num_fontdata;
}
