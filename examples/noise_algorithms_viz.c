/*
 * examples/noise_algorithms_viz.c
 *
 * Visualizes procedural noise algorithms from mu_lib (mu/mu_noise_math.h):
 *   Act 1: White, Value, Perlin & Simplex 2D fields + 1D cross-section waveforms
 *   Act 2: Worley / Cellular noise (F1 Euclidean, F2-F1 Edge, Manhattan metric)
 *   Act 3: 3D PBR Terrain synthesis (fBm, Ridged Multifractal, Recursive Domain Warp)
 *   Act 4: Divergence-free 2D Curl Noise (potential contours, vector field & tracers)
 *
 * Usage:
 *   ./noise_viz_demo                              Interactive preview
 *   ./noise_viz_demo --shot noise.png --time 13   Headless PNG shot
 *   ./noise_viz_demo --video noise.mp4            Headless MP4 export
 */
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_noise_math.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "../third_party/mugfx/desktop/platform.h"
#include "../third_party/mugfx/desktop/vk.h"
#include "../anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define rgb(r, g, b)  anim_rgba((r), (g), (b), 1.0f)
#define lin(r, g, b)  anim_rgba(powf((r), 2.2f) * 0.70f, powf((g), 2.2f) * 0.70f, powf((b), 2.2f) * 0.70f, 1.0f)
#define fade_in(id)   anim_anim_fade_in((id), ANIM_ORIGIN, 1.0f)
#define fade_out(id)  anim_anim_fade_out((id), ANIM_ORIGIN, 1.0f)

