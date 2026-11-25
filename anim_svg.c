/*
 * anim_svg.c — SVG parser, stb_truetype plain text, Typst/LaTeX math & mu_save cache.
 * Follows mu_lib/AGENTS.md conventions (C11, aligned block declarations, no hot malloc).
 */
#include "anim_internal.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb/stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "third_party/stb/stb_image.h"

#include <ctype.h>
#include <sys/stat.h>

/* ----------------------------------------------------- font file slot cache */

typedef struct CachedFont {
    bool            loaded;
    char            path[ANIM_PATH_LEN];
    uint8_t        *data;
    size_t          size;
    stbtt_fontinfo  info;
} CachedFont;

static CachedFont g_font_cache[16];

static const char *default_font_path(void)
{
    static const char *const CANDIDATES[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    uint32_t i;

    for (i = 0u; i < (uint32_t)MU_ARRAY_COUNT(CANDIDATES); ++i)
    {
        if (mu_files_path_exists(CANDIDATES[i]))
            return CANDIDATES[i];
    }
    return CANDIDATES[0];
}

static const CachedFont *load_cached_font_exact(const char *path)
{
    uint32_t i;
    void    *bytes = NULL;
    size_t   sz    = 0u;

    for (i = 0u; i < (uint32_t)MU_ARRAY_COUNT(g_font_cache); ++i)
    {
        if (g_font_cache[i].loaded && strcmp(g_font_cache[i].path, path) == 0)
            return &g_font_cache[i];
    }

    if (!mu_files_read_all(path, &bytes, &sz))
        return NULL;

    for (i = 0u; i < (uint32_t)MU_ARRAY_COUNT(g_font_cache); ++i)
    {
        if (!g_font_cache[i].loaded)
        {
            g_font_cache[i].data = (uint8_t *)bytes;
            g_font_cache[i].size = sz;
            snprintf(g_font_cache[i].path, sizeof g_font_cache[i].path, "%s", path);
            if (!stbtt_InitFont(&g_font_cache[i].info, g_font_cache[i].data,
                                stbtt_GetFontOffsetForIndex(g_font_cache[i].data, 0)))
            {
                free(bytes);
                return NULL;
            }
            g_font_cache[i].loaded = true;
            return &g_font_cache[i];
        }
    }
    free(bytes);
    return NULL;
}

static const CachedFont *load_cached_font(const char *font_path)
{
    const char       *def  = default_font_path();
    const char       *path = (font_path && font_path[0]) ? font_path : def;
    const CachedFont *cf   = load_cached_font_exact(path);

    if (!cf && strcmp(path, def) != 0)
    {
        anim__set_error("anim_text: failed to read font %s (using fallback %s)", path, def);
        cf = load_cached_font_exact(def);
    }
    else if (!cf)
    {
        anim__set_error("anim_text: failed to read font %s", path);
    }
    return cf;
}

static uint32_t utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t       cp;

    if (s[0] < 0x80u)
    {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u)
    {
        cp  = ((uint32_t)(s[0] & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
        *p += 2;
        return cp;
    }
    if ((s[0] & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u)
    {
        cp  = ((uint32_t)(s[0] & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
        *p += 3;
        return cp;
    }
    if ((s[0] & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u)
    {
        cp  = ((uint32_t)(s[0] & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
        *p += 4;
        return cp;
    }
    *p += 1;
    return 0xFFFDu;
}

AnimId anim_text(AnimScene *s, const char *font_path, const char *utf8, float world_height)
{
    const CachedFont *cf    = load_cached_font(font_path);
    AnimId            grp   = anim_group(s, NULL, 0u);
    int               ascent, descent, line_gap;
    float             scale;
    float             pen_x = 0.0f;
    float             pen_y = 0.0f;
    uint32_t          prev_cp = 0u;
    const char       *p     = utf8 ? utf8 : "";

    if (!cf || !ANIM_ID_VALID(grp))
        return grp;

    stbtt_GetFontVMetrics(&cf->info, &ascent, &descent, &line_gap);
    scale = (world_height > 0.0f ? world_height : 0.6f) / (float)MU_MAX(ascent - descent, 1);

    while (*p)
    {
        uint32_t       cp     = utf8_next(&p);
        int            adv    = 0;
        int            lsb    = 0;
        stbtt_vertex  *verts  = NULL;
        int            nverts;
        AnimId         glyph;
        int            i;

        if (cp == '\n')
        {
            pen_x   = 0.0f;
            pen_y  -= (float)(ascent - descent + line_gap) * scale * 1.2f;
            prev_cp = 0u;
            continue;
        }
        if (prev_cp != 0u)
            pen_x += (float)stbtt_GetCodepointKernAdvance(&cf->info, (int)prev_cp, (int)cp) * scale;

        stbtt_GetCodepointHMetrics(&cf->info, (int)cp, &adv, &lsb);
        nverts = stbtt_GetCodepointShape(&cf->info, (int)cp, &verts);

        if (nverts > 0 && cp != ' ' && cp != '\t')
        {
            bool has_open = false;
            glyph = anim_vmob_create(s);
            for (i = 0; i < nverts; ++i)
            {
                stbtt_vertex v  = verts[i];
                float        px = pen_x + (float)v.x * scale;
                float        py = pen_y + (float)v.y * scale;

                if (v.type == STBTT_vmove)
                {
                    if (has_open)
                        anim_vmob_close_subpath(s, glyph);
                    anim_vmob_begin_subpath(s, glyph, ANIM_V3(px, py, 0.0f));
                    has_open = true;
                }
                else if (v.type == STBTT_vline)
                {
                    anim_vmob_line_to(s, glyph, ANIM_V3(px, py, 0.0f));
                }
                else if (v.type == STBTT_vcurve)
                {
                    float cx = pen_x + (float)v.cx * scale;
                    float cy = pen_y + (float)v.cy * scale;
                    anim_vmob_quad_to(s, glyph, ANIM_V3(cx, cy, 0.0f), ANIM_V3(px, py, 0.0f));
                }
                else if (v.type == STBTT_vcubic)
                {
                    float cx1 = pen_x + (float)v.cx * scale;
                    float cy1 = pen_y + (float)v.cy * scale;
                    float cx2 = pen_x + (float)v.cx1 * scale;
                    float cy2 = pen_y + (float)v.cy1 * scale;
                    anim_vmob_cubic_to(s, glyph, ANIM_V3(cx1, cy1, 0.0f), ANIM_V3(cx2, cy2, 0.0f), ANIM_V3(px, py, 0.0f));
                }
            }
            if (has_open)
                anim_vmob_close_subpath(s, glyph);
            anim_set_fill(s, glyph, ANIM_WHITE, false);
            anim_set_stroke(s, glyph, ANIM_WHITE, 0.35f, false);
            anim_add_child(s, grp, glyph);
        }
        if (verts)
            stbtt_FreeShape(&cf->info, verts);

        pen_x  += (float)adv * scale;
        prev_cp = cp;
    }

    anim_move_to(s, grp, ANIM_ORIGIN);
    return grp;
}

typedef struct MarkupStyle {
    AnimColor color;
    float     size_mul;
    float     y_shift;
    bool      bold;
    bool      italic;
    bool      underline;
    bool      strikethrough;
    char      tag[ANIM_TAG_LEN];
    float     span_x0;
} MarkupStyle;

static AnimColor markup_parse_color(const char *val, AnimColor fallback)
{
    if (!val || !val[0]) return fallback;
    if (val[0] == '#')
    {
        unsigned int h = 0u;
        size_t len = strlen(val + 1);
        if (sscanf(val + 1, "%x", &h) == 1)
        {
            if (len == 8u)
                return anim_hex_a((h >> 8) & 0xFFFFFFu, (float)(h & 0xFFu) / 255.0f);
            return anim_hex(h & 0xFFFFFFu);
        }
    }
    if (strcmp(val, "red") == 0)    return ANIM_RED;
    if (strcmp(val, "blue") == 0)   return ANIM_BLUE;
    if (strcmp(val, "green") == 0)  return ANIM_GREEN;
    if (strcmp(val, "yellow") == 0) return ANIM_YELLOW;
    if (strcmp(val, "gold") == 0)   return ANIM_GOLD;
    if (strcmp(val, "teal") == 0)   return ANIM_TEAL;
    if (strcmp(val, "purple") == 0) return ANIM_PURPLE;
    if (strcmp(val, "pink") == 0)   return ANIM_PINK;
    if (strcmp(val, "maroon") == 0) return ANIM_MAROON;
    if (strcmp(val, "white") == 0)  return ANIM_WHITE;
    if (strcmp(val, "grey") == 0 || strcmp(val, "gray") == 0) return ANIM_GREY_B;
    if (strcmp(val, "black") == 0)  return ANIM_BLACK;
    return fallback;
}

static bool markup_read_attr(const char *tag_s, size_t tag_len, const char *attr, char *out, size_t out_sz)
{
    size_t alen = strlen(attr);
    size_t i = 0u;
    out[0] = '\0';
    while (i + alen + 2u < tag_len)
    {
        if ((i == 0u || isspace((unsigned char)tag_s[i - 1u])) &&
            memcmp(tag_s + i, attr, alen) == 0 && tag_s[i + alen] == '=')
        {
            size_t j = i + alen + 1u;
            char q = (tag_s[j] == '"' || tag_s[j] == '\'') ? tag_s[j++] : '\0';
            size_t k = 0u;
            while (j < tag_len && k + 1u < out_sz)
            {
                if (q ? (tag_s[j] == q) : isspace((unsigned char)tag_s[j]))
                    break;
                out[k++] = tag_s[j++];
            }
            out[k] = '\0';
            return true;
        }
        i++;
    }
    return false;
}

AnimId anim_markup(AnimScene *s, const char *font_path, const char *markup_utf8, float world_height)
{
    const CachedFont *cf    = load_cached_font(font_path);
    AnimId            grp   = anim_group(s, NULL, 0u);
    int               ascent, descent, line_gap;
    float             wh    = (world_height > 0.0f) ? world_height : 0.6f;
    float             base_scale;
    float             pen_x = 0.0f;
    float             pen_y = 0.0f;
    uint32_t          prev_cp = 0u;
    const char       *p     = markup_utf8 ? markup_utf8 : "";
    MarkupStyle       stk[16];
    int               sp    = 0;

    if (!cf || !ANIM_ID_VALID(grp))
        return grp;

    stbtt_GetFontVMetrics(&cf->info, &ascent, &descent, &line_gap);
    base_scale = wh / (float)MU_MAX(ascent - descent, 1);

    memset(&stk[0], 0, sizeof stk[0]);
    stk[0].color    = ANIM_WHITE;
    stk[0].size_mul = 1.0f;

    while (*p)
    {
        if (*p == '<')
        {
            const char *gt = strchr(p + 1, '>');
            if (gt)
            {
                const char *ts   = p + 1;
                size_t      tlen = (size_t)(gt - ts);
                bool        is_close = (tlen > 0u && ts[0] == '/');
                if (is_close)
                {
                    if (sp > 0)
                    {
                        MarkupStyle popped = stk[sp];
                        if (popped.underline && pen_x > popped.span_x0 + 1e-4f)
                        {
                            float uy = pen_y + popped.y_shift - wh * 0.10f;
                            AnimId ul = anim_line(s, ANIM_V3(popped.span_x0, uy, 0.0f), ANIM_V3(pen_x, uy, 0.0f));
                            anim_set_stroke(s, ul, popped.color, 2.2f * popped.size_mul, false);
                            anim_add_child(s, grp, ul);
                        }
                        if (popped.strikethrough && pen_x > popped.span_x0 + 1e-4f)
                        {
                            float sy = pen_y + popped.y_shift + wh * 0.28f * popped.size_mul;
                            AnimId sl = anim_line(s, ANIM_V3(popped.span_x0, sy, 0.0f), ANIM_V3(pen_x, sy, 0.0f));
                            anim_set_stroke(s, sl, popped.color, 2.2f * popped.size_mul, false);
                            anim_add_child(s, grp, sl);
                        }
                        sp--;
                    }
                }
                else if (sp < 15)
                {
                    MarkupStyle nxt = stk[sp];
                    char attr[ANIM_TAG_LEN];
                    nxt.span_x0 = pen_x;
                    if ((tlen == 1u && (ts[0] == 'b' || ts[0] == 'B')) ||
                        (tlen >= 6u && memcmp(ts, "strong", 6u) == 0))
                        nxt.bold = true;
                    else if ((tlen == 1u && (ts[0] == 'i' || ts[0] == 'I')) ||
                             (tlen >= 2u && memcmp(ts, "em", 2u) == 0))
                        nxt.italic = true;
                    else if (tlen == 1u && (ts[0] == 'u' || ts[0] == 'U'))
                        nxt.underline = true;
                    else if (tlen == 1u && (ts[0] == 's' || ts[0] == 'S'))
                        nxt.strikethrough = true;
                    else if (tlen >= 3u && memcmp(ts, "sup", 3u) == 0)
                    {
                        nxt.size_mul *= 0.65f;
                        nxt.y_shift  += wh * 0.40f;
                    }
                    else if (tlen >= 3u && memcmp(ts, "sub", 3u) == 0)
                    {
                        nxt.size_mul *= 0.65f;
                        nxt.y_shift  -= wh * 0.18f;
                    }
                    if (markup_read_attr(ts, tlen, "color", attr, sizeof attr) ||
                        markup_read_attr(ts, tlen, "fgcolor", attr, sizeof attr))
                        nxt.color = markup_parse_color(attr, nxt.color);
                    if (markup_read_attr(ts, tlen, "size", attr, sizeof attr))
                    {
                        float mul = (float)atof(attr);
                        if (mul > 0.05f && mul < 10.0f) nxt.size_mul *= mul;
                    }
                    if (markup_read_attr(ts, tlen, "weight", attr, sizeof attr) && strcmp(attr, "bold") == 0)
                        nxt.bold = true;
                    if (markup_read_attr(ts, tlen, "style", attr, sizeof attr) && strcmp(attr, "italic") == 0)
                        nxt.italic = true;
                    if (markup_read_attr(ts, tlen, "id", attr, sizeof attr))
                        snprintf(nxt.tag, sizeof nxt.tag, "%s", attr);
                    stk[++sp] = nxt;
                }
                p = gt + 1;
                continue;
            }
        }

        uint32_t cp = 0u;
        if (*p == '&')
        {
            if (strncmp(p, "&lt;", 4u) == 0) { cp = '<'; p += 4; }
            else if (strncmp(p, "&gt;", 4u) == 0) { cp = '>'; p += 4; }
            else if (strncmp(p, "&amp;", 5u) == 0) { cp = '&'; p += 5; }
            else if (strncmp(p, "&quot;", 6u) == 0) { cp = '"'; p += 6; }
            else if (strncmp(p, "&apos;", 6u) == 0) { cp = '\''; p += 6; }
        }
        if (cp == 0u)
            cp = utf8_next(&p);

        if (cp == '\n')
        {
            pen_x   = 0.0f;
            pen_y  -= (float)(ascent - descent + line_gap) * base_scale * 1.25f;
            prev_cp = 0u;
            continue;
        }

        {
            const MarkupStyle *st = &stk[sp];
            float scale = base_scale * st->size_mul;
            float base_y = pen_y + st->y_shift;
            float slant  = st->italic ? 0.22f : 0.0f;
            int   adv = 0, lsb = 0, nverts, i;
            stbtt_vertex *verts = NULL;

            if (prev_cp != 0u)
                pen_x += (float)stbtt_GetCodepointKernAdvance(&cf->info, (int)prev_cp, (int)cp) * scale;
            stbtt_GetCodepointHMetrics(&cf->info, (int)cp, &adv, &lsb);
            nverts = stbtt_GetCodepointShape(&cf->info, (int)cp, &verts);

            if (nverts > 0 && cp != ' ' && cp != '\t')
            {
                bool   has_open = false;
                AnimId glyph    = anim_vmob_create(s);
                for (i = 0; i < nverts; ++i)
                {
                    stbtt_vertex v  = verts[i];
                    float        ly = (float)v.y * scale;
                    float        px = pen_x + (float)v.x * scale + slant * ly;
                    float        py = base_y + ly;
                    if (v.type == STBTT_vmove)
                    {
                        if (has_open) anim_vmob_close_subpath(s, glyph);
                        anim_vmob_begin_subpath(s, glyph, ANIM_V3(px, py, 0.0f));
                        has_open = true;
                    }
                    else if (v.type == STBTT_vline)
                        anim_vmob_line_to(s, glyph, ANIM_V3(px, py, 0.0f));
                    else if (v.type == STBTT_vcurve)
                    {
                        float cy_l = (float)v.cy * scale;
                        float cx   = pen_x + (float)v.cx * scale + slant * cy_l;
                        float cy   = base_y + cy_l;
                        anim_vmob_quad_to(s, glyph, ANIM_V3(cx, cy, 0.0f), ANIM_V3(px, py, 0.0f));
                    }
                    else if (v.type == STBTT_vcubic)
                    {
                        float cy1_l = (float)v.cy * scale, cy2_l = (float)v.cy1 * scale;
                        float cx1 = pen_x + (float)v.cx * scale + slant * cy1_l;
                        float cx2 = pen_x + (float)v.cx1 * scale + slant * cy2_l;
                        anim_vmob_cubic_to(s, glyph, ANIM_V3(cx1, base_y + cy1_l, 0.0f), ANIM_V3(cx2, base_y + cy2_l, 0.0f), ANIM_V3(px, py, 0.0f));
                    }
                }
                if (has_open) anim_vmob_close_subpath(s, glyph);
                anim_set_fill(s, glyph, st->color, false);
                anim_set_stroke(s, glyph, st->color, st->bold ? 1.4f : 0.35f, false);
                if (st->tag[0]) anim_set_tag(s, glyph, st->tag);
                anim_add_child(s, grp, glyph);
            }
            if (verts) stbtt_FreeShape(&cf->info, verts);
            pen_x  += (float)adv * scale;
            prev_cp = cp;
        }
    }

    anim_move_to(s, grp, ANIM_ORIGIN);
    return grp;
}

void anim_text_set_t2c(AnimScene *s, AnimId text_id, const char *utf8_src, const char *substring, AnimColor color)
{
    uint32_t    nchild;
    size_t      sub_len;
    const char *scan;

    if (!s || !ANIM_ID_VALID(text_id) || !utf8_src || !substring || !substring[0])
        return;
    nchild  = anim_child_count(s, text_id);
    sub_len = strlen(substring);
    scan    = utf8_src;

    while ((scan = strstr(scan, substring)) != NULL)
    {
        uint32_t    g_start = 0u, g_count = 0u;
        const char *p = utf8_src;
        const char *sub_end = scan + sub_len;
        while (p < scan && *p)
        {
            uint32_t cp = utf8_next(&p);
            if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r')
                g_start++;
        }
        while (p < sub_end && *p)
        {
            uint32_t cp = utf8_next(&p);
            if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r')
                g_count++;
        }
        for (uint32_t k = 0u; k < g_count; ++k)
        {
            if (g_start + k < nchild)
                anim_set_color(s, anim_child_at(s, text_id, g_start + k), color, true);
        }
        scan += sub_len;
    }
}

/* --------------------------------------------------------------- SVG parser */

typedef struct SvgXform {
    float m[6]; /* affine [a b c d e f]: x' = a*x + c*y + e, y' = b*x + d*y + f */
} SvgXform;

static SvgXform xform_id(void)
{
    return (SvgXform){ { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f } };
}

static SvgXform xform_mul(SvgXform p, SvgXform c)
{
    return (SvgXform){ {
        p.m[0] * c.m[0] + p.m[2] * c.m[1],
        p.m[1] * c.m[0] + p.m[3] * c.m[1],
        p.m[0] * c.m[2] + p.m[2] * c.m[3],
        p.m[1] * c.m[2] + p.m[3] * c.m[3],
        p.m[0] * c.m[4] + p.m[2] * c.m[5] + p.m[4],
        p.m[1] * c.m[4] + p.m[3] * c.m[5] + p.m[5],
    } };
}

static AnimVec3 xform_pt(SvgXform xf, float x, float y)
{
    return ANIM_V3(xf.m[0] * x + xf.m[2] * y + xf.m[4], -(xf.m[1] * x + xf.m[3] * y + xf.m[5]), 0.0f);
}

static float xform_scale_mag(SvgXform xf)
{
    float sx = sqrtf(xf.m[0] * xf.m[0] + xf.m[1] * xf.m[1]);
    float sy = sqrtf(xf.m[2] * xf.m[2] + xf.m[3] * xf.m[3]);
    return 0.5f * (sx + sy);
}

typedef struct SvgStyle {
    bool      has_fill;
    bool      fill_none;
    AnimColor fill;
    bool      has_stroke;
    bool      stroke_none;
    AnimColor stroke;
    float     stroke_width;
    bool      even_odd;
    int32_t   part_id; /* -1 if untagged, else 0..254 */
} SvgStyle;

typedef struct SvgDef {
    char  id[64];
    char *d;
} SvgDef;

enum { SVG_MAX_DEFS = 512, SVG_MAX_STACK = 32 };

typedef struct SvgParser {
    AnimScene *scene;
    AnimId     root;
    AnimId     part_groups[ANIM_MAX_PARTS];
    uint32_t   part_count;
    SvgDef     defs[SVG_MAX_DEFS];
    uint32_t   def_count;
    SvgXform   xf_stack[SVG_MAX_STACK];
    SvgStyle   st_stack[SVG_MAX_STACK];
    uint32_t   depth;
    bool       in_defs;
    char       cur_symbol_id[64];
} SvgParser;

static void skip_ws_comma(const char **p)
{
    while (**p && (isspace((unsigned char)**p) || **p == ','))
        (*p)++;
}

static bool parse_svg_float(const char **p, float *out)
{
    const char *s;
    char       *end = NULL;
    double      v;

    skip_ws_comma(p);
    s = *p;
    if (!*s)
        return false;
    v = strtod(s, &end);
    if (end == s)
        return false;
    *out = (float)v;
    *p   = end;
    return true;
}

static bool parse_svg_flag(const char **p, int *out)
{
    skip_ws_comma(p);
    if (**p == '0' || **p == '1')
    {
        *out = (**p == '1') ? 1 : 0;
        (*p)++;
        return true;
    }
    return false;
}

static SvgXform parse_transform_str(const char *str)
{
    SvgXform    res = xform_id();
    const char *p   = str;

    while (*p)
    {
        while (*p && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (!*p)
            break;
        if (strncmp(p, "matrix", 6) == 0)
        {
            float v[6] = { 1, 0, 0, 1, 0, 0 };
            int   k;
            p = strchr(p, '(');
            if (!p) break;
            p++;
            for (k = 0; k < 6; ++k)
                parse_svg_float(&p, &v[k]);
            res = xform_mul(res, (SvgXform){ { v[0], v[1], v[2], v[3], v[4], v[5] } });
            p   = strchr(p, ')');
            if (!p) break;
            p++;
        }
        else if (strncmp(p, "translate", 9) == 0)
        {
            float tx = 0.0f, ty = 0.0f;
            p = strchr(p, '(');
            if (!p) break;
            p++;
            parse_svg_float(&p, &tx);
            parse_svg_float(&p, &ty);
            res = xform_mul(res, (SvgXform){ { 1, 0, 0, 1, tx, ty } });
            p   = strchr(p, ')');
            if (!p) break;
            p++;
        }
        else if (strncmp(p, "scale", 5) == 0)
        {
            float sx = 1.0f, sy = 1.0f;
            p = strchr(p, '(');
            if (!p) break;
            p++;
            if (parse_svg_float(&p, &sx))
            {
                if (!parse_svg_float(&p, &sy))
                    sy = sx;
            }
            res = xform_mul(res, (SvgXform){ { sx, 0, 0, sy, 0, 0 } });
            p   = strchr(p, ')');
            if (!p) break;
            p++;
        }
        else if (strncmp(p, "rotate", 6) == 0)
        {
            float deg = 0.0f, cx = 0.0f, cy = 0.0f;
            float rad, cs, sn;
            p = strchr(p, '(');
            if (!p) break;
            p++;
            parse_svg_float(&p, &deg);
            parse_svg_float(&p, &cx);
            parse_svg_float(&p, &cy);
            rad = deg * (ANIM_PI / 180.0f);
            cs  = cosf(rad);
            sn  = sinf(rad);
            res = xform_mul(res, (SvgXform){ { 1, 0, 0, 1, cx, cy } });
            res = xform_mul(res, (SvgXform){ { cs, sn, -sn, cs, 0, 0 } });
            res = xform_mul(res, (SvgXform){ { 1, 0, 0, 1, -cx, -cy } });
            p   = strchr(p, ')');
            if (!p) break;
            p++;
        }
        else
        {
            p++;
        }
    }
    return res;
}

static bool parse_hex_color(const char *s, AnimColor *out, int32_t *out_part_id)
{
    uint32_t rgb = 0u;
    size_t   len;

    if (!s || !*s)
        return false;
    if (strcmp(s, "none") == 0)
        return false;
    if (strcmp(s, "white") == 0 || strcmp(s, "#fff") == 0 || strcmp(s, "#ffffff") == 0 || strcmp(s, "#FFFFFF") == 0)
    {
        *out = ANIM_WHITE;
        return true;
    }
    if (strcmp(s, "black") == 0 || strcmp(s, "#000") == 0 || strcmp(s, "#000000") == 0)
    {
        *out = ANIM_BLACK;
        return true;
    }
    if (s[0] == '#')
    {
        len = strlen(s + 1);
        if (len == 6u)
        {
            rgb = (uint32_t)strtoul(s + 1, NULL, 16);
            if (((rgb >> 16) & 0xffu) == 0x01u && ((rgb >> 8) & 0xffu) == 0x00u && (rgb & 0xffu) >= 1u)
            {
                if (out_part_id)
                    *out_part_id = (int32_t)((rgb & 0xffu) - 1u);
                *out = ANIM_WHITE;
                return true;
            }
            *out = anim_hex(rgb);
            return true;
        }
        if (len == 3u)
        {
            uint32_t r = (uint32_t)strtoul((char[]){ s[1], s[1], 0 }, NULL, 16);
            uint32_t g = (uint32_t)strtoul((char[]){ s[2], s[2], 0 }, NULL, 16);
            uint32_t b = (uint32_t)strtoul((char[]){ s[3], s[3], 0 }, NULL, 16);
            *out = anim_hex((r << 16) | (g << 8) | b);
            return true;
        }
    }
    *out = ANIM_WHITE;
    return true;
}

static bool extract_attr(const char *tag_start, const char *tag_end, const char *attr, char *out, size_t out_cap)
{
    size_t      alen = strlen(attr);
    const char *p    = tag_start;

    while (p < tag_end)
    {
        if ((size_t)(tag_end - p) > alen + 2u && strncmp(p, attr, alen) == 0 &&
            (p == tag_start || isspace((unsigned char)p[-1])))
        {
            const char *q = p + alen;
            char        quote;
            const char *val_start;
            size_t      vlen;

            while (q < tag_end && isspace((unsigned char)*q)) q++;
            if (q < tag_end && *q == '=')
            {
                q++;
                while (q < tag_end && isspace((unsigned char)*q)) q++;
                if (q < tag_end && (*q == '"' || *q == '\''))
                {
                    quote     = *q++;
                    val_start = q;
                    while (q < tag_end && *q != quote) q++;
                    vlen = (size_t)(q - val_start);
                    if (vlen >= out_cap)
                        vlen = out_cap - 1u;
                    memcpy(out, val_start, vlen);
                    out[vlen] = '\0';
                    return true;
                }
            }
        }
        p++;
    }
    return false;
}

static void add_arc_svg(AnimScene *s, AnimId id, SvgXform xf,
                        float x0, float y0, float rx, float ry, float xrot_deg, int large_arc, int sweep, float x1, float y1)
{
    float phi  = xrot_deg * (ANIM_PI / 180.0f);
    float cp   = cosf(phi), sp = sinf(phi);
    float dx2  = 0.5f * (x0 - x1), dy2 = 0.5f * (y0 - y1);
    float x1p  =  cp * dx2 + sp * dy2;
    float y1p  = -sp * dx2 + cp * dy2;
    float rx2, ry2, lam, sign, num, den, rad, cxp, cyp, cx, cy, th1, dth;
    int   segs, k;

    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f)
    {
        anim_vmob_line_to(s, id, xform_pt(xf, x1, y1));
        return;
    }
    rx2 = rx * rx;
    ry2 = ry * ry;
    lam = (x1p * x1p) / rx2 + (y1p * y1p) / ry2;
    if (lam > 1.0f)
    {
        float sq = sqrtf(lam);
        rx *= sq; ry *= sq;
        rx2 = rx * rx; ry2 = ry * ry;
    }
    num  = MU_MAX(0.0f, rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p);
    den  = rx2 * y1p * y1p + ry2 * x1p * x1p;
    rad  = (den > 1e-12f) ? sqrtf(num / den) : 0.0f;
    sign = (large_arc == sweep) ? -1.0f : 1.0f;
    cxp  = sign * rad * (rx * y1p / ry);
    cyp  = sign * rad * (-ry * x1p / rx);
    cx   = cp * cxp - sp * cyp + 0.5f * (x0 + x1);
    cy   = sp * cxp + cp * cyp + 0.5f * (y0 + y1);

    {
        float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        float n1 = sqrtf(ux * ux + uy * uy);
        float c1 = MU_CLAMP(ux / MU_MAX(n1, 1e-8f), -1.0f, 1.0f);
        float dp = ux * vx + uy * vy;
        float n2 = sqrtf((ux * ux + uy * uy) * (vx * vx + vy * vy));
        float c2 = MU_CLAMP(dp / MU_MAX(n2, 1e-8f), -1.0f, 1.0f);
        th1 = (uy < 0.0f ? -1.0f : 1.0f) * acosf(c1);
        dth = ((ux * vy - uy * vx) < 0.0f ? -1.0f : 1.0f) * acosf(c2);
        if (!sweep && dth > 0.0f) dth -= ANIM_TAU;
        if (sweep && dth < 0.0f)  dth += ANIM_TAU;
    }

    segs = (int)ceilf(fabsf(dth) / (ANIM_PI * 0.5f));
    if (segs < 1) segs = 1;
    for (k = 0; k < segs; ++k)
    {
        float t0 = th1 + dth * ((float)k / (float)segs);
        float t1 = th1 + dth * ((float)(k + 1) / (float)segs);
        float dt = t1 - t0;
        float kk = (4.0f / 3.0f) * tanf(0.25f * dt);
        float c0 = cosf(t0), s0 = sinf(t0);
        float c1 = cosf(t1), s1 = sinf(t1);
        float q1x = c0 - kk * s0, q1y = s0 + kk * c0;
        float q2x = c1 + kk * s1, q2y = s1 - kk * c1;
        float p1x = cx + cp * (rx * q1x) - sp * (ry * q1y);
        float p1y = cy + sp * (rx * q1x) + cp * (ry * q1y);
        float p2x = cx + cp * (rx * q2x) - sp * (ry * q2y);
        float p2y = cy + sp * (rx * q2x) + cp * (ry * q2y);
        float p3x = cx + cp * (rx * c1) - sp * (ry * s1);
        float p3y = cy + sp * (rx * c1) + cp * (ry * s1);

        anim_vmob_cubic_to(s, id, xform_pt(xf, p1x, p1y), xform_pt(xf, p2x, p2y), xform_pt(xf, p3x, p3y));
    }
}

static void parse_svg_path_d(AnimScene *s, AnimId id, SvgXform xf, const char *d)
{
    const char *p        = d ? d : "";
    char        cmd      = 0;
    float       cx = 0.0f, cy = 0.0f;
    float       sx = 0.0f, sy = 0.0f;
    float       last_cpx = 0.0f, last_cpy = 0.0f;
    char        prev_cmd = 0;
    bool        sub_open = false;

    while (*p)
    {
        skip_ws_comma(&p);
        if (!*p)
            break;
        if (isalpha((unsigned char)*p))
        {
            cmd = *p++;
            if (cmd == 'Z' || cmd == 'z')
            {
                if (sub_open)
                {
                    anim_vmob_close_subpath(s, id);
                    sub_open = false;
                }
                cx       = sx;
                cy       = sy;
                prev_cmd = cmd;
                continue;
            }
        }
        else if (cmd == 0)
        {
            break;
        }

        switch (cmd)
        {
        case 'M':
        case 'm':
        {
            float x = 0.0f, y = 0.0f;
            if (!parse_svg_float(&p, &x) || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 'm') { x += cx; y += cy; }
            cx = sx = x; cy = sy = y;
            anim_vmob_begin_subpath(s, id, xform_pt(xf, cx, cy));
            sub_open = true;
            cmd      = (cmd == 'M') ? 'L' : 'l';
            prev_cmd = 'M';
            break;
        }
        case 'L':
        case 'l':
        {
            float x = 0.0f, y = 0.0f;
            if (!parse_svg_float(&p, &x) || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 'l') { x += cx; y += cy; }
            cx = x; cy = y;
            anim_vmob_line_to(s, id, xform_pt(xf, cx, cy));
            prev_cmd = cmd;
            break;
        }
        case 'H':
        case 'h':
        {
            float x = 0.0f;
            if (!parse_svg_float(&p, &x)) { p = ""; break; }
            cx = (cmd == 'h') ? (cx + x) : x;
            anim_vmob_line_to(s, id, xform_pt(xf, cx, cy));
            prev_cmd = cmd;
            break;
        }
        case 'V':
        case 'v':
        {
            float y = 0.0f;
            if (!parse_svg_float(&p, &y)) { p = ""; break; }
            cy = (cmd == 'v') ? (cy + y) : y;
            anim_vmob_line_to(s, id, xform_pt(xf, cx, cy));
            prev_cmd = cmd;
            break;
        }
        case 'C':
        case 'c':
        {
            float x1, y1, x2, y2, x, y;
            if (!parse_svg_float(&p, &x1) || !parse_svg_float(&p, &y1) ||
                !parse_svg_float(&p, &x2) || !parse_svg_float(&p, &y2) ||
                !parse_svg_float(&p, &x)  || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 'c') { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            anim_vmob_cubic_to(s, id, xform_pt(xf, x1, y1), xform_pt(xf, x2, y2), xform_pt(xf, x, y));
            last_cpx = x2; last_cpy = y2;
            cx = x; cy = y;
            prev_cmd = cmd;
            break;
        }
        case 'S':
        case 's':
        {
            float x1, y1, x2, y2, x, y;
            if (!parse_svg_float(&p, &x2) || !parse_svg_float(&p, &y2) ||
                !parse_svg_float(&p, &x)  || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 's') { x2 += cx; y2 += cy; x += cx; y += cy; }
            if (prev_cmd == 'C' || prev_cmd == 'c' || prev_cmd == 'S' || prev_cmd == 's')
            {
                x1 = 2.0f * cx - last_cpx;
                y1 = 2.0f * cy - last_cpy;
            }
            else
            {
                x1 = cx; y1 = cy;
            }
            anim_vmob_cubic_to(s, id, xform_pt(xf, x1, y1), xform_pt(xf, x2, y2), xform_pt(xf, x, y));
            last_cpx = x2; last_cpy = y2;
            cx = x; cy = y;
            prev_cmd = cmd;
            break;
        }
        case 'Q':
        case 'q':
        {
            float qx, qy, x, y;
            if (!parse_svg_float(&p, &qx) || !parse_svg_float(&p, &qy) ||
                !parse_svg_float(&p, &x)  || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 'q') { qx += cx; qy += cy; x += cx; y += cy; }
            anim_vmob_quad_to(s, id, xform_pt(xf, qx, qy), xform_pt(xf, x, y));
            last_cpx = qx; last_cpy = qy;
            cx = x; cy = y;
            prev_cmd = cmd;
            break;
        }
        case 'T':
        case 't':
        {
            float qx, qy, x, y;
            if (!parse_svg_float(&p, &x) || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 't') { x += cx; y += cy; }
            if (prev_cmd == 'Q' || prev_cmd == 'q' || prev_cmd == 'T' || prev_cmd == 't')
            {
                qx = 2.0f * cx - last_cpx;
                qy = 2.0f * cy - last_cpy;
            }
            else
            {
                qx = cx; qy = cy;
            }
            anim_vmob_quad_to(s, id, xform_pt(xf, qx, qy), xform_pt(xf, x, y));
            last_cpx = qx; last_cpy = qy;
            cx = x; cy = y;
            prev_cmd = cmd;
            break;
        }
        case 'A':
        case 'a':
        {
            float rx, ry, rot, x, y;
            int   fa = 0, fs = 0;
            if (!parse_svg_float(&p, &rx) || !parse_svg_float(&p, &ry) || !parse_svg_float(&p, &rot) ||
                !parse_svg_flag(&p, &fa)  || !parse_svg_flag(&p, &fs)  ||
                !parse_svg_float(&p, &x)  || !parse_svg_float(&p, &y)) { p = ""; break; }
            if (cmd == 'a') { x += cx; y += cy; }
            add_arc_svg(s, id, xf, cx, cy, rx, ry, rot, fa, fs, x, y);
            cx = x; cy = y;
            prev_cmd = cmd;
            break;
        }
        default:
            p++;
            break;
        }
    }
}

static void svg_apply_style(SvgParser *sp, AnimId id, const SvgStyle *st, SvgXform xf)
{
    AnimColor fc = st->fill_none ? anim_rgba(0, 0, 0, 0) : (st->has_fill ? st->fill : ANIM_WHITE);
    AnimColor sc = (st->has_stroke && !st->stroke_none) ? st->stroke : fc;
    float     sw = (st->has_stroke && !st->stroke_none) ? st->stroke_width * xform_scale_mag(xf) : 0.8f;

    if (st->fill_none && st->has_stroke && !st->stroke_none)
        fc = anim_rgba(0, 0, 0, 0);

    anim_set_fill(sp->scene, id, fc, false);
    anim_set_stroke(sp->scene, id, sc, sw, false);
    if (st->even_odd)
        anim_set_fill_rule(sp->scene, id, ANIM_FILL_EVEN_ODD, false);

    if (st->part_id >= 0 && (uint32_t)st->part_id < ANIM_MAX_PARTS)
    {
        while (sp->part_count <= (uint32_t)st->part_id)
            sp->part_groups[sp->part_count++] = ANIM_ID_NIL;
        if (!ANIM_ID_VALID(sp->part_groups[st->part_id]))
        {
            sp->part_groups[st->part_id] = anim_group(sp->scene, NULL, 0u);
            anim_add_child(sp->scene, sp->root, sp->part_groups[st->part_id]);
        }
        anim_add_child(sp->scene, sp->part_groups[st->part_id], id);
    }
    else
    {
        anim_add_child(sp->scene, sp->root, id);
    }
}

static SvgStyle parse_tag_style(SvgStyle parent, const char *s, const char *e)
{
    SvgStyle st = parent;
    char     buf[128];

    if (extract_attr(s, e, "fill", buf, sizeof buf))
    {
        if (strcmp(buf, "none") == 0)
        {
            st.fill_none = true;
            st.has_fill  = false;
        }
        else
        {
            st.fill_none = false;
            st.has_fill  = parse_hex_color(buf, &st.fill, &st.part_id);
        }
    }
    if (extract_attr(s, e, "stroke", buf, sizeof buf))
    {
        if (strcmp(buf, "none") == 0)
        {
            st.stroke_none = true;
            st.has_stroke  = false;
        }
        else
        {
            st.stroke_none = false;
            st.has_stroke  = parse_hex_color(buf, &st.stroke, &st.part_id);
        }
    }
    if (extract_attr(s, e, "stroke-width", buf, sizeof buf))
    {
        const char *bp = buf;
        float       w  = 1.0f;
        if (parse_svg_float(&bp, &w))
            st.stroke_width = w;
    }
    if (extract_attr(s, e, "fill-rule", buf, sizeof buf))
    {
        if (strcmp(buf, "evenodd") == 0)
            st.even_odd = true;
    }
    return st;
}

static const char *find_def(const SvgParser *sp, const char *href)
{
    const char *id = (href && href[0] == '#') ? href + 1 : href;
    uint32_t    i;

    if (!id || !id[0])
        return NULL;
    for (i = 0u; i < sp->def_count; ++i)
        if (strcmp(sp->defs[i].id, id) == 0)
            return sp->defs[i].d;
    return NULL;
}

static void parse_svg_defs(SvgParser *sp, const char *svg, size_t len)
{
    const char *cur = svg;
    const char *end = svg + len;
    char        attr[256];

    sp->in_defs          = false;
    sp->cur_symbol_id[0] = '\0';

    while (cur < end)
    {
        const char *lt = (const char *)memchr(cur, '<', (size_t)(end - cur));
        const char *gt;
        bool        closing;
        bool        self_close;
        char        tag[64];
        size_t      ti = 0u;
        const char *tp;

        if (!lt)
            break;
        gt = (const char *)memchr(lt, '>', (size_t)(end - lt));
        if (!gt)
            break;
        cur = gt + 1;

        if (lt + 1 < gt && (lt[1] == '?' || lt[1] == '!'))
            continue;
        closing    = (lt + 1 < gt && lt[1] == '/');
        self_close = (gt > lt + 1 && gt[-1] == '/');
        tp         = lt + (closing ? 2 : 1);
        while (tp < gt && !isspace((unsigned char)*tp) && *tp != '/' && ti + 1u < sizeof tag)
            tag[ti++] = *tp++;
        tag[ti] = '\0';

        if (closing)
        {
            if (strcmp(tag, "defs") == 0)
                sp->in_defs = false;
            else if (strcmp(tag, "symbol") == 0)
                sp->cur_symbol_id[0] = '\0';
            continue;
        }

        if (strcmp(tag, "defs") == 0)
        {
            if (!self_close) sp->in_defs = true;
            continue;
        }
        if (strcmp(tag, "symbol") == 0)
        {
            if (extract_attr(lt, gt, "id", attr, sizeof attr))
                snprintf(sp->cur_symbol_id, sizeof sp->cur_symbol_id, "%.63s", attr);
            continue;
        }
        if (strcmp(tag, "path") == 0 && (sp->in_defs || sp->cur_symbol_id[0]))
        {
            char        id_buf[64] = { 0 };
            const char *d_start    = NULL;
            char       *d_str      = NULL;
            size_t      d_len      = 0u;
            const char *p          = lt;

            while (p < gt)
            {
                if ((size_t)(gt - p) > 3u && strncmp(p, "d=", 2) == 0 && (p == lt || isspace((unsigned char)p[-1])))
                {
                    char quote = p[2];
                    if (quote == '"' || quote == '\'')
                    {
                        const char *q = strchr(p + 3, quote);
                        if (q && q < gt)
                        {
                            d_start = p + 3;
                            d_len   = (size_t)(q - d_start);
                        }
                    }
                    break;
                }
                p++;
            }
            if (!d_start)
                continue;
            if (!extract_attr(lt, gt, "id", id_buf, sizeof id_buf) && sp->cur_symbol_id[0])
                snprintf(id_buf, sizeof id_buf, "%.63s", sp->cur_symbol_id);
            if (id_buf[0] && sp->def_count < SVG_MAX_DEFS)
            {
                d_str = (char *)malloc(d_len + 1u);
                memcpy(d_str, d_start, d_len);
                d_str[d_len] = '\0';
                snprintf(sp->defs[sp->def_count].id, sizeof sp->defs[sp->def_count].id, "%.63s", id_buf);
                sp->defs[sp->def_count].d = d_str;
                sp->def_count++;
            }
        }
    }
}

static void parse_svg_internal(SvgParser *sp, const char *svg, size_t len)
{
    const char *cur = svg;
    const char *end = svg + len;
    char        attr[256];

    parse_svg_defs(sp, svg, len);

    sp->in_defs          = false;
    sp->cur_symbol_id[0] = '\0';
    sp->depth            = 0u;
    sp->xf_stack[0]      = xform_id();
    sp->st_stack[0]      = (SvgStyle){ .has_fill = true, .fill = ANIM_WHITE, .stroke_width = 1.0f, .part_id = -1 };

    while (cur < end)
    {
        const char *lt = (const char *)memchr(cur, '<', (size_t)(end - cur));
        const char *gt;
        bool        closing;
        bool        self_close;
        char        tag[64];
        size_t      ti = 0u;
        const char *tp;

        if (!lt)
            break;
        gt = (const char *)memchr(lt, '>', (size_t)(end - lt));
        if (!gt)
            break;
        cur = gt + 1;

        if (lt + 1 < gt && (lt[1] == '?' || lt[1] == '!'))
            continue;
        closing    = (lt + 1 < gt && lt[1] == '/');
        self_close = (gt > lt + 1 && gt[-1] == '/');
        tp         = lt + (closing ? 2 : 1);
        while (tp < gt && !isspace((unsigned char)*tp) && *tp != '/' && ti + 1u < sizeof tag)
            tag[ti++] = *tp++;
        tag[ti] = '\0';

        if (closing)
        {
            if (strcmp(tag, "defs") == 0)
                sp->in_defs = false;
            else if (strcmp(tag, "symbol") == 0)
                sp->cur_symbol_id[0] = '\0';
            else if ((strcmp(tag, "g") == 0 || strcmp(tag, "svg") == 0) && sp->depth > 0u)
                sp->depth--;
            continue;
        }

        if (strcmp(tag, "defs") == 0)
        {
            if (!self_close) sp->in_defs = true;
            continue;
        }
        if (strcmp(tag, "symbol") == 0)
        {
            if (!self_close)
                snprintf(sp->cur_symbol_id, sizeof sp->cur_symbol_id, "sym");
            continue;
        }
        if (sp->in_defs || sp->cur_symbol_id[0])
            continue;

        if (strcmp(tag, "g") == 0 || strcmp(tag, "svg") == 0)
        {
            if (!self_close && sp->depth + 1u < SVG_MAX_STACK)
            {
                SvgXform xf = sp->xf_stack[sp->depth];
                SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
                if (extract_attr(lt, gt, "transform", attr, sizeof attr))
                    xf = xform_mul(xf, parse_transform_str(attr));
                sp->depth++;
                sp->xf_stack[sp->depth] = xf;
                sp->st_stack[sp->depth] = st;
            }
            continue;
        }

        if (strcmp(tag, "path") == 0)
        {
            const char *d_start = NULL;
            char       *d_str   = NULL;
            size_t      d_len   = 0u;
            const char *p       = lt;

            while (p < gt)
            {
                if ((size_t)(gt - p) > 3u && strncmp(p, "d=", 2) == 0 && (p == lt || isspace((unsigned char)p[-1])))
                {
                    char quote = p[2];
                    if (quote == '"' || quote == '\'')
                    {
                        const char *q = strchr(p + 3, quote);
                        if (q && q < gt)
                        {
                            d_start = p + 3;
                            d_len   = (size_t)(q - d_start);
                        }
                    }
                    break;
                }
                p++;
            }
            if (!d_start)
                continue;
            d_str = (char *)malloc(d_len + 1u);
            memcpy(d_str, d_start, d_len);
            d_str[d_len] = '\0';

            {
                SvgXform xf = sp->xf_stack[sp->depth];
                SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
                AnimId   vm = anim_vmob_create(sp->scene);

                if (extract_attr(lt, gt, "transform", attr, sizeof attr))
                    xf = xform_mul(xf, parse_transform_str(attr));
                parse_svg_path_d(sp->scene, vm, xf, d_str);
                svg_apply_style(sp, vm, &st, xf);
            }
            free(d_str);
            continue;
        }

        if (strcmp(tag, "use") == 0)
        {
            char        href[128] = { 0 };
            const char *d_ref;
            if (!extract_attr(lt, gt, "xlink:href", href, sizeof href))
                extract_attr(lt, gt, "href", href, sizeof href);
            d_ref = find_def(sp, href);
            if (d_ref)
            {
                float    ux = 0.0f, uy = 0.0f;
                SvgXform xf = sp->xf_stack[sp->depth];
                SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
                AnimId   vm;

                if (extract_attr(lt, gt, "x", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &ux); }
                if (extract_attr(lt, gt, "y", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &uy); }
                if (extract_attr(lt, gt, "transform", attr, sizeof attr))
                    xf = xform_mul(xf, parse_transform_str(attr));
                xf = xform_mul(xf, (SvgXform){ { 1, 0, 0, 1, ux, uy } });

                vm = anim_vmob_create(sp->scene);
                parse_svg_path_d(sp->scene, vm, xf, d_ref);
                svg_apply_style(sp, vm, &st, xf);
            }
            continue;
        }

        if (strcmp(tag, "rect") == 0)
        {
            float    rx = 0, ry = 0, rw = 0, rh = 0;
            SvgXform xf = sp->xf_stack[sp->depth];
            SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
            AnimId   vm;

            if (extract_attr(lt, gt, "x", attr, sizeof attr))      { const char *bp = attr; parse_svg_float(&bp, &rx); }
            if (extract_attr(lt, gt, "y", attr, sizeof attr))      { const char *bp = attr; parse_svg_float(&bp, &ry); }
            if (extract_attr(lt, gt, "width", attr, sizeof attr))  { const char *bp = attr; parse_svg_float(&bp, &rw); }
            if (extract_attr(lt, gt, "height", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &rh); }
            if (extract_attr(lt, gt, "transform", attr, sizeof attr))
                xf = xform_mul(xf, parse_transform_str(attr));
            if (rw > 0.0f && rh > 0.0f)
            {
                vm = anim_vmob_create(sp->scene);
                anim_vmob_begin_subpath(sp->scene, vm, xform_pt(xf, rx, ry));
                anim_vmob_line_to(sp->scene, vm, xform_pt(xf, rx + rw, ry));
                anim_vmob_line_to(sp->scene, vm, xform_pt(xf, rx + rw, ry + rh));
                anim_vmob_line_to(sp->scene, vm, xform_pt(xf, rx, ry + rh));
                anim_vmob_close_subpath(sp->scene, vm);
                svg_apply_style(sp, vm, &st, xf);
            }
            continue;
        }

        if (strcmp(tag, "circle") == 0 || strcmp(tag, "ellipse") == 0)
        {
            float    cx = 0, cy = 0, rx = 0, ry = 0, k = 0.55228475f;
            SvgXform xf = sp->xf_stack[sp->depth];
            SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
            AnimId   vm;

            if (extract_attr(lt, gt, "cx", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &cx); }
            if (extract_attr(lt, gt, "cy", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &cy); }
            if (extract_attr(lt, gt, "r", attr, sizeof attr))  { const char *bp = attr; parse_svg_float(&bp, &rx); ry = rx; }
            if (extract_attr(lt, gt, "rx", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &rx); }
            if (extract_attr(lt, gt, "ry", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &ry); }
            if (rx > 0.0f && ry > 0.0f)
            {
                vm = anim_vmob_create(sp->scene);
                anim_vmob_begin_subpath(sp->scene, vm, xform_pt(xf, cx + rx, cy));
                anim_vmob_cubic_to(sp->scene, vm, xform_pt(xf, cx + rx, cy + k * ry), xform_pt(xf, cx + k * rx, cy + ry), xform_pt(xf, cx, cy + ry));
                anim_vmob_cubic_to(sp->scene, vm, xform_pt(xf, cx - k * rx, cy + ry), xform_pt(xf, cx - rx, cy + k * ry), xform_pt(xf, cx - rx, cy));
                anim_vmob_cubic_to(sp->scene, vm, xform_pt(xf, cx - rx, cy - k * ry), xform_pt(xf, cx - k * rx, cy - ry), xform_pt(xf, cx, cy - ry));
                anim_vmob_cubic_to(sp->scene, vm, xform_pt(xf, cx + k * rx, cy - ry), xform_pt(xf, cx + rx, cy - k * ry), xform_pt(xf, cx + rx, cy));
                anim_vmob_close_subpath(sp->scene, vm);
                svg_apply_style(sp, vm, &st, xf);
            }
            continue;
        }

        if (strcmp(tag, "line") == 0)
        {
            float    x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            SvgXform xf = sp->xf_stack[sp->depth];
            SvgStyle st = parse_tag_style(sp->st_stack[sp->depth], lt, gt);
            AnimId   vm = anim_vmob_create(sp->scene);

            if (extract_attr(lt, gt, "x1", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &x1); }
            if (extract_attr(lt, gt, "y1", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &y1); }
            if (extract_attr(lt, gt, "x2", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &x2); }
            if (extract_attr(lt, gt, "y2", attr, sizeof attr)) { const char *bp = attr; parse_svg_float(&bp, &y2); }
            st.fill_none = true;
            st.has_stroke = true;
            anim_vmob_begin_subpath(sp->scene, vm, xform_pt(xf, x1, y1));
            anim_vmob_line_to(sp->scene, vm, xform_pt(xf, x2, y2));
            svg_apply_style(sp, vm, &st, xf);
            continue;
        }
    }

    for (uint32_t i = 0u; i < sp->def_count; ++i)
        free(sp->defs[i].d);
}

static void normalize_svg_tree(AnimScene *s, AnimId root, float world_height)
{
    AnimVec3 mn, mx;
    float    h;
    float    sc;
    AnimId   nodes[512];
    uint32_t count = 0u;
    uint32_t i;

    anim_bbox_world(s, root, &mn, &mx);
    h = mx.y - mn.y;
    if (h > 1e-5f && world_height > 0.0f)
    {
        sc = world_height / h;
        anim_scale(s, root, sc);
        anim__collect_subtree(s, root, nodes, &count, 512u);
        for (i = 0u; i < count; ++i)
            s->hot[nodes[i].index].stroke_width = MU_MAX(s->hot[nodes[i].index].stroke_width * sc * 8.0f, 1.0f);
    }
    anim_move_to(s, root, ANIM_ORIGIN);
}

AnimId anim_svg_from_memory(AnimScene *s, const char *svg, size_t len, float world_height)
{
    SvgParser *sp = (SvgParser *)calloc(1, sizeof *sp);

    MU_ASSERT(sp != NULL);
    sp->scene = s;
    sp->root  = anim_group(s, NULL, 0u);
    if (svg && len > 0u)
        parse_svg_internal(sp, svg, len);
    normalize_svg_tree(s, sp->root, world_height > 0.0f ? world_height : 1.0f);
    {
        AnimId root = sp->root;
        free(sp);
        return root;
    }
}

AnimId anim_svg_from_file(AnimScene *s, const char *path, float world_height)
{
    void  *data = NULL;
    size_t size = 0u;
    AnimId res;

    if (!mu_files_read_all(path, &data, &size))
    {
        anim__set_error("anim_svg: failed to open %s", path ? path : "(null)");
        return anim_group(s, NULL, 0u);
    }
    res = anim_svg_from_memory(s, (const char *)data, size, world_height);
    free(data);
    return res;
}

/* ------------------------------------------- Typst / LaTeX + mu_save cache */

static uint64_t fnv1a_bytes(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t         i;
    for (i = 0u; i < len; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

#define TEX_CACHE_FORMAT_ID 0x4D4E414Du /* 'MNAM' */
#define TEX_CACHE_CHUNK_SVG 0x53564731u /* 'SVG1' */

static bool try_load_svg_cache(const char *cache_path, char **out_svg, size_t *out_len)
{
    void          *buf  = NULL;
    size_t         size = 0u;
    mu_save_reader r;
    uint32_t       cap  = 0u;
    uint32_t       got  = 0u;
    char          *svg;

    if (!mu_files_read_all(cache_path, &buf, &size))
        return false;
    if (mu_save_open(&r, buf, (uint32_t)size, TEX_CACHE_FORMAT_ID) != MU_SAVE_OK)
    {
        free(buf);
        return false;
    }
    if (mu_save_find(&r, TEX_CACHE_CHUNK_SVG) != MU_SAVE_OK)
    {
        free(buf);
        return false;
    }
    cap = mu_save_chunk_remaining(&r);
    if (cap == 0u || cap > size)
    {
        free(buf);
        return false;
    }
    svg = (char *)malloc((size_t)cap + 1u);
    got = mu_save_read_bytes(&r, svg, cap);
    if (got == 0u || r.failed)
    {
        free(svg);
        free(buf);
        return false;
    }
    svg[got] = '\0';
    free(buf);
    *out_svg = svg;
    *out_len = got;
    return true;
}

static void save_svg_cache(const char *cache_dir, const char *cache_path, const char *svg, size_t len)
{
    uint32_t       cap = (uint32_t)len + 256u;
    void          *buf = malloc(cap);
    mu_save_writer w;
    uint32_t       written;

    mu_files_mkdirs(cache_dir);
    if (mu_save_writer_begin(&w, buf, cap, TEX_CACHE_FORMAT_ID) != MU_SAVE_OK)
    {
        free(buf);
        return;
    }
    mu_save_chunk_begin(&w, TEX_CACHE_CHUNK_SVG, 1u);
    mu_save_bytes(&w, svg, (uint32_t)len);
    mu_save_chunk_end(&w);
    written = mu_save_writer_commit(&w);
    if (written > 0u)
        mu_files_write_atomic(cache_path, buf, written);
    free(buf);
}

static AnimTexBackend resolve_backend(AnimTexBackend b, const char *const *parts, uint32_t count)
{
    uint32_t i;
    if (b != ANIM_TEX_AUTO)
        return b;
    for (i = 0u; i < count; ++i)
    {
        if (parts[i] && strchr(parts[i], '\\') != NULL)
            return ANIM_TEX_LATEX;
    }
    return ANIM_TEX_TYPST;
}

static bool compile_tex_to_svg(AnimTexBackend backend, const char *const *parts, const bool *tagged, uint32_t count,
                               char **out_svg, size_t *out_len)
{
    char     tmp_dir[256];
    char     src_path[320];
    char     dvi_path[320];
    char     svg_path[320];
    char     cmd[2048];
    FILE    *f;
    uint32_t i;
    uint32_t tag_idx = 1u;
    int      rc;

    snprintf(tmp_dir, sizeof tmp_dir, "/tmp/mu_anim_tex_%lu", (unsigned long)(mu_clock_ns() & 0xffffffffull));
    mu_files_mkdirs(tmp_dir);
    snprintf(svg_path, sizeof svg_path, "%s/out.svg", tmp_dir);

    if (backend == ANIM_TEX_TYPST)
    {
        snprintf(src_path, sizeof src_path, "%s/in.typ", tmp_dir);
        f = fopen(src_path, "w");
        if (!f)
            return false;
        fprintf(f, "#set page(width: auto, height: auto, margin: 2pt, fill: none)\n");
        fprintf(f, "#set text(size: 20pt, fill: white)\n$ ");
        for (i = 0u; i < count; ++i)
        {
            if (tagged[i])
            {
                fprintf(f, "#text(fill: rgb(\"#0100%02X\"))[$%s$] ", (unsigned)(tag_idx & 0xffu), parts[i]);
                tag_idx++;
            }
            else
            {
                fprintf(f, "%s ", parts[i]);
            }
        }
        fprintf(f, "$\n");
        fclose(f);
        snprintf(cmd, sizeof cmd, "typst compile --format svg \"%s\" \"%s\" >/dev/null 2>&1", src_path, svg_path);
        rc = system(cmd);
    }
    else
    {
        snprintf(src_path, sizeof src_path, "%s/in.tex", tmp_dir);
        snprintf(dvi_path, sizeof dvi_path, "%s/in.dvi", tmp_dir);
        f = fopen(src_path, "w");
        if (!f)
            return false;
        fprintf(f, "\\documentclass[12pt]{article}\n\\pagestyle{empty}\n\\usepackage{amsmath,amssymb,color}\n\\begin{document}\n\\[\n");
        for (i = 0u; i < count; ++i)
        {
            if (tagged[i])
            {
                fprintf(f, "{\\color[RGB]{1,0,%u}%s} ", (unsigned)(tag_idx & 0xffu), parts[i]);
                tag_idx++;
            }
            else
            {
                fprintf(f, "%s ", parts[i]);
            }
        }
        fprintf(f, "\n\\]\n\\end{document}\n");
        fclose(f);
        snprintf(cmd, sizeof cmd,
                 "latex -interaction=nonstopmode -halt-on-error -output-directory=\"%s\" \"%s\" >/dev/null 2>&1 && "
                 "dvisvgm --no-fonts --exact-bbox \"%s\" -o \"%s\" >/dev/null 2>&1",
                 tmp_dir, src_path, dvi_path, svg_path);
        rc = system(cmd);
    }

    if (rc == 0)
    {
        void  *data = NULL;
        size_t sz   = 0u;
        if (mu_files_read_all(svg_path, &data, &sz))
        {
            *out_svg = (char *)data;
            *out_len = sz;
        }
    }
    snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", tmp_dir);
    (void)system(cmd);
    return *out_svg != NULL;
}

static AnimId build_tex_from_parts(AnimScene *s, AnimTexBackend backend, const char *const *parts,
                                   const bool *tagged, uint32_t count, float world_height)
{
    AnimTexBackend resolved = resolve_backend(backend, parts, count);
    uint64_t       h        = 14695981039346656037ull;
    char           cache_path[ANIM_PATH_LEN + 64];
    char          *svg      = NULL;
    size_t         svg_len  = 0u;
    uint32_t       i;
    SvgParser     *sp;
    uint32_t       tag_idx  = 0u;

    h = fnv1a_bytes(h, &resolved, sizeof resolved);
    for (i = 0u; i < count; ++i)
    {
        if (parts[i])
            h = fnv1a_bytes(h, parts[i], strlen(parts[i]) + 1u);
        h = fnv1a_bytes(h, &tagged[i], sizeof tagged[i]);
    }
    snprintf(cache_path, sizeof cache_path, "%s/tex_%016llx.msv", s->cache_dir, (unsigned long long)h);

    if (!try_load_svg_cache(cache_path, &svg, &svg_len))
    {
        if (!compile_tex_to_svg(resolved, parts, tagged, count, &svg, &svg_len))
        {
            /* Graceful fallback to plain stb_truetype text if compiler absent, preserving part tags */
            AnimId grp = anim_group(s, NULL, 0u);
            float  cursor_x = 0.0f;
            float  wh = world_height > 0.0f ? world_height : 0.7f;
            anim__set_error("anim_tex: %s compilation failed, using plain text fallback",
                            resolved == ANIM_TEX_TYPST ? "typst" : "latex");
            for (i = 0u; i < count; ++i)
            {
                const char *pt = parts[i];
                AnimId      pm;
                AnimVec3    bmin, bmax;
                if (!pt) continue;
                while (*pt == ' ') pt++;
                if (!*pt) continue;
                pm = anim_text(s, NULL, pt, wh);
                if (!ANIM_ID_VALID(pm)) continue;
                anim_bbox_world(s, pm, &bmin, &bmax);
                anim_shift(s, pm, ANIM_V3(cursor_x - bmin.x, 0.0f, 0.0f));
                cursor_x += (bmax.x - bmin.x) + wh * 0.25f;
                if (tagged[i])
                    anim_set_tag(s, pm, parts[i]);
                anim_add_child(s, grp, pm);
            }
            anim_move_to(s, grp, ANIM_V3(0.0f, 0.0f, 0.0f));
            return grp;
        }
        save_svg_cache(s->cache_dir, cache_path, svg, svg_len);
    }

    sp        = (SvgParser *)calloc(1, sizeof *sp);
    sp->scene = s;
    sp->root  = anim_group(s, NULL, 0u);
    parse_svg_internal(sp, svg, svg_len);
    for (i = 0u; i < count; ++i)
    {
        if (tagged[i])
        {
            if (tag_idx < sp->part_count && ANIM_ID_VALID(sp->part_groups[tag_idx]))
                anim_set_tag(s, sp->part_groups[tag_idx], parts[i]);
            tag_idx++;
        }
    }
    normalize_svg_tree(s, sp->root, world_height > 0.0f ? world_height : 0.7f);
    {
        AnimId root = sp->root;
        free(sp);
        free(svg);
        return root;
    }
}

AnimId anim_tex(AnimScene *s, AnimTexBackend backend, const char *expr, float world_height)
{
    const char *raw_parts[64];
    bool        tagged[64];
    char        storage[2048];
    size_t      used = 0u;
    uint32_t    n    = 0u;
    const char *p    = expr ? expr : "";

    /* Support inline {{token}} syntax for semantic TransformMatchingTex */
    if (strstr(p, "{{") != NULL)
    {
        while (*p && n < 64u)
        {
            const char *open = strstr(p, "{{");
            if (!open)
            {
                size_t l = strlen(p);
                if (l > 0u && used + l + 1u < sizeof storage)
                {
                    raw_parts[n] = storage + used;
                    tagged[n]    = false;
                    memcpy(storage + used, p, l);
                    storage[used + l] = '\0';
                    used += l + 1u;
                    n++;
                }
                break;
            }
            if (open > p)
            {
                size_t l = (size_t)(open - p);
                if (used + l + 1u < sizeof storage)
                {
                    raw_parts[n] = storage + used;
                    tagged[n]    = false;
                    memcpy(storage + used, p, l);
                    storage[used + l] = '\0';
                    used += l + 1u;
                    n++;
                }
            }
            {
                const char *close = strstr(open + 2, "}}");
                if (!close)
                    break;
                size_t l = (size_t)(close - (open + 2));
                if (l > 0u && used + l + 1u < sizeof storage && n < 64u)
                {
                    raw_parts[n] = storage + used;
                    tagged[n]    = true;
                    memcpy(storage + used, open + 2, l);
                    storage[used + l] = '\0';
                    used += l + 1u;
                    n++;
                }
                p = close + 2;
            }
        }
        if (n > 0u)
            return build_tex_from_parts(s, backend, raw_parts, tagged, n, world_height);
    }

    raw_parts[0] = p;
    tagged[0]    = false;
    return build_tex_from_parts(s, backend, raw_parts, tagged, 1u, world_height);
}

AnimId anim_tex_parts(AnimScene *s, AnimTexBackend backend, const char *const *parts, uint32_t part_count, float world_height)
{
    bool     tagged[ANIM_MAX_PARTS];
    uint32_t n = MU_MIN(part_count, (uint32_t)ANIM_MAX_PARTS);
    uint32_t i;

    for (i = 0u; i < n; ++i)
        tagged[i] = true;
    return build_tex_from_parts(s, backend, parts, tagged, n, world_height);
}

AnimId anim_tex_align(AnimScene *s, AnimTexBackend backend, const char *const *lines,
                      uint32_t line_count, float line_height, float line_spacing)
{
    uint32_t i;
    bool     has_amp = false;
    float    lh      = line_height > 0.0f ? line_height : 0.55f;
    float    lsep    = line_spacing > 0.0f ? line_spacing : (lh * 0.45f);

    if (!s || !lines || line_count == 0u)
        return anim_group(s, NULL, 0u);

    for (i = 0u; i < line_count; ++i)
        if (lines[i] && strchr(lines[i], '&') != NULL)
            has_amp = true;

    if (has_amp && line_count > 1u)
    {
        AnimTexBackend resolved = resolve_backend(backend, lines, line_count);
        size_t         cap      = 4096u;
        char          *buf      = (char *)malloc(cap);
        size_t         used     = 0u;
        const char    *part_ptr[1];
        bool           part_tag[1] = { false };

        if (resolved == ANIM_TEX_LATEX)
        {
            const char *hdr = "\\begin{aligned} ";
            memcpy(buf + used, hdr, strlen(hdr));
            used += strlen(hdr);
        }
        for (i = 0u; i < line_count; ++i)
        {
            const char *ln = lines[i] ? lines[i] : "";
            size_t      ll = strlen(ln);
            const char *sep = (i + 1u < line_count)
                ? (resolved == ANIM_TEX_LATEX ? " \\\\ " : " \\ ")
                : "";
            size_t      sl = strlen(sep);
            if (used + ll + sl + 64u > cap)
            {
                cap = (used + ll + sl + 128u) * 2u;
                buf = (char *)realloc(buf, cap);
            }
            memcpy(buf + used, ln, ll);
            used += ll;
            memcpy(buf + used, sep, sl);
            used += sl;
        }
        if (resolved == ANIM_TEX_LATEX)
        {
            const char *ftr = " \\end{aligned}";
            memcpy(buf + used, ftr, strlen(ftr));
            used += strlen(ftr);
        }
        buf[used]   = '\0';
        part_ptr[0] = buf;
        {
            float  total_h = (float)line_count * lh + (float)(line_count - 1u) * lsep;
            AnimId res     = build_tex_from_parts(s, resolved, part_ptr, part_tag, 1u, total_h);
            free(buf);
            return res;
        }
    }

    {
        AnimId *kids = (AnimId *)malloc((size_t)line_count * sizeof *kids);
        float   y    = 0.5f * ((float)(line_count - 1u)) * (lh + lsep);
        AnimId  grp;
        MU_ASSERT(kids != NULL);
        for (i = 0u; i < line_count; ++i)
        {
            kids[i] = anim_tex(s, backend, lines[i] ? lines[i] : "", lh);
            anim_move_to(s, kids[i], ANIM_V3(0.0f, y - (float)i * (lh + lsep), 0.0f));
        }
        grp = anim_group(s, kids, line_count);
        free(kids);
        return grp;
    }
}

AnimId anim_tex_diff(AnimScene *s, AnimTexBackend backend, const char *const *parts_a, uint32_t count_a,
                     const char *const *parts_b, uint32_t count_b, float world_height,
                     AnimColor removed_col, AnimColor added_col, AnimId *out_eq_a, AnimId *out_eq_b)
{
    uint32_t na = MU_MIN(count_a, 63u);
    uint32_t nb = MU_MIN(count_b, 63u);
    uint16_t dp[65][65];
    uint8_t  match_a[64];
    uint8_t  match_b[64];
    AnimId   eq_a, eq_b;
    uint32_t i, j, match_id = 0u;
    float    wh = world_height > 0.0f ? world_height : 0.7f;

    if (!s)
        return ANIM_ID_NIL;

    eq_a = anim_tex_parts(s, backend, parts_a, na, wh);
    eq_b = anim_tex_parts(s, backend, parts_b, nb, wh);

    memset(dp, 0, sizeof dp);
    memset(match_a, 0, sizeof match_a);
    memset(match_b, 0, sizeof match_b);

    for (i = na; i-- > 0u;)
    {
        for (j = nb; j-- > 0u;)
        {
            const char *sa = parts_a && parts_a[i] ? parts_a[i] : "";
            const char *sb = parts_b && parts_b[j] ? parts_b[j] : "";
            if (strcmp(sa, sb) == 0)
                dp[i][j] = (uint16_t)(1u + dp[i + 1u][j + 1u]);
            else
                dp[i][j] = (dp[i + 1u][j] >= dp[i][j + 1u]) ? dp[i + 1u][j] : dp[i][j + 1u];
        }
    }

    i = 0u; j = 0u;
    while (i < na && j < nb)
    {
        const char *sa = parts_a && parts_a[i] ? parts_a[i] : "";
        const char *sb = parts_b && parts_b[j] ? parts_b[j] : "";
        if (strcmp(sa, sb) == 0)
        {
            char tag_buf[ANIM_TAG_LEN];
            snprintf(tag_buf, sizeof tag_buf, "_diff_m%u", match_id++);
            match_a[i] = 1u;
            match_b[j] = 1u;
            if (i < anim_child_count(s, eq_a))
                anim_set_tag(s, anim_child_at(s, eq_a, i), tag_buf);
            if (j < anim_child_count(s, eq_b))
                anim_set_tag(s, anim_child_at(s, eq_b, j), tag_buf);
            i++; j++;
        }
        else if (dp[i + 1u][j] >= dp[i][j + 1u])
        {
            i++;
        }
        else
        {
            j++;
        }
    }

    for (i = 0u; i < anim_child_count(s, eq_a) && i < na; ++i)
    {
        if (!match_a[i])
        {
            char tag_buf[ANIM_TAG_LEN];
            AnimId ch = anim_child_at(s, eq_a, i);
            snprintf(tag_buf, sizeof tag_buf, "_diff_del_%u", i);
            anim_set_tag(s, ch, tag_buf);
            if (removed_col.a > 0.0f)
                anim_set_color(s, ch, removed_col, true);
        }
    }
    for (j = 0u; j < anim_child_count(s, eq_b) && j < nb; ++j)
    {
        if (!match_b[j])
        {
            char tag_buf[ANIM_TAG_LEN];
            AnimId ch = anim_child_at(s, eq_b, j);
            snprintf(tag_buf, sizeof tag_buf, "_diff_add_%u", j);
            anim_set_tag(s, ch, tag_buf);
            if (added_col.a > 0.0f)
                anim_set_color(s, ch, added_col, true);
        }
    }

    anim_move_to(s, eq_a, ANIM_V3(0.0f,  wh * 0.95f, 0.0f));
    anim_move_to(s, eq_b, ANIM_V3(0.0f, -wh * 0.95f, 0.0f));
    if (out_eq_a) *out_eq_a = eq_a;
    if (out_eq_b) *out_eq_b = eq_b;
    {
        AnimId kids[2] = { eq_a, eq_b };
        return anim_group(s, kids, 2u);
    }
}

/* ------------------------------------------------ Image / Texture Quads */

static uint64_t hash_rgba_fnv(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint64_t hash = 14695981039346656037ull;
    size_t   n    = (size_t)w * (size_t)h * 4u;
    hash = fnv1a_bytes(hash, &w, sizeof w);
    hash = fnv1a_bytes(hash, &h, sizeof h);
    if (rgba && n > 0u)
        hash = fnv1a_bytes(hash, rgba, n);
    return hash ? hash : 1ull;
}

uint32_t anim__register_texture_rgba(AnimScene *s, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint64_t hash;
    uint32_t i;
    size_t   nbytes;
    uint8_t *copy;

    if (!s || !rgba || w == 0u || h == 0u)
        return 0u;

    hash = hash_rgba_fnv(rgba, w, h);
    for (i = 0u; i < s->texture_count; ++i)
    {
        if (s->textures[i].width == w && s->textures[i].height == h && s->textures[i].hash == hash)
            return i + 1u;
    }

    if (s->texture_count >= s->texture_cap)
    {
        uint32_t new_cap = s->texture_cap ? s->texture_cap * 2u : 16u;
        s->textures    = (AnimTexture *)realloc(s->textures, (size_t)new_cap * sizeof *s->textures);
        s->texture_cap = new_cap;
        MU_ASSERT(s->textures != NULL);
    }

    nbytes = (size_t)w * (size_t)h * 4u;
    copy   = (uint8_t *)malloc(nbytes);
    MU_ASSERT(copy != NULL);
    memcpy(copy, rgba, nbytes);

    i = s->texture_count++;
    s->textures[i].rgba   = copy;
    s->textures[i].width  = w;
    s->textures[i].height = h;
    s->textures[i].hash   = hash;
    return i + 1u;
}

uint32_t anim__load_texture_file(AnimScene *s, const char *path, uint32_t *out_w, uint32_t *out_h)
{
    int      w = 0, h = 0, comp = 0;
    stbi_uc *px;
    uint32_t tid;

    if (!s || !path || !path[0])
        return 0u;
    px = stbi_load(path, &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0)
    {
        anim__set_error("anim_image: failed to load '%s' (%s)", path, stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (px) stbi_image_free(px);
        return 0u;
    }
    tid = anim__register_texture_rgba(s, px, (uint32_t)w, (uint32_t)h);
    stbi_image_free(px);
    if (out_w) *out_w = (uint32_t)w;
    if (out_h) *out_h = (uint32_t)h;
    return tid;
}

uint32_t anim__load_texture_memory(AnimScene *s, const void *bytes, size_t len, uint32_t *out_w, uint32_t *out_h)
{
    int      w = 0, h = 0, comp = 0;
    stbi_uc *px;
    uint32_t tid;

    if (!s || !bytes || len == 0u)
        return 0u;
    px = stbi_load_from_memory((const stbi_uc *)bytes, (int)len, &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0)
    {
        anim__set_error("anim_image_from_memory: decode failed (%s)", stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (px) stbi_image_free(px);
        return 0u;
    }
    tid = anim__register_texture_rgba(s, px, (uint32_t)w, (uint32_t)h);
    stbi_image_free(px);
    if (out_w) *out_w = (uint32_t)w;
    if (out_h) *out_h = (uint32_t)h;
    return tid;
}

static AnimId create_textured_quad(AnimScene *s, uint32_t tex_id, uint32_t w_px, uint32_t h_px, float world_height)
{
    const uint32_t nx = 8u, ny = 8u;
    uint32_t       vc = (nx + 1u) * (ny + 1u);
    uint32_t       ic = nx * ny * 6u;
    float          wh = world_height > 0.0f ? world_height : 3.0f;
    float          ww = wh * ((float)w_px / (float)MU_MAX(h_px, 1u));
    AnimId         id = anim_vmob_create(s);
    VMobHot       *hd;
    VMobCold      *cd;
    uint32_t       vo, io, ix, iy, k = 0u;

    if (!ANIM_ID_VALID(id))
        return id;

    anim__ensure_surf_vert_cap(s, vc);
    anim__ensure_surf_idx_cap(s, ic);
    hd = &s->hot[id.index];
    cd = &s->cold[id.index];
    vo = s->surf_vert_count;
    io = s->surf_idx_count;

    for (iy = 0u; iy <= ny; ++iy)
    {
        float v = (float)iy / (float)ny; /* 0 = top of image, 1 = bottom */
        float y = (0.5f - v) * wh;
        for (ix = 0u; ix <= nx; ++ix)
        {
            float       u = (float)ix / (float)nx;
            float       x = (u - 0.5f) * ww;
            SurfaceVert sv;
            sv.pos   = ANIM_V3(x, y, 0.0f);
            sv.norm  = ANIM_V3(0.0f, 0.0f, 1.0f);
            sv.u     = u;
            sv.v     = v;
            sv.color = ANIM_WHITE;
            s->surf_verts[vo + iy * (nx + 1u) + ix] = sv;
        }
    }
    for (iy = 0u; iy < ny; ++iy)
    {
        for (ix = 0u; ix < nx; ++ix)
        {
            uint32_t i00 = iy * (nx + 1u) + ix;
            uint32_t i10 = i00 + 1u;
            uint32_t i01 = i00 + (nx + 1u);
            uint32_t i11 = i01 + 1u;
            s->surf_indices[io + k++] = i00; s->surf_indices[io + k++] = i10; s->surf_indices[io + k++] = i11;
            s->surf_indices[io + k++] = i00; s->surf_indices[io + k++] = i11; s->surf_indices[io + k++] = i01;
        }
    }

    s->surf_vert_count += vc;
    s->surf_idx_count  += ic;
    hd->surf_vert_offset = vo;
    hd->surf_vert_count  = vc;
    hd->surf_idx_offset  = io;
    hd->surf_idx_count   = ic;
    hd->fill_color       = ANIM_WHITE;
    hd->stroke_color     = anim_rgba(0.0f, 0.0f, 0.0f, 0.0f);
    cd->surf_grid_density = 0.0f;
    cd->surf_grid_alpha   = 0.0f;
    cd->surf_roughness    = 0.5f;
    cd->surf_tex_id       = tex_id;
    cd->surf_unlit        = true;
    return id;
}

AnimId anim_image(AnimScene *s, const char *path, float world_height)
{
    uint32_t w = 0u, h = 0u;
    uint32_t tid = anim__load_texture_file(s, path, &w, &h);
    if (tid == 0u)
        return ANIM_ID_NIL;
    return create_textured_quad(s, tid, w, h, world_height);
}

AnimId anim_image_from_memory(AnimScene *s, const void *bytes, size_t len, float world_height)
{
    uint32_t w = 0u, h = 0u;
    uint32_t tid = anim__load_texture_memory(s, bytes, len, &w, &h);
    if (tid == 0u)
        return ANIM_ID_NIL;
    return create_textured_quad(s, tid, w, h, world_height);
}

AnimId anim_pixel_array(AnimScene *s, const uint8_t *rgba, uint32_t width, uint32_t height, float world_height)
{
    uint32_t tid;
    AnimId   id;
    VMobCold *cd;

    if (!s || !rgba || width == 0u || height == 0u)
        return ANIM_ID_NIL;
    tid = anim__register_texture_rgba(s, rgba, width, height);
    if (tid == 0u)
        return ANIM_ID_NIL;
    id = create_textured_quad(s, tid, width, height, world_height);
    cd = anim__cold(s, id);
    if (cd)
        cd->surf_tex_nearest = true;
    return id;
}

AnimId anim_pixel_array_fn(AnimScene *s, uint32_t width, uint32_t height, float world_height, AnimPixelFn fn, void *user)
{
    uint32_t w = width > 0u ? width : 32u;
    uint32_t h = height > 0u ? height : 32u;
    uint8_t *rgba;
    uint32_t x, y;
    AnimId   id;

    if (!s || !fn)
        return ANIM_ID_NIL;
    rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba)
        return ANIM_ID_NIL;

    for (y = 0u; y < h; ++y)
    {
        for (x = 0u; x < w; ++x)
        {
            AnimColor c = fn(x, y, w, h, user);
            size_t idx  = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            rgba[idx + 0u] = (uint8_t)(MU_CLAMP(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[idx + 1u] = (uint8_t)(MU_CLAMP(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[idx + 2u] = (uint8_t)(MU_CLAMP(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            rgba[idx + 3u] = (uint8_t)(MU_CLAMP(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    id = anim_pixel_array(s, rgba, w, h, world_height);
    free(rgba);
    return id;
}

void anim_pixel_array_update(AnimScene *s, AnimId id, const uint8_t *rgba, uint32_t width, uint32_t height)
{
    VMobCold *cd = anim__cold(s, id);
    uint32_t  tid;

    if (!cd || !rgba || width == 0u || height == 0u)
        return;
    tid = anim__register_texture_rgba(s, rgba, width, height);
    if (tid != 0u)
    {
        cd->surf_tex_id      = tid;
        cd->surf_tex_nearest = true;
    }
}