static const char *find_font_path(void)
{
    static const char *const candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    size_t i;
    for (i = 0u; i < MU_ARRAY_COUNT(candidates); ++i)
    {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

static AnimColor lerp_c(AnimColor a, AnimColor b, float t)
{
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    return anim_rgba(a.r + (b.r - a.r) * u,
                     a.g + (b.g - a.g) * u,
                     a.b + (b.b - a.b) * u,
                     a.a + (b.a - a.a) * u);
}

static AnimColor heatmap_color(float t)
{
    AnimColor c0 = lin(0.04f, 0.07f, 0.24f);
    AnimColor c1 = lin(0.08f, 0.40f, 0.72f);
    AnimColor c2 = lin(0.18f, 0.82f, 0.88f);
    AnimColor c3 = lin(0.99f, 0.88f, 0.28f);
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    if (u < 0.35f) return lerp_c(c0, c1, u / 0.35f);
    if (u < 0.70f) return lerp_c(c1, c2, (u - 0.35f) / 0.35f);
    return lerp_c(c2, c3, (u - 0.70f) / 0.30f);
}

static AnimColor cellular_color(float t)
{
    AnimColor c0 = lin(0.03f, 0.04f, 0.14f);
    AnimColor c1 = lin(0.55f, 0.16f, 0.72f);
    AnimColor c2 = lin(0.96f, 0.38f, 0.52f);
    AnimColor c3 = lin(0.99f, 0.90f, 0.42f);
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    if (u < 0.35f) return lerp_c(c0, c1, u / 0.35f);
    if (u < 0.72f) return lerp_c(c1, c2, (u - 0.35f) / 0.37f);
    return lerp_c(c2, c3, (u - 0.72f) / 0.28f);
}

static AnimColor terrain_color(float h)
{
    AnimColor water = lin(0.04f, 0.12f, 0.36f);
    AnimColor shore = lin(0.06f, 0.54f, 0.62f);
    AnimColor grass = lin(0.20f, 0.78f, 0.42f);
    AnimColor rock  = lin(0.88f, 0.62f, 0.22f);
    AnimColor snow  = lin(0.96f, 0.98f, 1.00f);
    float u = MU_CLAMP(h, 0.0f, 1.0f);
    if (u < 0.28f) return lerp_c(water, shore, u / 0.28f);
    if (u < 0.52f) return lerp_c(shore, grass, (u - 0.28f) / 0.24f);
    if (u < 0.78f) return lerp_c(grass, rock,  (u - 0.52f) / 0.26f);
    return lerp_c(rock, snow, (u - 0.78f) / 0.22f);
}

typedef enum NoiseKind {
    NK_WHITE = 0,
    NK_VALUE,
    NK_PERLIN,
    NK_SIMPLEX,
    NK_WORLEY_F1,
    NK_WORLEY_EDGE,
    NK_WORLEY_MANHATTAN,
    NK_FBM,
    NK_RIDGED,
    NK_WARPED
} NoiseKind;

typedef struct PanelDesc {
    NoiseKind kind;
    float     half_size;
    float     freq;
    float     relief;
    uint32_t  seed;
} PanelDesc;

static float eval_noise_01(NoiseKind kind, float x, float y, uint32_t seed)
{
    switch (kind)
    {
    case NK_WHITE:
        return 0.5f + 0.5f * mu_noise_white2(floorf(x * 4.0f), floorf(y * 4.0f), seed);
    case NK_VALUE:
        return MU_CLAMP(0.5f + 0.72f * mu_noise_value2(x, y, seed), 0.0f, 1.0f);
    case NK_PERLIN:
        return MU_CLAMP(0.5f + 0.85f * mu_noise_perlin2(x, y, seed), 0.0f, 1.0f);
    case NK_SIMPLEX:
        return MU_CLAMP(0.5f + 0.80f * mu_noise_simplex2(x, y, seed), 0.0f, 1.0f);
    case NK_WORLEY_F1:
        return MU_CLAMP(mu_noise_worley2_f1(x, y, seed, MU_NOISE_DIST_EUCLIDEAN) * 1.15f, 0.0f, 1.0f);
    case NK_WORLEY_EDGE:
        return MU_CLAMP(1.0f - mu_noise_worley2_edge(x, y, seed, MU_NOISE_DIST_EUCLIDEAN) * 1.65f, 0.0f, 1.0f);
    case NK_WORLEY_MANHATTAN:
        return MU_CLAMP(mu_noise_worley2_f1(x, y, seed, MU_NOISE_DIST_MANHATTAN) * 0.90f, 0.0f, 1.0f);
    case NK_FBM:
        return MU_CLAMP(0.5f + 0.78f * mu_noise_fbm2(mu_noise_simplex2, x, y, seed, 5, 2.05f, 0.5f), 0.0f, 1.0f);
    case NK_RIDGED:
        return MU_CLAMP(0.46f + 0.62f * mu_noise_ridged2(mu_noise_simplex2, x, y, seed, 5, 2.1f, 0.52f, 1.0f), 0.0f, 1.0f);
    case NK_WARPED:
        return MU_CLAMP(0.5f + 0.82f * mu_noise_domain_warped2_recursive(
            mu_noise_simplex2, mu_noise_perlin2, x, y, seed, 1.35f, 0.95f, 2), 0.0f, 1.0f);
    }
    return 0.5f;
}

static AnimVec3 panel_surf_fn(float u, float v, void *user)
{
    const PanelDesc *d   = (const PanelDesc *)user;
    float            val = eval_noise_01(d->kind, u * d->freq, v * d->freq, d->seed);
    return ANIM_V3(u * d->half_size, v * d->half_size, (val - 0.5f) * d->relief);
}

static AnimColor panel_color_fn(float u, float v, AnimVec3 pos, AnimVec3 normal, void *user)
{
    const PanelDesc *d   = (const PanelDesc *)user;
    float            val = eval_noise_01(d->kind, u * d->freq, v * d->freq, d->seed);
    (void)pos;
    (void)normal;
    if (d->kind == NK_WORLEY_F1 || d->kind == NK_WORLEY_EDGE || d->kind == NK_WORLEY_MANHATTAN)
        return cellular_color(val);
    if (d->kind == NK_FBM || d->kind == NK_RIDGED || d->kind == NK_WARPED)
        return terrain_color(val);
    return heatmap_color(val);
}

static AnimId make_noise_panel(AnimScene *s, const PanelDesc *desc, uint32_t steps,
                               AnimVec3 center, const char *title, const char *font_path,
                               AnimColor border_col, AnimId *out_frame, AnimId *out_lbl)
{
    AnimId surf = anim_surface3d(s, panel_surf_fn, panel_color_fn,
                                 -1.0f, 1.0f, -1.0f, 1.0f, steps, steps, (void *)desc);
    anim_move_to(s, surf, center);
    anim_surface_set_roughness(s, surf, 0.88f, false);
    anim_surface_set_wireframe(s, surf, 0.0f, 0.0f, false);

    AnimId frame = anim_rounded_rect(s, desc->half_size * 2.0f + 0.08f, desc->half_size * 2.0f + 0.08f, 0.08f);
    anim_move_to(s, frame, ANIM_V3(center.x, center.y, center.z + 0.08f));
    anim_set_fill(s, frame, anim_rgba(0, 0, 0, 0), false);
    anim_set_stroke(s, frame, border_col, 2.2f, false);

    AnimId lbl = anim_text(s, font_path, title, 0.24f);
    anim_move_to(s, lbl, ANIM_V3(center.x, center.y - desc->half_size - 0.28f, 0.0f));
    anim_set_fill(s, lbl, rgb(0.92f, 0.96f, 1.0f), false);

    if (out_frame) *out_frame = frame;
    if (out_lbl)   *out_lbl   = lbl;
    return surf;
}

static float slice_value_fn(float x, void *user)   { (void)user; return mu_noise_value2(x * 1.4f, 1.7f, 1337u); }
static float slice_perlin_fn(float x, void *user)  { (void)user; return mu_noise_perlin2(x * 1.4f, 1.7f, 1337u); }
static float slice_simplex_fn(float x, void *user) { (void)user; return mu_noise_simplex2(x * 1.4f, 1.7f, 1337u); }

static float curl_potential_fn(float x, float y, uint32_t seed)
{
    return mu_noise_fbm2(mu_noise_simplex2, x * 0.62f, y * 0.62f, seed, 3, 2.0f, 0.5f);
}

static float curl_implicit_fn(float x, float y, void *user)
{
    (void)user;
    return curl_potential_fn(x, y, 2026u);
}

static AnimVec3 curl_field_fn(AnimVec3 p, void *user)
{
    mu_noise_vec2 v;
    (void)user;
    mu_noise_curl2(v, curl_potential_fn, p.x, p.y, 2026u, 0.02f);
    return ANIM_V3(v[0] * 0.55f, v[1] * 0.55f, 0.0f);
}

#define NUM_TRACERS 20
typedef struct CurlCtx {
    AnimTrackerId tr_flow;
    AnimId        axes;
    AnimVec3      seeds[NUM_TRACERS];
    AnimId        dots[NUM_TRACERS];
} CurlCtx;

static void update_curl_tracers(AnimScene *s, AnimId target, float dt, void *user)
{
    CurlCtx *ctx   = (CurlCtx *)user;
    float    t     = anim_tracker_get(s, ctx->tr_flow);
    uint32_t steps = (uint32_t)MU_MAX((int)(t * 36.0f), 1);
    float    h     = (t > 1e-5f) ? (t / (float)steps) : 0.0f;
    uint32_t i;
    (void)target;
    (void)dt;

    for (i = 0u; i < NUM_TRACERS; ++i)
    {
        float    px = ctx->seeds[i].x;
        float    py = ctx->seeds[i].y;
        uint32_t k;
        for (k = 0u; k < steps; ++k)
        {
            mu_noise_vec2 v1, v2;
            float         mx, my;
            mu_noise_curl2(v1, curl_potential_fn, px, py, 2026u, 0.02f);
            mx = px + 0.5f * h * v1[0] * 0.85f;
            my = py + 0.5f * h * v1[1] * 0.85f;
            mu_noise_curl2(v2, curl_potential_fn, mx, my, 2026u, 0.02f);
            px += h * v2[0] * 0.85f;
            py += h * v2[1] * 0.85f;
        }
        anim_move_to(s, ctx->dots[i], anim_axes_c2p(s, ctx->axes, px, py));
    }
}

static void build_scene(AnimScene *s, AnimTimeline *tl, CurlCtx *cctx, const char *font_path)
{
    anim_scene_set_bg(s, anim_hex(0x000000), anim_hex(0x050812));

    AnimColor col_sky    = anim_hex(0x38BDF8);
    AnimColor col_green  = anim_hex(0x4ADE80);
    AnimColor col_rose   = anim_hex(0xFB7185);
    AnimColor col_gold   = anim_hex(0xF9E26B);
    AnimColor col_violet = anim_hex(0xA78BFA);

    AnimId title = anim_text(s, font_path, "Procedural Noise Algorithms (mu_noise_math.h)", 0.42f);
    anim_move_to(s, title, ANIM_V3(0.0f, 3.16f, 0.0f));
    anim_set_fill(s, title, rgb(0.94f, 0.97f, 1.0f), false);
    anim_set_glow(s, title, 0.18f, false);

    /* ==================== ACT 1: Lattice & Gradient Noise ==================== */
    static PanelDesc p_white   = { NK_WHITE,   1.12f, 3.2f, 0.015f, 1337u };
    static PanelDesc p_value   = { NK_VALUE,   1.12f, 3.2f, 0.015f, 1337u };
    static PanelDesc p_perlin  = { NK_PERLIN,  1.12f, 3.2f, 0.015f, 1337u };
    static PanelDesc p_simplex = { NK_SIMPLEX, 1.12f, 3.2f, 0.015f, 1337u };

    AnimId f_w, l_w, f_v, l_v, f_p, l_p, f_s, l_s;
    AnimId surf_w = make_noise_panel(s, &p_white,   36u, ANIM_V3(-4.15f,  1.15f, 0.0f), "White (Hash)",   font_path, anim_rgba(0.65f,0.75f,0.90f,0.7f), &f_w, &l_w);
    AnimId surf_v = make_noise_panel(s, &p_value,   36u, ANIM_V3(-1.35f,  1.15f, 0.0f), "Value Noise",    font_path, col_rose,   &f_v, &l_v);
    AnimId surf_p = make_noise_panel(s, &p_perlin,  36u, ANIM_V3(-4.15f, -1.65f, 0.0f), "Perlin (Grad)",  font_path, col_sky,    &f_p, &l_p);
    AnimId surf_s = make_noise_panel(s, &p_simplex, 36u, ANIM_V3(-1.35f, -1.65f, 0.0f), "Simplex 2D",     font_path, col_green,  &f_s, &l_s);

    AnimId card1 = anim_rounded_rect(s, 5.40f, 5.25f, 0.20f);
    anim_move_to(s, card1, ANIM_V3(3.15f, -0.25f, -0.15f));
    anim_set_fill(s, card1, anim_rgba(0.005f, 0.009f, 0.022f, 0.92f), false);
    anim_set_stroke(s, card1, anim_rgba(0.28f, 0.48f, 0.78f, 0.85f), 2.0f, false);

    AnimId act1_hdr = anim_text(s, font_path, "1. Lattice & Gradient Families", 0.31f);
    anim_move_to(s, act1_hdr, ANIM_V3(3.15f, 1.98f, 0.0f));
    anim_set_fill(s, act1_hdr, col_gold, false);

    AnimId act1_eq = anim_tex(s, ANIM_TEX_LATEX,
        "\\text{Perlin: } \\sum_i w_i (\\mathbf{g}_i \\cdot (\\mathbf{p}-\\mathbf{c}_i))", 0.44f);
    anim_move_to(s, act1_eq, ANIM_V3(3.15f, 1.32f, 0.0f));
    anim_set_fill(s, act1_eq, col_sky, true);

    AnimAxesDesc ax1_d = {
        .x_min = -3.0f, .x_max = 3.0f, .x_step = 1.0f,
        .y_min = -1.1f, .y_max = 1.1f, .y_step = 0.5f,
        .x_length = 4.45f, .y_length = 2.05f,
        .include_ticks = true, .include_tips = true,
        .include_grid = true, .include_numbers = false,
        .font_path = font_path
    };
    AnimId ax1 = anim_axes(s, &ax1_d);
    anim_move_to(s, ax1, ANIM_V3(3.15f, -0.45f, 0.0f));
    anim_set_stroke(s, ax1, anim_rgba(0.60f, 0.72f, 0.90f, 0.72f), 1.5f, true);

    AnimId c_val = anim_plot(s, ax1, slice_value_fn,   -3.0f, 3.0f, 90u, NULL);
    AnimId c_per = anim_plot(s, ax1, slice_perlin_fn,  -3.0f, 3.0f, 90u, NULL);
    AnimId c_sim = anim_plot(s, ax1, slice_simplex_fn, -3.0f, 3.0f, 90u, NULL);
    anim_set_fill(s, c_val, anim_rgba(0,0,0,0), false);
    anim_set_fill(s, c_per, anim_rgba(0,0,0,0), false);
    anim_set_fill(s, c_sim, anim_rgba(0,0,0,0), false);
    anim_set_stroke(s, c_val, col_rose,  2.4f, false);
    anim_set_stroke(s, c_per, col_sky,   2.8f, false);
    anim_set_stroke(s, c_sim, col_green, 2.8f, false);

    AnimId act1_cap = anim_text(s, font_path, "1D Slice: Value (rose) | Perlin (sky) | Simplex (green)", 0.21f);
    anim_move_to(s, act1_cap, ANIM_V3(3.15f, -2.18f, 0.0f));
    anim_set_fill(s, act1_cap, rgb(0.82f, 0.90f, 1.0f), false);

    anim_add(s, title); anim_add(s, card1);
    anim_add(s, surf_w); anim_add(s, f_w); anim_add(s, l_w);
    anim_add(s, surf_v); anim_add(s, f_v); anim_add(s, l_v);
    anim_add(s, surf_p); anim_add(s, f_p); anim_add(s, l_p);
    anim_add(s, surf_s); anim_add(s, f_s); anim_add(s, l_s);
    anim_add(s, act1_hdr); anim_add(s, act1_eq); anim_add(s, ax1);
    anim_add(s, c_val); anim_add(s, c_per); anim_add(s, c_sim); anim_add(s, act1_cap);
    {
        AnimSpec sp[] = {
            anim_anim_write(title),
            fade_in(card1),
            fade_in(surf_w), fade_in(f_w), fade_in(l_w),
            fade_in(surf_v), fade_in(f_v), fade_in(l_v),
            fade_in(surf_p), fade_in(f_p), fade_in(l_p),
            fade_in(surf_s), fade_in(f_s), fade_in(l_s),
            anim_anim_write(act1_hdr), anim_anim_write(act1_eq),
            fade_in(ax1), fade_in(act1_cap),
            anim_anim_create(c_val), anim_anim_create(c_per), anim_anim_create(c_sim),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.85f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 2.85f);
    {
        AnimSpec sp[] = {
            fade_out(surf_w), fade_out(f_w), fade_out(l_w),
            fade_out(surf_v), fade_out(f_v), fade_out(l_v),
            fade_out(surf_p), fade_out(f_p), fade_out(l_p),
            fade_out(surf_s), fade_out(f_s), fade_out(l_s),
            fade_out(card1), fade_out(act1_hdr), fade_out(act1_eq),
            fade_out(ax1), fade_out(c_val), fade_out(c_per),
            fade_out(c_sim), fade_out(act1_cap),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 0.65f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, surf_w); anim_remove(s, f_w); anim_remove(s, l_w);
    anim_remove(s, surf_v); anim_remove(s, f_v); anim_remove(s, l_v);
    anim_remove(s, surf_p); anim_remove(s, f_p); anim_remove(s, l_p);
    anim_remove(s, surf_s); anim_remove(s, f_s); anim_remove(s, l_s);
    anim_remove(s, card1); anim_remove(s, act1_hdr); anim_remove(s, act1_eq);
    anim_remove(s, ax1); anim_remove(s, c_val); anim_remove(s, c_per);
    anim_remove(s, c_sim); anim_remove(s, act1_cap);

    /* ==================== ACT 2: Worley / Cellular Noise ==================== */
    static PanelDesc p_wf1   = { NK_WORLEY_F1,        1.65f, 2.6f, 0.015f, 42u };
    static PanelDesc p_wedge = { NK_WORLEY_EDGE,      1.65f, 2.6f, 0.015f, 42u };
    static PanelDesc p_wman  = { NK_WORLEY_MANHATTAN, 1.65f, 2.6f, 0.015f, 42u };

    AnimId f_wf1, l_wf1, f_wedge, l_wedge, f_wman, l_wman;
    AnimId s_wf1   = make_noise_panel(s, &p_wf1,   44u, ANIM_V3(-3.95f,  0.30f, 0.0f), "Worley F1 (Euclidean)",  font_path, col_violet, &f_wf1,   &l_wf1);
    AnimId s_wedge = make_noise_panel(s, &p_wedge, 44u, ANIM_V3( 0.00f,  0.30f, 0.0f), "Voronoi Edge (F2 - F1)", font_path, col_rose,   &f_wedge, &l_wedge);
    AnimId s_wman  = make_noise_panel(s, &p_wman,  44u, ANIM_V3( 3.95f,  0.30f, 0.0f), "Manhattan Metric F1",    font_path, col_gold,   &f_wman,  &l_wman);

    AnimId act2_hdr = anim_text(s, font_path, "2. Worley Cellular & Distance Metrics", 0.34f);
    anim_move_to(s, act2_hdr, ANIM_V3(0.0f, 2.42f, 0.0f));
    anim_set_fill(s, act2_hdr, col_gold, false);

    AnimId act2_eq = anim_tex(s, ANIM_TEX_LATEX,
        "F_1(\\mathbf{p}) = \\min_i \\|\\mathbf{p}-\\mathbf{s}_i\\|, \\qquad \\text{edge}(\\mathbf{p}) = F_2 - F_1", 0.56f);
    anim_move_to(s, act2_eq, ANIM_V3(0.0f, -2.42f, 0.0f));
    anim_set_fill(s, act2_eq, rgb(0.88f, 0.94f, 1.0f), true);

    anim_add(s, act2_hdr); anim_add(s, act2_eq);
    anim_add(s, s_wf1);   anim_add(s, f_wf1);   anim_add(s, l_wf1);
    anim_add(s, s_wedge); anim_add(s, f_wedge); anim_add(s, l_wedge);
    anim_add(s, s_wman);  anim_add(s, f_wman);  anim_add(s, l_wman);
    {
        AnimSpec sp[] = {
            anim_anim_write(act2_hdr),
            fade_in(s_wf1),   fade_in(f_wf1),   fade_in(l_wf1),
            fade_in(s_wedge), fade_in(f_wedge), fade_in(l_wedge),
            fade_in(s_wman),  fade_in(f_wman),  fade_in(l_wman),
            anim_anim_write(act2_eq),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.45f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 2.75f);
    {
        AnimSpec sp[] = {
            fade_out(act2_hdr), fade_out(act2_eq),
            fade_out(s_wf1),   fade_out(f_wf1),   fade_out(l_wf1),
            fade_out(s_wedge), fade_out(f_wedge), fade_out(l_wedge),
            fade_out(s_wman),  fade_out(f_wman),  fade_out(l_wman),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 0.65f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, act2_hdr); anim_remove(s, act2_eq);
    anim_remove(s, s_wf1);   anim_remove(s, f_wf1);   anim_remove(s, l_wf1);
    anim_remove(s, s_wedge); anim_remove(s, f_wedge); anim_remove(s, l_wedge);
    anim_remove(s, s_wman);  anim_remove(s, f_wman);  anim_remove(s, l_wman);

    /* ==================== ACT 3: 3D PBR Fractal Terrains ==================== */
    static PanelDesc p_fbm    = { NK_FBM,    2.45f, 1.95f, 1.55f, 701u };
    static PanelDesc p_ridged = { NK_RIDGED, 2.45f, 1.95f, 1.65f, 701u };
    static PanelDesc p_warped = { NK_WARPED, 2.45f, 1.85f, 1.55f, 701u };

    AnimId t_fbm    = anim_surface3d(s, panel_surf_fn, panel_color_fn, -1, 1, -1, 1, 56u, 56u, &p_fbm);
    AnimId t_ridged = anim_surface3d(s, panel_surf_fn, panel_color_fn, -1, 1, -1, 1, 56u, 56u, &p_ridged);
    AnimId t_warped = anim_surface3d(s, panel_surf_fn, panel_color_fn, -1, 1, -1, 1, 56u, 56u, &p_warped);

    AnimId terrains[3] = { t_fbm, t_ridged, t_warped };
    for (uint32_t ti = 0u; ti < 3u; ++ti)
    {
        anim_move_to(s, terrains[ti], ANIM_V3(-2.35f, -0.35f, 0.0f));
        anim_rotate(s, terrains[ti], -0.92f, ANIM_RIGHT);
        anim_rotate(s, terrains[ti],  0.42f, ANIM_UP);
        anim_surface_set_roughness(s, terrains[ti], 0.42f, false);
        anim_surface_set_wireframe(s, terrains[ti], 22.0f, 0.14f, false);
    }

    AnimId card3 = anim_rounded_rect(s, 5.25f, 4.95f, 0.20f);
    anim_move_to(s, card3, ANIM_V3(3.30f, -0.25f, -0.15f));
    anim_set_fill(s, card3, anim_rgba(0.005f, 0.009f, 0.022f, 0.92f), false);
    anim_set_stroke(s, card3, anim_rgba(0.28f, 0.48f, 0.78f, 0.85f), 2.0f, false);

    AnimId act3_hdr = anim_text(s, font_path, "3. 3D Fractal Terrain Synthesis", 0.31f);
    anim_move_to(s, act3_hdr, ANIM_V3(3.30f, 1.80f, 0.0f));
    anim_set_fill(s, act3_hdr, col_gold, false);

    AnimId eq_fbm = anim_tex(s, ANIM_TEX_LATEX,
        "\\text{fBm: } \\textstyle\\sum_{k=0}^{4} g^k\\, n(L^k \\mathbf{p})", 0.48f);
    anim_move_to(s, eq_fbm, ANIM_V3(3.30f, 0.88f, 0.0f));
    anim_set_fill(s, eq_fbm, col_green, true);

    AnimId eq_rid = anim_tex(s, ANIM_TEX_LATEX,
        "\\text{Ridged: } \\textstyle\\sum_{k=0}^{4} w_k\\,(1 - |n(L^k \\mathbf{p})|)^2", 0.42f);
    anim_move_to(s, eq_rid, ANIM_V3(3.30f, -0.30f, 0.0f));
    anim_set_fill(s, eq_rid, col_rose, true);

    AnimId eq_wrp = anim_tex(s, ANIM_TEX_LATEX,
        "\\text{Warp: } n\\big(\\mathbf{p} + a\\,\\mathbf{W}(\\mathbf{p} + b\\,\\mathbf{W}(\\mathbf{p}))\\big)", 0.40f);
    anim_move_to(s, eq_wrp, ANIM_V3(3.30f, -1.50f, 0.0f));
    anim_set_fill(s, eq_wrp, col_sky, true);

    anim_add(s, card3); anim_add(s, act3_hdr);
    anim_add(s, t_fbm); anim_add(s, eq_fbm);
    {
        AnimSpec sp[] = {
            fade_in(card3), anim_anim_write(act3_hdr),
            fade_in(t_fbm), anim_anim_write(eq_fbm),
            anim_anim_indicate(eq_fbm, 1.10f, col_green),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.35f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.70f);

    anim_add(s, t_ridged); anim_add(s, eq_rid);
    {
        AnimSpec sp[] = {
            fade_out(t_fbm),
            fade_in(t_ridged),
            anim_anim_write(eq_rid),
            anim_anim_indicate(eq_rid, 1.10f, col_rose),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.15f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, t_fbm);
    anim_wait(tl, s, 0.70f);

    anim_add(s, t_warped); anim_add(s, eq_wrp);
    {
        AnimSpec sp[] = {
            fade_out(t_ridged),
            fade_in(t_warped),
            anim_anim_write(eq_wrp),
            anim_anim_indicate(eq_wrp, 1.10f, col_sky),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.15f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, t_ridged);
    anim_wait(tl, s, 0.85f);

    {
        AnimSpec sp[] = {
            fade_out(t_warped), fade_out(card3),
            fade_out(act3_hdr), fade_out(eq_fbm),
            fade_out(eq_rid),   fade_out(eq_wrp),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 0.65f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, t_warped); anim_remove(s, card3);
    anim_remove(s, act3_hdr); anim_remove(s, eq_fbm);
    anim_remove(s, eq_rid);   anim_remove(s, eq_wrp);

    /* ==================== ACT 4: Divergence-Free Curl Flow ==================== */
    cctx->tr_flow = anim_tracker_create(s, 0.0f);

    AnimAxesDesc ax4_d = {
        .x_min = -3.4f, .x_max = 3.4f, .x_step = 1.0f,
        .y_min = -2.1f, .y_max = 2.1f, .y_step = 1.0f,
        .x_length = 8.4f, .y_length = 4.9f,
        .include_ticks = true, .include_tips = false,
        .include_grid = false, .include_numbers = false,
        .font_path = font_path
    };
    cctx->axes = anim_axes(s, &ax4_d);
    anim_move_to(s, cctx->axes, ANIM_V3(-1.45f, -0.22f, 0.0f));
    anim_set_stroke(s, cctx->axes, anim_rgba(0.55f, 0.68f, 0.88f, 0.55f), 1.4f, true);

    static const float levels[7] = { -0.55f, -0.35f, -0.15f, 0.0f, 0.15f, 0.35f, 0.55f };
    AnimId contours = anim_contour_plot(s, cctx->axes, curl_implicit_fn,
                                        -3.2f, 3.2f, -2.0f, 2.0f, 44u, 30u,
                                        levels, 7u,
                                        anim_rgba(0.16f, 0.30f, 0.68f, 0.28f),
                                        anim_rgba(0.22f, 0.74f, 0.97f, 0.34f), NULL);

    AnimId vfield = anim_vector_field(s, cctx->axes, curl_field_fn, 0.65f, 0.55f, 0.42f, NULL);
    anim_set_color(s, vfield, anim_rgba(0.55f, 0.75f, 0.95f, 0.38f), true);
    anim_set_stroke(s, vfield, anim_rgba(0.55f, 0.75f, 0.95f, 0.38f), 1.4f, true);

    AnimId streams = anim_stream_lines(s, cctx->axes, curl_field_fn,
                                       -3.1f, 3.1f, -1.9f, 1.9f, 0.95f, 0.78f, 22u, NULL);
    anim_set_stroke(s, streams, anim_rgba(0.29f, 0.87f, 0.50f, 0.68f), 2.0f, true);

    AnimId trails[NUM_TRACERS];
    for (uint32_t i = 0u; i < NUM_TRACERS; ++i)
    {
        uint32_t col = i % 5u;
        uint32_t row = i / 5u;
        float sx = -2.45f + (float)col * 1.10f + ((row % 2u) ? 0.22f : -0.20f);
        float sy = -1.45f + (float)row * 0.95f;
        cctx->seeds[i] = ANIM_V3(sx, sy, 0.0f);
        cctx->dots[i]  = anim_dot(s, anim_axes_c2p(s, cctx->axes, sx, sy), 0.075f);
        anim_set_fill(s, cctx->dots[i], col_gold, false);
        anim_set_glow(s, cctx->dots[i], 0.85f, false);
        trails[i] = anim_traced_path(s, cctx->dots[i], anim_rgba(0.98f, 0.89f, 0.42f, 0.85f), 2.4f, 64u);
    }

    AnimId card4 = anim_rounded_rect(s, 3.65f, 4.90f, 0.20f);
    anim_move_to(s, card4, ANIM_V3(4.52f, -0.22f, -0.15f));
    anim_set_fill(s, card4, anim_rgba(0.005f, 0.009f, 0.022f, 0.92f), false);
    anim_set_stroke(s, card4, anim_rgba(0.28f, 0.48f, 0.78f, 0.85f), 2.0f, false);

    AnimId act4_hdr = anim_text(s, font_path, "4. 2D Curl Noise", 0.31f);
    anim_move_to(s, act4_hdr, ANIM_V3(4.52f, 1.78f, 0.0f));
    anim_set_fill(s, act4_hdr, col_gold, false);

    AnimId eq_curl = anim_tex(s, ANIM_TEX_LATEX,
        "\\mathbf{v} = \\begin{bmatrix} \\partial_y \\psi \\\\ -\\partial_x \\psi \\end{bmatrix}", 0.96f);
    anim_move_to(s, eq_curl, ANIM_V3(4.52f, 0.45f, 0.0f));
    anim_set_fill(s, eq_curl, col_green, true);

    AnimId eq_div = anim_tex(s, ANIM_TEX_LATEX,
        "\\nabla \\cdot \\mathbf{v} = 0", 0.56f);
    anim_move_to(s, eq_div, ANIM_V3(4.52f, -0.95f, 0.0f));
    anim_set_fill(s, eq_div, col_sky, true);

    AnimId act4_note = anim_text(s, font_path, "Incompressible fluid eddies", 0.22f);
    anim_move_to(s, act4_note, ANIM_V3(4.52f, -1.78f, 0.0f));
    anim_set_fill(s, act4_note, rgb(0.82f, 0.90f, 1.0f), false);

    anim_add_updater(s, cctx->dots[0], update_curl_tracers, cctx, NULL, 0u, &cctx->tr_flow, 1u);

    anim_add(s, cctx->axes); anim_add(s, contours); anim_add(s, vfield); anim_add(s, streams);
    anim_add(s, card4); anim_add(s, act4_hdr); anim_add(s, eq_curl); anim_add(s, eq_div); anim_add(s, act4_note);
    for (uint32_t i = 0u; i < NUM_TRACERS; ++i)
    {
        anim_add(s, cctx->dots[i]);
        anim_add(s, trails[i]);
    }
    {
        AnimSpec sp[10 + NUM_TRACERS];
        uint32_t k = 0u;
        sp[k++] = fade_in(cctx->axes);
        sp[k++] = fade_in(contours);
        sp[k++] = fade_in(vfield);
        sp[k++] = anim_anim_create(streams);
        sp[k++] = fade_in(card4);
        sp[k++] = anim_anim_write(act4_hdr);
        sp[k++] = anim_anim_write(eq_curl);
        sp[k++] = anim_anim_write(eq_div);
        sp[k++] = fade_in(act4_note);
        for (uint32_t i = 0u; i < NUM_TRACERS; ++i)
            sp[k++] = anim_anim_grow_from_center(cctx->dots[i]);
        anim_play(tl, s, sp, k, 1.20f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec sp[] = { anim_anim_tracker_to(cctx->tr_flow, 2.20f) };
        anim_play(tl, s, sp, 1u, 3.20f, ANIM_RATE_LINEAR);
    }
    anim_wait(tl, s, 0.55f);
}

int main(int argc, char **argv)
{
    uint32_t       w = 1280u, h = 720u, fps = 60u, blur = 2u;
    const char    *shot_path  = NULL;
    const char    *video_path = NULL;
    float          shot_time  = -1.0f;
    bool           validate   = false;
    int            i, rc = 0;

    Platform      *platform;
    Gpu           *gpu;
    AnimRenderer  *ren;
    AnimScene     *scene;
    AnimTimeline  *tl;
    AnimRenderOpts opts;
    CurlCtx        cctx = { 0 };

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)        shot_path  = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) video_path = argv[++i];
        else if ((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "--t") == 0) && i + 1 < argc)
            shot_time = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)  w          = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) h          = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--lowres") == 0)                 { w = 854u; h = 480u; fps = 30u; blur = 1u; }
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)   fps        = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)  blur       = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate = atoi(argv[++i]) != 0;
    }

    scene = anim_scene_create();
    tl    = anim_timeline_create();
    build_scene(scene, tl, &cctx, find_font_path());
    log_info("noise_algorithms_viz duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim - Noise Algorithms",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "noise_viz_demo",
        .width                = w,
        .height               = h,
        .enable_validation    = validate,
        .size_of_cpu_pool     = MU_MB(32),
        .size_of_gpu_pool     = MU_MB(32),
        .size_of_staging_pool = MU_MB(48),
        .vsync                = true,
    });
    if (!shot_path && !video_path)
        gpu_set_surface(gpu, platform_create_surface(platform, gpu_raw_instance(gpu)));

    ren  = anim_renderer_create(gpu, w, h);
    opts = anim_render_opts_default(w, h);
    opts.fps                 = fps;
    opts.motion_blur_samples = MU_MAX(blur, 1u);
    opts.tonemap             = ANIM_TONEMAP_AGX;
    opts.aberration_px       = 0.40f;
    opts.grain_amount        = 0.0012f;

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path)) rc = 1;
    }
    else if (video_path)
    {
        if (!anim_export_video(ren, tl, scene, &opts, video_path, NULL)) rc = 1;
    }
    else
    {
        anim_run_interactive(platform, gpu, ren, tl, scene, opts);
    }

    gpu_wait_idle(gpu);
    if (gpu_error(gpu)[0] != '\0')
    {
        log_error("Vulkan error: %s", gpu_error(gpu));
        rc = 1;
    }
    anim_renderer_destroy(ren);
    gpu_destroy(gpu);
    platform_destroy(platform);
    anim_timeline_destroy(tl);
    anim_scene_destroy(scene);
    return rc;
}
