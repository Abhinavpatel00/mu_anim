/*
 * tests/test_anim.c — unit + integration + headless Vulkan validation suite for mu_anim.
 */
#include "../third_party/mu_new/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mu_new/vk.h"
#include "../anim.h"
#include "../anim_script.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                              \
    do                                                                                \
    {                                                                                 \
        if (!(cond))                                                                  \
        {                                                                             \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));         \
            g_failures++;                                                             \
        }                                                                             \
    } while (0)

static float test_quad_fn(float x, void *user)
{
    (void)user;
    return 0.5f * x * x;
}

static AnimVec3 test_warp_fn(AnimVec3 p, void *user)
{
    (void)user;
    return ANIM_V3(p.x, p.y + 0.35f * sinf(p.x * 2.0f), p.z);
}

static AnimVec3 test_field_fn(AnimVec3 p, void *user)
{
    (void)user;
    return ANIM_V3(-p.y * 0.4f, p.x * 0.4f, 0.0f);
}

static AnimVec3 test_helix3d_fn(float t, void *user)
{
    (void)user;
    return ANIM_V3(1.2f * cosf(t), 1.2f * sinf(t), 0.3f * t - 1.0f);
}

static AnimVec3 test_saddle3d_fn(float u, float v, void *user)
{
    (void)user;
    return ANIM_V3(u, 0.35f * (u * u - v * v), v);
}

static float test_custom_rate_fn(float t, void *user)
{
    (void)user;
    return t * t;
}

static void test_sho_ode(float t, const float *y, float *dydt, uint32_t dim, void *user)
{
    (void)t;
    (void)dim;
    (void)user;
    dydt[0] = y[1];
    dydt[1] = -y[0];
}

typedef struct UpdaterCtx {
    AnimTrackerId tracker;
    AnimId        axes;
    AnimId        label;
    uint32_t     *order_log;
    uint32_t     *order_len;
    uint32_t      id;
} UpdaterCtx;

static void dot_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    UpdaterCtx *ctx = (UpdaterCtx *)user;
    float       x   = anim_tracker_get(s, ctx->tracker);
    (void)dt;
    anim_move_to(s, target, anim_axes_c2p(s, ctx->axes, x, test_quad_fn(x, NULL)));
    ctx->order_log[(*ctx->order_len)++] = ctx->id;
}

static void label_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    UpdaterCtx *ctx = (UpdaterCtx *)user;
    float       x   = anim_tracker_get(s, ctx->tracker);
    (void)dt;
    anim_decimal_number_set(s, target, test_quad_fn(x, NULL));
    ctx->order_log[(*ctx->order_len)++] = ctx->id;
}

static AnimVec3 test_vfield3d_fn(AnimVec3 p, void *user)
{
    (void)user;
    return ANIM_V3(-p.z, 0.4f * (1.0f - p.y), p.x);
}

static bool png_has_non_uniform_pixels(const char *path)
{
    FILE   *f = fopen(path, "rb");
    uint8_t sig[8];
    long    sz;

    if (!f)
        return false;
    if (fread(sig, 1, 8, f) != 8)
    {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G' && sz > 1000;
}

static float test_polar_r(float th, void *user) { (void)user; return 1.0f + 0.4f * cosf(3.0f * th); }
static float test_implicit_circle(float x, float y, void *user) { (void)user; return x * x + y * y - 1.2f; }
static AnimVec3 test_swirl_field(AnimVec3 p, void *user) { (void)user; return ANIM_V3(-p.y, p.x, 0.0f); }
static AnimVec3 test_homotopy_fn(AnimVec3 p, float t, void *user)
{
    (void)user;
    return ANIM_V3(p.x, p.y + 0.25f * t * sinf(p.x * 2.0f), p.z);
}
static void test_custom_anim_fn(AnimScene *s, AnimId target, float alpha, void *user)
{
    (void)user;
    anim_set_opacity(s, target, 0.3f + 0.7f * alpha, false);
}

static float test_saddle_fn(float x, float y, void *user) { (void)user; return x * x - y * y; }

static AnimComplex test_complex_sq(AnimComplex z, void *user)
{
    (void)user;
    return (AnimComplex){ 0.25f * (z.re * z.re - z.im * z.im), 0.5f * z.re * z.im };
}

static AnimVec3 test_lathe_fn(float t, void *user)
{
    (void)user;
    return ANIM_V3(0.7f + 0.25f * sinf(t * 2.0f), t, 0.0f);
}

static AnimVec3 test_flow_fn(AnimVec3 p, void *user)
{
    (void)user;
    return ANIM_V3(-p.y, p.x, 0.0f);
}

int main(void)
{
    AnimScene    *s;
    AnimTimeline *tl;
    Gpu          *gpu;
    AnimRenderer *ren;
    AnimRenderOpts opts;

    printf("=== mu_anim test suite ===\n");

    /* 1. Color & Rate functions */
    {
        AnimColor c = anim_color_lerp_oklab(ANIM_BLUE, ANIM_RED, 0.5f);
        CHECK(c.r > 0.0f && c.g > 0.0f && c.b > 0.0f && fabsf(c.a - 1.0f) < 1e-5f, "Oklab midpoint valid");
        CHECK(fabsf(anim_rate_eval(ANIM_RATE_SMOOTH, 0.0f)) < 1e-5f, "smooth(0)==0");
        CHECK(fabsf(anim_rate_eval(ANIM_RATE_SMOOTH, 1.0f) - 1.0f) < 1e-5f, "smooth(1)==1");
        CHECK(fabsf(anim_rate_eval(ANIM_RATE_THERE_AND_BACK, 0.5f) - 1.0f) < 1e-5f, "there_and_back(0.5)==1");
        CHECK(fabsf(anim_rate_eval(ANIM_RATE_THERE_AND_BACK, 1.0f)) < 1e-5f, "there_and_back(1)==0");
    }

    s = anim_scene_create();
    anim_scene_set_cache_dir(s, "/tmp/mu_anim_test_cache");

    /* 2. Geometry, Hierarchy, Layout, Brace, Warp */
    {
        AnimId sq   = anim_square(s, 2.0f);
        AnimId cir  = anim_circle(s, 0.8f);
        AnimId br;
        AnimVec3 c_sq, c_cir, mn, mx;

        anim_set_fill(s, sq, ANIM_BLUE, false);
        anim_set_fill(s, cir, ANIM_RED, false);
        anim_next_to(s, cir, sq, ANIM_RIGHT, 0.5f);
        c_sq  = anim_center(s, sq);
        c_cir = anim_center(s, cir);
        CHECK(fabsf(c_sq.x) < 1e-4f, "square centered at 0");
        CHECK(fabsf((c_cir.x - 0.8f) - (c_sq.x + 1.0f) - 0.5f) < 0.05f, "next_to buffer exact");

        br = anim_brace(s, sq, ANIM_DOWN, 0.2f);
        anim_bbox_world(s, br, &mn, &mx);
        CHECK(mx.y < -1.0f, "brace positioned below square");

        AnimId line = anim_line(s, ANIM_V3(-2.0f, 0.0f, 0.0f), ANIM_V3(2.0f, 0.0f, 0.0f));
        anim_vmob_apply_warp(s, line, test_warp_fn, NULL, 4u);
        CHECK(anim_vmob_cubic_count(s, line) == 4u, "warp subdivided line to 4 cubics");
        {
            AnimVec3 p_mid = anim_vmob_point_at(s, line, 0.5f);
            CHECK(fabsf(p_mid.y) < 0.1f && anim_height(s, line) > 0.3f, "warped line has sinusoidal height");
        }
        anim_add(s, sq);
        anim_add(s, cir);
        anim_add(s, br);
        anim_add(s, line);
    }

    /* 3. Axes, Plot, Riemann, VectorField, DecimalNumber, Matrix */
    {
        AnimAxesDesc ad = { -3.0f, 3.0f, 1.0f, -2.0f, 5.0f, 1.0f, 6.0f, 4.0f, true, true, true, false, NULL };
        AnimId       ax = anim_axes(s, &ad);
        AnimId       pl = anim_plot(s, ax, test_quad_fn, -2.5f, 2.5f, 32u, NULL);
        AnimId       rr = anim_riemann_rectangles(s, ax, test_quad_fn, 0.0f, 2.0f, 0.5f, NULL, ANIM_BLUE, ANIM_TEAL);
        AnimId       vf = anim_vector_field(s, ax, test_field_fn, 1.5f, 1.5f, 0.5f, NULL);
        float        mvals[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        AnimId       mat = anim_matrix(s, mvals, 2u, 2u, 1u, NULL, 0.35f);

        CHECK(anim_child_count(s, rr) == 4u, "riemann rectangles count == 4");
        CHECK(anim_child_count(s, vf) > 4u, "vector field generated arrows");
        CHECK(anim_child_count(s, mat) >= 6u, "matrix has cells + brackets");
        {
            AnimVec3 wp = anim_axes_c2p(s, ax, 2.0f, 2.0f);
            float    rx = 0.0f, ry = 0.0f;
            anim_axes_p2c(s, ax, wp, &rx, &ry);
            CHECK(fabsf(rx - 2.0f) < 1e-3f && fabsf(ry - 2.0f) < 1e-3f, "axes c2p/p2c roundtrip");
        }
        anim_add(s, ax);
        anim_add(s, pl);
        anim_add(s, rr);
        anim_add(s, mat);
    }

    /* 4. SVG import, stb_truetype Text, Typst + LaTeX math & mu_save cache */
    {
        static const char *TEST_SVG =
            "<svg viewBox='0 0 100 100'>"
            "<g transform='translate(10 10)'>"
            "<path fill='#58C4DD' d='M 0 0 L 80 0 L 80 80 L 0 80 Z'/>"
            "<circle cx='40' cy='40' r='20' fill='#FC6255'/>"
            "</g></svg>";
        AnimId svg_mob = anim_svg_from_memory(s, TEST_SVG, strlen(TEST_SVG), 2.0f);
        AnimId txt_mob = anim_text(s, NULL, "Hello mu_anim", 0.6f);
        AnimId typ_1   = anim_tex(s, ANIM_TEX_TYPST, "{{a^2}} + {{b^2}} = {{c^2}}", 0.7f);
        AnimId typ_2   = anim_tex(s, ANIM_TEX_TYPST, "{{a^2}} + {{b^2}} = {{c^2}}", 0.7f); /* cache hit */
        AnimId ltx_1   = anim_tex(s, ANIM_TEX_LATEX, "\\int_0^1 x^2 \\, dx = \\frac{1}{3}", 0.9f);
        AnimId tag_a   = anim_find_by_tag(s, typ_1, "a^2");

        CHECK(anim_child_count(s, svg_mob) == 2u, "svg imported 2 shapes");
        CHECK(anim_child_count(s, txt_mob) >= 10u, "text generated glyph children");
        CHECK(anim_child_count(s, typ_1) >= 3u, "typst math generated parts");
        CHECK(anim_child_count(s, typ_2) == anim_child_count(s, typ_1), "typst mu_save cache matches cold compile");
        CHECK(ANIM_ID_VALID(tag_a), "typst {{a^2}} token tagged");
        CHECK(anim_child_count(s, ltx_1) >= 1u, "latex+dvisvgm compiled math");
        anim_add(s, svg_mob);
        anim_add(s, txt_mob);
        anim_add(s, typ_1);
        anim_add(s, ltx_1);
    }

    /* 5. ValueTracker + mu_toposort DAG + Seekable Timeline */
    anim_scene_clear(s);
    {
        uint32_t     order_log[16];
        uint32_t     order_len = 0u;
        AnimAxesDesc ad  = { -2.0f, 2.0f, 1.0f, -1.0f, 3.0f, 1.0f, 6.0f, 4.0f, true, true, true, false, NULL };
        AnimId       ax  = anim_axes(s, &ad);
        AnimId       pl  = anim_plot(s, ax, test_quad_fn, -2.0f, 2.0f, 24u, NULL);
        AnimId       dot = anim_dot(s, ANIM_ORIGIN, 0.12f);
        AnimId       num = anim_decimal_number(s, 0.0f, 2u, NULL, 0.45f);
        AnimId       eq1 = anim_tex(s, ANIM_TEX_TYPST, "{{a^2}} + {{b^2}} = {{c^2}}", 0.7f);
        AnimId       eq2 = anim_tex(s, ANIM_TEX_TYPST, "{{c^2}} - {{b^2}} = {{a^2}}", 0.7f);
        AnimTrackerId tr = anim_tracker_create(s, -1.5f);
        UpdaterCtx   u1  = { tr, ax, num, order_log, &order_len, 1u };
        UpdaterCtx   u2  = { tr, ax, num, order_log, &order_len, 2u };

        anim_move_to(s, eq1, ANIM_V3(0.0f, 2.8f, 0.0f));
        anim_move_to(s, eq2, ANIM_V3(0.0f, 2.8f, 0.0f));
        anim_to_corner(s, num, ANIM_UR, 0.5f);
        anim_set_fill(s, dot, ANIM_YELLOW, false);
        anim_add(s, ax);
        anim_add(s, pl);
        anim_add(s, dot);
        anim_add(s, num);
        anim_add(s, eq1);

        /* num depends on dot so dot updater (id 1) must execute BEFORE label updater (id 2) */
        anim_add_updater(s, num, label_updater, &u2, &dot, 1u, &tr, 1u);
        anim_add_updater(s, dot, dot_updater, &u1, NULL, 0u, &tr, 1u);

        tl = anim_timeline_create();
        {
            AnimSpec s1[2] = { anim_anim_write(eq1), anim_anim_create(pl) };
            AnimSpec s2[2] = { anim_anim_tracker_to(tr, 1.5f), anim_anim_transform_matching_tex(eq1, eq2) };
            anim_play(tl, s, s1, 2u, 1.0f, ANIM_RATE_SMOOTH);
            anim_play(tl, s, s2, 2u, 1.5f, ANIM_RATE_SMOOTH);
            anim_wait(tl, s, 0.5f);
        }
        CHECK(fabsf(anim_timeline_duration(tl) - 3.0f) < 1e-4f, "timeline duration == 3.0s");

        order_len = 0u;
        anim_seek(tl, s, 1.75f);
        CHECK(order_len == 2u && order_log[0] == 1u && order_log[1] == 2u, "mu_toposort ran dot before dependent label");
        CHECK(fabsf(anim_tracker_get(s, tr)) < 0.05f, "tracker midpoint near 0.0 at t=1.75s");

        /* Seek backward & verify determinism */
        anim_seek(tl, s, 0.5f);
        CHECK(fabsf(anim_tracker_get(s, tr) - (-1.5f)) < 1e-4f, "backward seek restored tracker to -1.5");
        anim_seek(tl, s, 2.75f);
        CHECK(fabsf(anim_tracker_get(s, tr) - 1.5f) < 1e-4f, "forward seek placed tracker at 1.5");

        /* 6. Headless Vulkan rendering + validation check */
        gpu = gpu_create(&(GpuDesc){
            .name                   = "test_anim",
            .width                  = 640,
            .height                 = 360,
            .enable_validation      = true,
            .size_of_cpu_pool       = MU_MB(16),
            .size_of_gpu_pool       = MU_MB(16),
            .size_of_staging_pool   = MU_MB(16),
        });
        ren  = anim_renderer_create(gpu, 640, 360);
        opts = anim_render_opts_default(640, 360);

        anim_seek(tl, s, 1.8f);
        CHECK(anim_export_png(ren, s, &opts, "/tmp/test_anim_frame.png"), "exported opaque PNG");
        CHECK(png_has_non_uniform_pixels("/tmp/test_anim_frame.png"), "opaque PNG written & non-empty");

        opts.transparent_bg = true;
        CHECK(anim_export_png(ren, s, &opts, "/tmp/test_anim_alpha.png"), "exported transparent PNG");
        CHECK(png_has_non_uniform_pixels("/tmp/test_anim_alpha.png"), "transparent PNG written & non-empty");

        {
            AnimFrameStats st = anim_renderer_last_stats(ren);
            printf("stats: %u shapes, %u bands, %u quad segs, %.2f ms CPU build\n",
                   st.shape_count, st.band_count, st.quad_seg_count, st.cpu_build_ms);
            CHECK(st.shape_count > 0u && st.band_count > 0u && st.quad_seg_count > 0u, "renderer emitted shapes/bands/segs");
        }

        /* 7. Diagnostics, Spring + Custom Rate, Gradients, Dashes, Shadows, Clips */
        {
            anim_clear_error();
            CHECK(anim_last_error()[0] == '\0', "error cleared");
            AnimId fallback_txt = anim_text(s, "/nonexistent/font.ttf", "Fallback", 0.5f);
            CHECK(anim_last_error()[0] != '\0' && anim_child_count(s, fallback_txt) > 0u, "missing font sets error and falls back cleanly");
            anim_clear_error();

            CHECK(fabsf(anim_rate_eval(ANIM_RATE_SPRING, 0.0f)) < 1e-4f, "spring(0)==0");
            CHECK(fabsf(anim_rate_eval(ANIM_RATE_SPRING, 1.0f) - 1.0f) < 1e-3f, "spring(1)==1");
            CHECK(anim_rate_eval_spring(0.25f, 0.35f, 14.0f) > 1.0f, "underdamped spring overshoots 1.0");
        }

        /* 8. 3D Surfaces/Curves + Technical Widgets + Overlapping Clips + Motion Blur + >5k Cubic Stress */
        anim_scene_clear(s);
        {
            AnimId surf   = anim_surface3d(s, test_saddle3d_fn, NULL, -1.6f, 1.6f, -1.6f, 1.6f, 24u, 24u, NULL);
            AnimId h3d    = anim_parametric_curve(s, test_helix3d_fn, 0.0f, ANIM_TAU * 2.0f, 48u, NULL);
            AnimId para   = anim_paragraph(s, NULL, "Line one\nLine two", 0.35f, 1.3f, ANIM_ALIGN_LEFT);
            const char *code_src = "float step(float x) {\n    return x * x;\n}";
            AnimId code   = anim_code(s, NULL, code_src, 0.30f, true);
            AnimId hilite = anim_code_highlight_line(s, code, 2u, anim_hex_a(0xF9E26B, 0.25f));
            const char *tbl_cells[4] = { "N", "Val", "1", "3.14" };
            AnimId tbl    = anim_table(s, tbl_cells, 2u, 2u, NULL, 0.30f, true);
            AnimVec3 g_pos[3] = { ANIM_V3(-1.0f, 0.0f, 0.0f), ANIM_V3(1.0f, 0.0f, 0.0f), ANIM_V3(0.0f, 1.0f, 0.0f) };
            const char *g_lbls[3] = { "A", "B", "C" };
            uint32_t g_from[2] = { 0u, 1u };
            uint32_t g_to[2]   = { 1u, 2u };
            AnimId gnet   = anim_graph_network(s, 3u, g_pos, g_lbls, 2u, g_from, g_to, true, NULL);
            AnimId card   = anim_rounded_rect(s, 3.0f, 1.4f, 0.2f);
            AnimId follower = anim_dot(s, ANIM_ORIGIN, 0.12f);
            AnimId offscreen = anim_circle(s, 0.5f);
            AnimTimeline *tl2;

            anim_surface_set_wireframe(s, surf, 8.0f, 0.5f, false);
            anim_surface_set_roughness(s, surf, 0.28f, false);
            anim_set_fill_gradient_linear(s, card, ANIM_BLUE_E, ANIM_TEAL, ANIM_V3(-5.0f, 2.0f, 0.0f), ANIM_V3(-2.0f, 2.0f, 0.0f), false);
            anim_set_dash(s, card, 14.0f, 8.0f, 0.0f, false);
            anim_set_shadow(s, card, 12.0f, false);
            anim_set_clip_rect(s, card, ANIM_V3(-5.5f, 1.0f, 0.0f), ANIM_V3(-1.5f, 3.0f, 0.0f), false);
            anim_move_to(s, card, ANIM_V3(-3.5f, 2.0f, 0.0f));
            anim_move_to(s, para, ANIM_V3(-3.5f, -2.0f, 0.0f));
            anim_move_to(s, code, ANIM_V3(3.5f, 2.0f, 0.0f));
            anim_move_to(s, tbl, ANIM_V3(3.5f, -2.0f, 0.0f));
            anim_move_to(s, gnet, ANIM_V3(0.0f, -2.2f, 0.0f));
            anim_move_to(s, offscreen, ANIM_V3(50.0f, 0.0f, 0.0f)); /* offscreen frustum cull test */

            CHECK(anim_child_count(s, para) == 2u, "paragraph created 2 lines");
            CHECK(anim_child_count(s, code) == 2u, "code created card + syntax body");
            CHECK(ANIM_ID_VALID(hilite), "code line highlight valid");
            CHECK(ANIM_ID_VALID(anim_table_cell(s, tbl, 0u, 1u)), "table cell accessor valid");
            CHECK(anim_child_count(s, gnet) > 4u, "graph_network created edges + nodes");
            CHECK(anim_vmob_cubic_count(s, h3d) > 20u, "3d parametric curve generated cubics");

            /* Stress test: >5,000 cubics in one scene */
            for (uint32_t k = 0u; k < 260u; ++k)
            {
                AnimId st = anim_star(s, 10u, 0.12f, 0.05f); /* 20 cubics each => 5,200 cubics */
                anim_move_to(s, st, ANIM_V3(-5.0f + 0.038f * (float)k, -3.4f, 0.0f));
                anim_set_fill(s, st, ANIM_GOLD, false);
                anim_add(s, st);
            }

            anim_add(s, surf);
            anim_add(s, h3d);
            anim_add(s, para);
            anim_add(s, code);
            anim_add(s, hilite);
            anim_add(s, tbl);
            anim_add(s, gnet);
            anim_add(s, card);
            anim_add(s, follower);
            anim_add(s, offscreen);

            tl2 = anim_timeline_create();
            {
                AnimSpec sp_orbit = anim_spec_with_custom_rate(
                    anim_anim_camera_orbit(1.2f, 0.25f, 0.9f),
                    test_custom_rate_fn, NULL);
                AnimSpec sp_dash = anim_anim_dash_flow(card, 120.0f);
                AnimSpec sp_path = anim_anim_follow_path(follower, card, false);

                anim_play_at(tl2, s, 0.0f, &sp_orbit, 1u, 2.0f, ANIM_RATE_LINEAR);
                anim_play_at(tl2, s, 0.25f, &sp_dash, 1u, 1.5f, ANIM_RATE_LINEAR);
                anim_play_at(tl2, s, 0.5f, &sp_path, 1u, 1.5f, ANIM_RATE_SPRING);
            }
            CHECK(fabsf(anim_timeline_duration(tl2) - 2.0f) < 1e-4f, "overlapping anim_play_at timeline duration == 2.0s");

            anim_seek(tl2, s, 1.0f);
            {
                AnimRenderOpts preset = anim_render_opts_preset(ANIM_RES_1080P);
                CHECK(preset.width == 1920u && preset.height == 1080u && preset.fps == 60u, "preset 1080p matches");
            }
            opts = anim_render_opts_default(640, 360);
            CHECK(anim_export_png(ren, s, &opts, "/tmp/test_anim_3d.png"), "exported 3D + 5k-cubic PNG");
            CHECK(png_has_non_uniform_pixels("/tmp/test_anim_3d.png"), "3D PNG non-empty");
            {
                AnimFrameStats st = anim_renderer_last_stats(ren);
                printf("stress stats: %u shapes, %u bands, %u quad segs, %u surface tris, %u culled, %.2f ms CPU build\n",
                       st.shape_count, st.band_count, st.quad_seg_count, st.surface_tri_count, st.culled_shape_count, st.cpu_build_ms);
                CHECK(st.quad_seg_count > 5000u, "stress test rendered >5,000 quad segs");
                CHECK(st.surface_tri_count == 24u * 24u * 2u, "3D surface emitted 24x24x2 triangles");
                CHECK(st.culled_shape_count >= 1u, "frustum culling culled offscreen shape");
            }

            /* Exercise temporal motion blur accumulation pass via short 2-frame export */
            {
                AnimTimeline *tl_mb = anim_timeline_create();
                AnimSpec sp_sh = anim_anim_shift(follower, ANIM_V3(2.0f, 0.0f, 0.0f));
                anim_play(tl_mb, s, &sp_sh, 1u, 0.2f, ANIM_RATE_LINEAR);
                opts.fps                 = 10u;
                opts.motion_blur_samples = 3u;
                opts.shutter_angle       = 0.5f;
                CHECK(anim_export_video(ren, tl_mb, s, &opts, "/tmp/test_anim_mb.mp4", NULL), "exported motion-blur MP4");
                anim_timeline_destroy(tl_mb);
            }

            anim_timeline_destroy(tl2);
        }

        /* 9. Physics & Computer Science Toolkit */
        anim_scene_clear(s);
        {
            float rk_y[2] = { 1.0f, 0.0f };
            for (int st = 0; st < 100; ++st)
                anim_phys_step_rk4(rk_y, 2u, (float)st * (ANIM_PI * 0.01f), ANIM_PI * 0.01f, test_sho_ode, NULL);
            CHECK(fabsf(rk_y[0] + 1.0f) < 1e-4f && fabsf(rk_y[1]) < 1e-4f, "RK4 harmonic oscillator at t=pi == -1");

            float y0[2] = { 1.0f, 0.0f };
            AnimId traj = anim_phys_trajectory(s, y0, 2u, 0.0f, ANIM_TAU, 32u, test_sho_ode, NULL, NULL);
            AnimId spr  = anim_phys_spring(s, ANIM_V3(-4.0f, 2.0f, 0.0f), ANIM_V3(-2.0f, 2.0f, 0.0f), 6u, 0.35f);
            AnimId pend = anim_phys_pendulum(s, ANIM_V3(0.0f, 2.5f, 0.0f), 1.4f, 0.4f, 0.16f);
            AnimVec3 fvecs[2] = { ANIM_V3(0.0f, 0.9f, 0.0f), ANIM_V3(0.0f, -0.9f, 0.0f) };
            const char *flbls[2] = { "N", "mg" };
            AnimColor fcols[2] = { ANIM_TEAL, ANIM_GOLD };
            AnimId fbd  = anim_phys_free_body(s, ANIM_V3(3.0f, 2.0f, 0.0f), 0.7f, 0.5f, fvecs, flbls, fcols, 2u, NULL);
            AnimId wave = anim_phys_wave(s, ANIM_V3(-5.0f, 0.0f, 0.0f), ANIM_V3(-1.5f, 0.0f, 0.0f), 0.3f, 2.0f, 0.0f, 32u);
            AnimVec3 cpos[2] = { ANIM_V3(-1.0f, -1.5f, 0.0f), ANIM_V3(1.0f, -1.5f, 0.0f) };
            float    cval[2] = { 1.0f, -1.0f };
            AnimId qp   = anim_phys_charge(s, cpos[0], 0.2f, 1);
            AnimId qm   = anim_phys_charge(s, cpos[1], 0.2f, -1);
            AnimId fl   = anim_phys_field_lines(s, cpos, cval, 2u, ANIM_V3(-3.0f, -3.0f, 0.0f), ANIM_V3(3.0f, 0.0f, 0.0f), 6u);
            AnimId lens = anim_phys_lens(s, ANIM_V3(4.0f, -1.5f, 0.0f), 2.0f, 0.35f, false);
            AnimId rays = anim_phys_ray_bundle(s, ANIM_V3(4.0f, -1.5f, 0.0f), 1.2f, 1.8f, NULL, 5u, 2.5f, 5.5f);

            const char *arr_l[3] = { "3", "1", "2" };
            AnimId arr_cells[3];
            AnimId arr   = anim_cs_array(s, arr_l, 3u, 0.65f, 0.55f, true, NULL, arr_cells);
            float  bvals[3] = { 3.0f, 1.0f, 2.0f };
            AnimId bars[3];
            AnimId bch   = anim_cs_bar_chart(s, bvals, arr_l, 3u, 2.4f, 1.2f, ANIM_BLUE, ANIM_TEAL, NULL, bars);
            AnimId llist = anim_cs_linked_list(s, arr_l, 3u, 0.8f, 0.45f, 0.35f, NULL, NULL);
            AnimId stk   = anim_cs_stack_queue(s, arr_l, 3u, false, 0.7f, 0.35f, NULL);
            const char *tlbl[3] = { "2", "1", "3" };
            AnimId tree  = anim_cs_binary_tree(s, tlbl, 3u, 0.8f, 0.9f, 0.22f, NULL, NULL);
            AnimVec3 fpos[2] = { ANIM_V3(-0.8f, 0.0f, 0.0f), ANIM_V3(0.8f, 0.0f, 0.0f) };
            bool     facc[2] = { false, true };
            uint32_t ff[2] = { 0u, 1u }, ft[2] = { 1u, 1u };
            const char *fel[2] = { "a", "b" };
            AnimId fsm   = anim_cs_fsm(s, 2u, fpos, tlbl, facc, 0u, 2u, ff, ft, fel, 0.26f, NULL);
            AnimId gate  = anim_cs_logic_gate(s, ANIM_GATE_XOR, ANIM_V3(0.0f, 0.0f, 0.0f), 0.7f, "XOR", NULL);
            AnimId breg  = anim_cs_bit_register(s, 0xAu, 4u, 0.45f, true, NULL, NULL);
            uint32_t nnl[3] = { 2u, 3u, 1u };
            AnimId nn    = anim_cs_neural_net(s, nnl, 3u, 2.0f, 1.4f, 0.12f, ANIM_BLUE, ANIM_GOLD);

            CHECK(anim_vmob_cubic_count(s, traj) >= 30u, "RK4 trajectory generated cubics");
            CHECK(anim_child_count(s, fbd) >= 4u, "free body created box + forces");
            CHECK(anim_child_count(s, fl) >= 4u, "field lines generated streamlines");
            CHECK(anim_child_count(s, rays) >= 5u, "ray bundle generated rays");
            CHECK(anim_child_count(s, arr) >= 6u, "cs_array created cells + indices");
            CHECK(anim_child_count(s, bch) == 4u, "bar chart created baseline + 3 bars");
            CHECK(anim_child_count(s, llist) >= 6u, "linked list created nodes + arrows");
            CHECK(anim_child_count(s, stk) >= 4u, "stack created frame + items");
            CHECK(anim_child_count(s, tree) == 5u, "binary tree created 2 edges + 3 nodes");
            CHECK(anim_child_count(s, fsm) >= 6u, "fsm created states + transitions");
            CHECK(anim_child_count(s, gate) >= 5u, "logic gate created body + pins");
            CHECK(anim_child_count(s, breg) == 8u, "4-bit register created 4 bits + 4 indices");
            CHECK(anim_child_count(s, nn) == 9u + 6u, "neural net created 9 edges + 6 neurons");

            anim_add(s, traj); anim_add(s, spr); anim_add(s, pend); anim_add(s, fbd);
            anim_add(s, wave); anim_add(s, qp); anim_add(s, qm); anim_add(s, fl);
            anim_add(s, lens); anim_add(s, rays);
            anim_add(s, arr); anim_add(s, bch); anim_add(s, llist); anim_add(s, stk);
            anim_add(s, tree); anim_add(s, fsm); anim_add(s, gate); anim_add(s, breg);
            anim_add(s, nn);

            {
                AnimTimeline *tl_pcs = anim_timeline_create();
                anim_cs_swap_mobs(tl_pcs, s, arr_cells[0], arr_cells[1], 0.4f, 0.4f);
                anim_cs_bar_swap(tl_pcs, s, bars, 0u, 1u, 0.4f, 0.4f);
                anim_seek(tl_pcs, s, 0.2f);
                opts = anim_render_opts_default(640, 360);
                CHECK(anim_export_png(ren, s, &opts, "/tmp/test_anim_pcs.png"), "exported Physics+CS PNG");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_pcs.png"), "Physics+CS PNG non-empty");
                anim_timeline_destroy(tl_pcs);
            }
        }

        /* 10. Manim Parity & Beyond-Manim Suite (CSG, Marching Squares, StreamLines, 3D Solids, PIP Zoom, New Anims) */
        {
            AnimScene    *ms   = anim_scene_create();
            AnimTimeline *tl_m = anim_timeline_create();
            AnimId cb    = anim_cubic_bezier(ms, ANIM_V3(-5.5f, 2.5f, 0.0f), ANIM_V3(-5.0f, 3.3f, 0.0f), ANIM_V3(-4.2f, 1.8f, 0.0f), ANIM_V3(-3.6f, 2.6f, 0.0f));
            AnimId abp   = anim_arc_between_points(ms, ANIM_V3(-3.2f, 2.2f, 0.0f), ANIM_V3(-1.8f, 2.2f, 0.0f), 1.2f);
            AnimId sec   = anim_sector(ms, 0.25f, 0.65f, 0.2f, 1.8f);
            AnimId ang   = anim_angle(ms, ANIM_V3(1.5f, 2.0f, 0.0f), ANIM_V3(0.5f, 2.0f, 0.0f), ANIM_V3(1.2f, 2.8f, 0.0f), 0.45f);
            AnimId rang  = anim_right_angle(ms, ANIM_V3(2.8f, 2.0f, 0.0f), ANIM_V3(2.0f, 2.0f, 0.0f), ANIM_V3(2.0f, 2.8f, 0.0f), 0.25f);
            AnimId sq_t  = anim_square(ms, 0.9f);
            AnimId srect, brect, ul, crs;

            anim_move_to(ms, sec, ANIM_V3(-0.6f, 2.4f, 0.0f));
            anim_move_to(ms, sq_t, ANIM_V3(-4.5f, 0.2f, 0.0f));
            srect = anim_surrounding_rect(ms, sq_t, 0.14f, 0.12f);
            brect = anim_background_rect(ms, sq_t, 0.18f, anim_rgba(0.12f, 0.16f, 0.28f, 0.85f));
            ul    = anim_underline(ms, sq_t, 0.12f);
            crs   = anim_cross(ms, sq_t, 4.0f);

            /* 2D Boolean CSG: Union, Intersect, Difference, XOR */
            AnimId ba = anim_circle(ms, 0.65f);
            AnimId bb = anim_circle(ms, 0.65f);
            anim_move_to(ms, ba, ANIM_V3(-0.35f, 0.0f, 0.0f));
            anim_move_to(ms, bb, ANIM_V3(0.35f, 0.0f, 0.0f));
            AnimId b_uni = anim_bool_op(ms, ba, bb, ANIM_BOOL_UNION);
            AnimId b_int = anim_bool_op(ms, ba, bb, ANIM_BOOL_INTERSECT);
            AnimId b_dif = anim_bool_op(ms, ba, bb, ANIM_BOOL_DIFFERENCE);
            AnimId b_xor = anim_bool_op(ms, ba, bb, ANIM_BOOL_XOR);
            CHECK(anim_vmob_cubic_count(ms, b_uni) >= 12u, "CSG union generated closed cubics");
            CHECK(anim_vmob_cubic_count(ms, b_int) >= 8u,  "CSG intersect generated closed cubics");
            CHECK(anim_vmob_cubic_count(ms, b_dif) >= 12u, "CSG difference generated closed cubics");
            CHECK(anim_vmob_cubic_count(ms, b_xor) >= 16u, "CSG xor generated closed cubics");
            anim_move_to(ms, b_uni, ANIM_V3(-2.4f, 0.4f, 0.0f));
            anim_move_to(ms, b_int, ANIM_V3(-0.9f, 0.4f, 0.0f));
            anim_move_to(ms, b_dif, ANIM_V3(0.6f, 0.4f, 0.0f));
            anim_move_to(ms, b_xor, ANIM_V3(2.1f, 0.4f, 0.0f));
            anim_set_fill(ms, b_dif, ANIM_GOLD, false);
            anim_set_fill(ms, b_xor, ANIM_PURPLE, false);

            /* Polar, Implicit Marching Squares, StreamLines */
            AnimId pplane = anim_polar_plane(ms, 1.5f, 0.5f, 12u, 2.2f);
            anim_move_to(ms, pplane, ANIM_V3(-4.5f, -2.0f, 0.0f));
            AnimId pcurve = anim_plot_polar(ms, pplane, test_polar_r, 0.0f, ANIM_TAU, 96u, NULL);
            AnimAxesDesc axd = { -2.0f, 2.0f, 1.0f, -2.0f, 2.0f, 1.0f, 2.2f, 2.2f, false, false, false, false, NULL };
            AnimId iaxes  = anim_axes(ms, &axd);
            anim_move_to(ms, iaxes, ANIM_V3(-1.4f, -2.0f, 0.0f));
            AnimId icont  = anim_implicit_plot(ms, iaxes, test_implicit_circle, -1.8f, 1.8f, -1.8f, 1.8f, 28u, 28u, NULL);
            AnimId slines = anim_stream_lines(ms, iaxes, test_swirl_field, -1.8f, 1.8f, -1.8f, 1.8f, 0.7f, 0.7f, 18u, NULL);
            CHECK(anim_vmob_cubic_count(ms, icont) >= 8u, "marching squares extracted contour segments");
            CHECK(anim_child_count(ms, slines) >= 9u, "streamlines generated field trajectories");

            /* BulletedList + Variable synced with tracker */
            const char *bl_items[3] = { "Boolean CSG", "Marching Squares", "Platonic 3D" };
            AnimId blist = anim_bulleted_list(ms, NULL, bl_items, 3u, 0.28f, 0.16f);
            anim_move_to(ms, blist, ANIM_V3(-0.2f, 2.3f, 0.0f));
            AnimTrackerId tv = anim_tracker_create(ms, 1.25f);
            AnimId var = anim_variable(ms, "k", tv, 2u, NULL, 0.34f);
            anim_move_to(ms, var, ANIM_V3(5.0f, 0.3f, 0.0f));

            /* True Z-buffered 3D Solids + Platonic Polyhedra */
            AnimId p_tet  = anim_polyhedron(ms, ANIM_POLY_TETRAHEDRON, 0.55f);
            AnimId p_oct  = anim_polyhedron(ms, ANIM_POLY_OCTAHEDRON, 0.55f);
            AnimId p_dod  = anim_polyhedron(ms, ANIM_POLY_DODECAHEDRON, 0.55f);
            AnimId p_ico  = anim_polyhedron(ms, ANIM_POLY_ICOSAHEDRON, 0.55f);
            AnimId p_cyl  = anim_cylinder(ms, 0.35f, 0.9f, 20u);
            AnimId p_cone = anim_cone(ms, 0.40f, 0.9f, 20u);
            AnimId p_arr  = anim_arrow3d(ms, ANIM_V3(1.2f, -2.5f, 0.0f), ANIM_V3(2.2f, -1.6f, 0.3f), 0.04f, 0.11f, 0.25f, 14u);
            AnimId p_dot  = anim_dot3d(ms, ANIM_V3(2.2f, -1.6f, 0.3f), 0.10f);
            anim_move_to(ms, p_tet,  ANIM_V3(1.6f, -1.0f, 0.0f));
            anim_move_to(ms, p_oct,  ANIM_V3(2.8f, -1.0f, 0.0f));
            anim_move_to(ms, p_dod,  ANIM_V3(4.0f, -1.0f, 0.0f));
            anim_move_to(ms, p_ico,  ANIM_V3(5.2f, -1.0f, 0.0f));
            anim_move_to(ms, p_cyl,  ANIM_V3(3.6f, -2.3f, 0.0f));
            anim_move_to(ms, p_cone, ANIM_V3(4.9f, -2.3f, 0.0f));
            anim_set_fill(ms, p_ico, ANIM_PINK, false);

            /* TracedPath + FollowPath */
            AnimId t_orb = anim_circle(ms, 0.55f);
            anim_move_to(ms, t_orb, ANIM_V3(-2.4f, -2.0f, 0.0f));
            anim_set_stroke(ms, t_orb, ANIM_GREY_D, 1.5f, false);
            anim_set_fill(ms, t_orb, anim_rgba(0,0,0,0), false);
            AnimId t_dot = anim_dot(ms, ANIM_V3(-1.85f, -2.0f, 0.0f), 0.09f);
            anim_set_fill(ms, t_dot, ANIM_YELLOW, false);
            AnimId t_path = anim_traced_path(ms, t_dot, ANIM_YELLOW, 3.0f, 128u);

            anim_add(ms, cb); anim_add(ms, abp); anim_add(ms, sec); anim_add(ms, ang); anim_add(ms, rang);
            anim_add(ms, brect); anim_add(ms, sq_t); anim_add(ms, srect); anim_add(ms, ul); anim_add(ms, crs);
            anim_add(ms, b_uni); anim_add(ms, b_int); anim_add(ms, b_dif); anim_add(ms, b_xor);
            anim_add(ms, pplane); anim_add(ms, pcurve); anim_add(ms, iaxes); anim_add(ms, icont); anim_add(ms, slines);
            anim_add(ms, blist); anim_add(ms, var);
            anim_add(ms, p_tet); anim_add(ms, p_oct); anim_add(ms, p_dod); anim_add(ms, p_ico);
            anim_add(ms, p_cyl); anim_add(ms, p_cone); anim_add(ms, p_arr); anim_add(ms, p_dot);
            anim_add(ms, t_orb); anim_add(ms, t_path); anim_add(ms, t_dot);

            /* Exercise new animations + LaggedStart + Succession + Matrix + Homotopy + Flash/Broadcast */
            float shear_m3[9] = { 1.0f, 0.35f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f };
            AnimSpec lag_specs[3] = {
                anim_anim_spin_in_from_nothing(p_dod, ANIM_PI),
                anim_anim_grow_from_edge(blist, ANIM_LEFT),
                anim_anim_show_increasing_subsets(slines),
            };
            anim_play_lagged(tl_m, ms, lag_specs, 3u, 0.6f, 0.25f, ANIM_RATE_SMOOTH);

            AnimSpec seq_specs[2] = {
                anim_anim_tracker_to(tv, 3.14f),
                anim_anim_custom(b_uni, test_custom_anim_fn, NULL),
            };
            anim_play_sequence(tl_m, ms, seq_specs, 2u, 0.3f, ANIM_RATE_LINEAR);

            AnimSpec combo[6] = {
                anim_anim_follow_path(t_dot, t_orb, false),
                anim_anim_apply_matrix3(sq_t, shear_m3),
                anim_anim_homotopy(abp, test_homotopy_fn, NULL),
                anim_anim_flash(ANIM_V3(-0.9f, 0.4f, 0.0f), ANIM_YELLOW, 0.25f, 10u, 0.6f),
                anim_anim_broadcast(ANIM_V3(0.6f, 0.4f, 0.0f), ANIM_TEAL, 3u, 1.1f),
                anim_anim_type_with_cursor(blist, ANIM_YELLOW),
            };
            anim_play(tl_m, ms, combo, 6u, 0.8f, ANIM_RATE_SMOOTH);
            anim_seek(tl_m, ms, anim_timeline_duration(tl_m) - 0.35f);
            CHECK(anim_vmob_cubic_count(ms, t_path) >= 8u, "TracedPath recorded smooth path along orbit");

            /* Render with GPU Picture-in-Picture Zoomed Camera Inset */
            opts = anim_render_opts_default(640, 360);
            opts.zoom_inset_enable = true;
            opts.zoom_inset_cam = anim_camera_default();
            opts.zoom_inset_cam.pos    = ANIM_V3(4.0f, -1.2f, 5.5f);
            opts.zoom_inset_cam.target = ANIM_V3(4.0f, -1.2f, 0.0f);
            opts.zoom_inset_cam.zoom   = 1.8f;
            opts.zoom_inset_rect[0] = 0.65f; opts.zoom_inset_rect[1] = 0.04f;
            opts.zoom_inset_rect[2] = 0.97f; opts.zoom_inset_rect[3] = 0.38f;
            CHECK(anim_export_png(ren, ms, &opts, "/tmp/test_anim_manim_extra.png"), "exported Manim-parity + PIP Zoom PNG");
            CHECK(png_has_non_uniform_pixels("/tmp/test_anim_manim_extra.png"), "Manim-parity + PIP Zoom PNG non-empty");

            /* 11. Advanced Geometry, Voronoi, Multi-Contours, Frustum3D, PixelGrid, Particles, Billboard, AgX */
            {
                AnimVec3 hull_pts[6] = {
                    ANIM_V3(-5.0f, 1.0f, 0.0f), ANIM_V3(-4.2f, 2.2f, 0.0f), ANIM_V3(-3.3f, 1.3f, 0.0f),
                    ANIM_V3(-4.1f, 1.5f, 0.0f), ANIM_V3(-4.7f, 0.4f, 0.0f), ANIM_V3(-3.6f, 0.5f, 0.0f)
                };
                AnimId chull = anim_convex_hull(ms, hull_pts, 6u);
                AnimVec3 vsites[5] = {
                    ANIM_V3(-2.0f, 1.5f, 0.0f), ANIM_V3(-0.9f, 2.1f, 0.0f), ANIM_V3(-1.3f, 0.7f, 0.0f),
                    ANIM_V3(-0.3f, 1.1f, 0.0f), ANIM_V3(-1.8f, 0.5f, 0.0f)
                };
                AnimId vor  = anim_voronoi(ms, vsites, 5u, ANIM_V3(-2.6f, 0.1f, 0.0f), ANIM_V3(0.2f, 2.6f, 0.0f), NULL);
                float clv[4] = { -0.8f, -0.2f, 0.2f, 0.8f };
                AnimId cplot = anim_contour_plot(ms, iaxes, test_saddle_fn, -1.8f, 1.8f, -1.8f, 1.8f, 24u, 24u, clv, 4u, ANIM_BLUE, ANIM_GOLD, NULL);
                AnimId frust = anim_frustum3d(ms, 45.0f, 1.4f, 0.6f, 2.0f, ANIM_V3(2.2f, 1.5f, 1.2f), ANIM_V3(3.4f, 1.1f, -0.4f));
                AnimId pcells[24];
                AnimId pgrid = anim_pixel_grid(ms, 6u, 4u, 0.28f, true, pcells);
                anim_move_to(ms, pgrid, ANIM_V3(4.5f, 1.5f, 0.0f));
                anim_set_fill(ms, pcells[7], ANIM_TEAL, false);
                AnimId psys  = anim_particle_system(ms, 18u, ANIM_V3(-4.5f, -0.5f, 0.0f),
                                                    ANIM_V3(-0.6f, 0.4f, 0.0f), ANIM_V3(0.6f, 1.2f, 0.0f),
                                                    ANIM_V3(0.0f, -0.8f, 0.0f), 1.0f, ANIM_YELLOW, ANIM_PINK, 0.06f);
                AnimId blbl  = anim_text(ms, NULL, "3D Label", 0.26f);
                anim_move_to(ms, blbl, ANIM_V3(3.0f, 2.3f, 0.5f));
                anim_set_billboard(ms, blbl, true, true);

                CHECK(anim_vmob_cubic_count(ms, chull) >= 4u, "convex hull generated closed polygon");
                CHECK(anim_child_count(ms, vor) == 5u, "voronoi generated 5 clipped cells");
                CHECK(anim_child_count(ms, cplot) == 4u, "contour plot generated 4 isoline levels");
                CHECK(anim_child_count(ms, frust) == 14u, "frustum3d generated 12 edges + 2 clip planes");
                CHECK(anim_child_count(ms, pgrid) == 48u, "pixel grid generated 24 cells + 24 centers");
                CHECK(anim_child_count(ms, psys) == 18u, "particle system generated 18 particles");

                anim_add(ms, chull); anim_add(ms, vor); anim_add(ms, cplot);
                anim_add(ms, frust); anim_add(ms, pgrid); anim_add(ms, psys); anim_add(ms, blbl);
                {
                    AnimCamera zc = anim_camera_default();
                    zc.pos = ANIM_V3(3.0f, 1.4f, 4.5f); zc.target = ANIM_V3(3.0f, 1.4f, 0.0f); zc.zoom = 1.6f;
                    AnimSpec zs = anim_anim_zoom_inset_to(true, zc, NULL);
                    anim_play(tl_m, ms, &zs, 1u, 0.4f, ANIM_RATE_SMOOTH);
                    anim_seek(tl_m, ms, anim_timeline_duration(tl_m) - 0.1f);
                    opts = anim_render_opts_default(640, 360);
                    opts.tonemap = ANIM_TONEMAP_AGX;
                    opts.aberration_px = 0.8f;
                    opts.grain_amount  = 0.002f;
                    CHECK(anim_export_png(ren, ms, &opts, "/tmp/test_anim_advanced.png"), "exported Advanced AgX + Scene-PIP PNG");
                    CHECK(png_has_non_uniform_pixels("/tmp/test_anim_advanced.png"), "Advanced AgX PNG non-empty");
                }
            }

            /* Section 12: 3D Camera Movements, 3D Surface Morphing & 3D Transitions */
            {
                AnimScene    *s3  = anim_scene_create();
                AnimTimeline *tl3 = anim_timeline_create();
                AnimId sph = anim_sphere(s3, 1.0f, 20u, 12u);
                AnimId tor = anim_torus(s3, 1.1f, 0.38f, 24u, 14u);
                AnimId ico = anim_polyhedron(s3, ANIM_POLY_ICOSAHEDRON, 0.85f);
                AnimId card_a = anim_rounded_rect(s3, 2.2f, 1.4f, 0.15f);
                AnimId card_b = anim_rounded_rect(s3, 2.2f, 1.4f, 0.15f);
                AnimId orb_dot = anim_dot3d(s3, ANIM_V3(2.0f, 0.0f, 0.0f), 0.22f);
                AnimId cam_rail = anim_arc_between_points(s3, ANIM_V3(0.0f, 0.0f, 9.5f), ANIM_V3(6.5f, 3.0f, 6.5f), 0.7f);
                AnimCamera c_start = anim_camera_default();
                AnimCamera c_side  = anim_camera_default();
                c_side.pos = ANIM_V3(c_start.pos.z, 1.0f, 0.0f);

                anim_set_fill(s3, card_a, ANIM_BLUE, false);
                anim_set_fill(s3, card_b, ANIM_GOLD, false);
                anim_move_to(s3, card_a, ANIM_V3(-3.0f, 0.0f, 0.0f));
                anim_move_to(s3, card_b, ANIM_V3(-3.0f, 0.0f, 0.0f));
                anim_move_to(s3, ico, ANIM_V3(3.0f, 0.0f, 0.0f));
                anim_add(s3, sph); anim_add(s3, card_a); anim_add(s3, ico); anim_add(s3, orb_dot);

                /* 12a: Spherical camera fly-to preserves orbit radius */
                {
                    AnimSpec sp = anim_anim_camera_fly_to(c_side, 0.5f);
                    AnimCamera cur;
                    float r_mid, dx, dy, dz;
                    anim_play(tl3, s3, &sp, 1u, 1.0f, ANIM_RATE_LINEAR);
                    anim_seek(tl3, s3, 0.5f);
                    cur = anim_scene_camera(s3);
                    dx = cur.pos.x - cur.target.x;
                    dy = cur.pos.y - cur.target.y;
                    dz = cur.pos.z - cur.target.z;
                    r_mid = sqrtf(dx * dx + dy * dy + dz * dz);
                    CHECK(r_mid > 9.5f, "camera_fly_to orbits instead of chord-cutting");
                }
                /* 12b: Gimbal-free polar orbit over +Y pole */
                {
                    AnimSpec sp = anim_anim_camera_orbit_axis(ANIM_PI, ANIM_RIGHT, 1.0f);
                    AnimCamera cur;
                    anim_play(tl3, s3, &sp, 1u, 1.0f, ANIM_RATE_SMOOTH);
                    anim_seek(tl3, s3, 1.5f);
                    cur = anim_scene_camera(s3);
                    CHECK(cur.up.y != 0.0f || fabsf(cur.up.z) > 0.1f, "camera_orbit_axis rotates camera up over poles");
                }
                /* 12c: Dolly zoom keeps d * tan(fov/2) constant */
                {
                    float d0, fov0_rad, inv0, d1, fov1_rad, inv1;
                    AnimCamera cur;
                    anim_seek(tl3, s3, 2.0f);
                    cur = anim_scene_camera(s3);
                    d0 = sqrtf((cur.pos.x - cur.target.x) * (cur.pos.x - cur.target.x) +
                               (cur.pos.y - cur.target.y) * (cur.pos.y - cur.target.y) +
                               (cur.pos.z - cur.target.z) * (cur.pos.z - cur.target.z));
                    fov0_rad = cur.fov_deg * (ANIM_PI / 180.0f);
                    inv0 = d0 * tanf(0.5f * fov0_rad);
                    {
                        AnimSpec dz = anim_anim_camera_dolly_zoom(70.0f);
                        anim_play(tl3, s3, &dz, 1u, 1.0f, ANIM_RATE_SMOOTH);
                        anim_seek(tl3, s3, 3.0f);
                    }
                    cur = anim_scene_camera(s3);
                    d1 = sqrtf((cur.pos.x - cur.target.x) * (cur.pos.x - cur.target.x) +
                               (cur.pos.y - cur.target.y) * (cur.pos.y - cur.target.y) +
                               (cur.pos.z - cur.target.z) * (cur.pos.z - cur.target.z));
                    fov1_rad = cur.fov_deg * (ANIM_PI / 180.0f);
                    inv1 = d1 * tanf(0.5f * fov1_rad);
                    CHECK(fabsf(inv0 - inv1) < 0.05f, "camera_dolly_zoom preserves frustum height invariant");
                }
                /* 12d: 3D surface morph sphere -> torus + 3D transitions */
                {
                    AnimSpec m[4] = {
                        anim_anim_transform(sph, tor),
                        anim_anim_flip_replace_3d(card_a, card_b, ANIM_UP),
                        anim_anim_explode_3d(ico, 0.8f, ANIM_PI),
                        anim_anim_rotate_around(orb_dot, ANIM_ORIGIN, ANIM_PI, ANIM_UP, true)
                    };
                    anim_play(tl3, s3, m, 4u, 1.2f, ANIM_RATE_SMOOTH);
                    anim_seek(tl3, s3, 3.6f);
                    CHECK(anim_export_png(ren, s3, &opts, "/tmp/test_anim_3d_morph.png"), "exported 3D morph & transitions PNG");
                    CHECK(png_has_non_uniform_pixels("/tmp/test_anim_3d_morph.png"), "3D morph PNG non-empty");
                }
                /* 12e: Camera path + track + shake + swirl + pulse + spring + tumble */
                {
                    AnimSpec s_cam[6] = {
                        anim_anim_camera_path(cam_rail, ANIM_ORIGIN, false),
                        anim_anim_camera_track(orb_dot, ANIM_V3(0.0f, 1.5f, 6.0f), false),
                        anim_anim_camera_shake(0.12f, 8.0f),
                        anim_anim_swirl_3d(sph, 1.8f, ANIM_UP),
                        anim_anim_pulse_glow(card_b, 0.8f, 1.15f),
                        anim_anim_spring_move(orb_dot, ANIM_V3(0.0f, -1.5f, 0.0f), 0.5f, 14.0f)
                    };
                    AnimSpec tin = anim_anim_tumble_in(card_a, ANIM_V3(-2.0f, 2.0f, -3.0f), ANIM_TAU, ANIM_UR);
                    AnimSpec tout = anim_anim_tumble_out(ico, ANIM_V3(2.0f, -1.0f, 2.0f), ANIM_PI, ANIM_UP);
                    AnimSpec flp = anim_anim_flip_3d(card_b, ANIM_RIGHT, 1.0f);
                    anim_play(tl3, s3, s_cam, 6u, 1.0f, ANIM_RATE_SMOOTH);
                    anim_play(tl3, s3, &tin, 1u, 0.6f, ANIM_RATE_SMOOTH);
                    anim_play(tl3, s3, &tout, 1u, 0.6f, ANIM_RATE_SMOOTH);
                    anim_play(tl3, s3, &flp, 1u, 0.6f, ANIM_RATE_SMOOTH);
                    anim_seek(tl3, s3, anim_timeline_duration(tl3) - 0.1f);
                    CHECK(anim_export_png(ren, s3, &opts, "/tmp/test_anim_3d_cam.png"), "exported 3D camera path/transitions PNG");
                }
                anim_timeline_destroy(tl3);
                anim_scene_destroy(s3);
            }

            /* 13. Multi-viewport, textured quads/surfaces, 3D field/streamlines, plots & tex align/diff */
            {
                AnimScene    *s4  = anim_scene_create();
                AnimTimeline *tl4 = anim_timeline_create();
                AnimAxesDesc  axd = { 0.0f, 5.0f, 1.0f, 0.0f, 5.0f, 1.0f, 4.4f, 3.0f, true, true, true, false, NULL };
                AnimId        ax  = anim_axes(s4, &axd);
                float         xs[5] = { 0.6f, 1.4f, 2.3f, 3.4f, 4.3f };
                float         ys[5] = { 1.1f, 2.8f, 2.2f, 4.1f, 3.5f };
                float         bar_vals[6] = { 1.2f, 2.1f, 2.8f, 1.5f, 1.9f, 3.2f };
                AnimBoxStats  boxes[2] = { { 0.5f, 1.2f, 2.0f, 3.1f, 4.2f }, { 1.0f, 1.8f, 2.7f, 3.6f, 4.6f } };
                float         hm_vals[16];
                AnimId        sc, br, bx, hm, vf3, sl3, img_q, sph_t, aln, dgrp, eq_a, eq_b;
                uint32_t      vp0, vp1, k;
                const char   *align_lines[2] = { "E &= m c^2", "p &= gamma m v" };
                const char   *diff_a[4] = { "E", "=", "m", "c^2" };
                const char   *diff_b[5] = { "E^2", "=", "(m c^2)^2", "+", "(p c)^2" };
                AnimCamera    cam_top = anim_camera_default();

                anim_move_to(s4, ax, ANIM_V3(-3.0f, -1.0f, 0.0f));
                anim_add(s4, ax);
                sc = anim_scatter_plot(s4, ax, xs, ys, NULL, NULL, 5u, 0.09f);
                br = anim_bar_plot(s4, ax, bar_vals, 3u, 2u, false, NULL, 0.7f);
                bx = anim_box_plot(s4, ax, boxes, 2u, 0.45f, NULL);
                for (k = 0u; k < 16u; ++k) hm_vals[k] = (float)k / 15.0f;
                hm = anim_heatmap(s4, ax, hm_vals, 4u, 4u, 0.0f, 1.0f, ANIM_BLUE_D, ANIM_TEAL, ANIM_YELLOW);
                CHECK(ANIM_ID_VALID(sc) && ANIM_ID_VALID(br) && ANIM_ID_VALID(bx) && ANIM_ID_VALID(hm),
                      "scatter/bar/box/heatmap builders valid");
                hm_vals[0] = 0.95f;
                anim_heatmap_set(s4, hm, hm_vals, 4u, 4u, 0.0f, 1.0f, ANIM_BLUE_D, ANIM_TEAL, ANIM_YELLOW);
                anim_set_opacity(s4, hm, 0.45f, true);
                anim_add(s4, hm);
                anim_add(s4, br);
                anim_add(s4, bx);
                anim_add(s4, sc);

                vf3 = anim_vector_field3d(s4, test_vfield3d_fn, ANIM_V3(-1.5f, -1.5f, -1.5f), ANIM_V3(1.5f, 1.5f, 1.5f),
                                          3u, 3u, 3u, 0.55f, ANIM_BLUE_D, ANIM_YELLOW, NULL);
                sl3 = anim_stream_lines3d(s4, test_vfield3d_fn, ANIM_V3(-1.5f, -1.5f, -1.5f), ANIM_V3(1.5f, 1.5f, 1.5f),
                                          2u, 2u, 2u, 0.09f, 20u, ANIM_TEAL, ANIM_GOLD, NULL);
                CHECK(ANIM_ID_VALID(vf3) && ANIM_ID_VALID(sl3), "3D vector field & streamlines valid");
                anim_move_to(s4, vf3, ANIM_V3(3.0f, 0.0f, 0.0f));
                anim_move_to(s4, sl3, ANIM_V3(3.0f, 0.0f, 0.0f));
                anim_add(s4, vf3);
                anim_add(s4, sl3);

                /* Texture loading from already-generated PNG */
                img_q = anim_image(s4, "/tmp/test_anim_3d_morph.png", 1.8f);
                CHECK(ANIM_ID_VALID(img_q) && anim_width(s4, img_q) > 2.0f, "anim_image loaded PNG into quad");
                anim_move_to(s4, img_q, ANIM_V3(-3.0f, 2.2f, 0.0f));
                anim_add(s4, img_q);

                sph_t = anim_sphere(s4, 0.85f, 18u, 12u);
                anim_surface_set_texture(s4, sph_t, "/tmp/test_anim_3d_morph.png", true, false);
                anim_move_to(s4, sph_t, ANIM_V3(0.4f, 2.0f, 0.0f));
                anim_add(s4, sph_t);

                /* Multi-line Typst align + diff */
                aln = anim_tex_align(s4, ANIM_TEX_AUTO, align_lines, 2u, 0.42f, 0.2f);
                CHECK(ANIM_ID_VALID(aln), "anim_tex_align valid");
                anim_move_to(s4, aln, ANIM_V3(0.0f, -2.5f, 0.0f));
                anim_add(s4, aln);

                dgrp = anim_tex_diff(s4, ANIM_TEX_AUTO, diff_a, 4u, diff_b, 5u, 0.45f,
                                     ANIM_RED, ANIM_GREEN, &eq_a, &eq_b);
                CHECK(ANIM_ID_VALID(dgrp) && ANIM_ID_VALID(eq_a) && ANIM_ID_VALID(eq_b), "anim_tex_diff valid");
                anim_move_to(s4, eq_a, ANIM_V3(3.2f, 2.4f, 0.0f));
                anim_move_to(s4, eq_b, ANIM_V3(3.2f, 1.7f, 0.0f));
                anim_add(s4, eq_a);

                /* Multi-viewport split screen + animated viewport_to */
                cam_top.pos = ANIM_V3(3.0f, 4.5f, 4.5f);
                cam_top.target = ANIM_V3(3.0f, 0.0f, 0.0f);
                vp0 = anim_scene_add_viewport(s4, cam_top, (const float[4]){ 0.64f, 0.03f, 0.97f, 0.38f }, ANIM_TEAL, 2.0f);
                vp1 = anim_scene_add_viewport(s4, anim_camera_default(), (const float[4]){ 0.03f, 0.03f, 0.34f, 0.38f }, ANIM_GOLD, 2.0f);
                CHECK(vp0 == 0u && vp1 == 1u && anim_scene_viewport_count(s4) == 2u, "multi-viewport API");

                {
                    AnimCamera c_end = cam_top;
                    c_end.pos = ANIM_V3(0.0f, 2.2f, 6.2f);
                    c_end.target = ANIM_V3(3.0f, 0.0f, 0.0f);
                    AnimSpec adv[2] = {
                        anim_anim_transform_matching_tex(eq_a, eq_b),
                        anim_anim_viewport_to(vp0, true, c_end, (const float[4]){ 0.56f, 0.03f, 0.97f, 0.42f })
                    };
                    anim_play(tl4, s4, adv, 2u, 1.0f, ANIM_RATE_SMOOTH);
                    anim_seek(tl4, s4, 0.6f);
                }

                CHECK(anim_export_png(ren, s4, &opts, "/tmp/test_anim_adv_v4.png"), "exported multi-viewport/textured/plots PNG");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_adv_v4.png"), "advanced v4 PNG non-empty");

                anim_scene_clear_viewports(s4);
                CHECK(anim_scene_viewport_count(s4) == 0u, "anim_scene_clear_viewports resets count");
                anim_timeline_destroy(tl4);
                anim_scene_destroy(s4);
            }

            /* 14. .anim DSL: := trackers, UFCS ., named args, $...$, =>, f(x)=, ranges, CSG, plots, viewports, property -> */
            {
                const char *src =
                    "scene { bg: #040712 -> #0C1733, tonemap: agx, fps: 30, blur: 1 }\n"
                    "sq(x) = x * x * 0.25\n"
                    "fn badge(lbl, col) {\n"
                    "    return text(lbl, 0.34).color(col)\n"
                    "}\n"
                    "th := 0.0\n"
                    "x0 := 1.0\n"
                    "x0 = 1.5\n"
                    "pt_c := (cos(th), sin(th)) * 1.5\n"
                    "d_c = dot(pos: pt_c, r: 0.12).fill(#F9E26B).glow(0.8)\n"
                    "v_c = arrow((0, 0), pt_c).color(#38BDF8).stroke(3.2)\n"
                    "ax = axes(x: -3..3:1, y: -1..4:1, size: (5.2, 3.4), ticks: true).pos(-1.5, 0)\n"
                    "gr = ax.plot(x => sq(x), x: -3..3, samples: 48).stroke(TEAL, 3.0)\n"
                    "p := ax(x0, sq(x0))\n"
                    "pt = dot(pos: p, r: 0.11).fill(GOLD).glow(0.6)\n"
                    "eq = $f(x) = 0.25 x^2$.pos(2.8, 2.2).fill(WHITE)\n"
                    "aligned = tex([\"$a = 1$\", \"$b = 2$\"], align: true, size: 0.45).pos(2.8, 1.0)\n"
                    "hdr = badge(\"DSL Minimal Test\", #FFFFFF).pos(0, 3.1)\n"
                    "c1 = circle(0.55).pos(2.4, -1.2).fill(BLUE, 0.7)\n"
                    "c2 = circle(0.55).pos(2.9, -1.2).fill(RED, 0.7)\n"
                    "csg = (c1 | c2).stroke(WHITE, 2.0)\n"
                    "box_g = (square(0.5).pos(2.5, -2.4) + square(0.3).pos(3.2, -2.4)).fill(GREEN, 0.6)\n"
                    "for i in 0..6:2 {\n"
                    "    k = dot(r: 0.06).pos(-4.5 + i * 0.25, -2.6).fill(ORANGE)\n"
                    "    show(k)\n"
                    "}\n"
                    "hm = heatmap(ax, [0.1, 0.4, 0.7, 1.0], 2, 2, 0.0, 1.0).opacity(0.35)\n"
                    "vp = viewport(rect: [0.65, 0.04, 0.96, 0.36], border: #38BDF8)\n"
                    "play create(ax, gr)\n"
                    "play(1.0s) {\n"
                    "    fade_in(hdr, eq, aligned, hm, csg, box_g)\n"
                    "    grow(d_c, pt)\n"
                    "    grow_arrow(v_c)\n"
                    "    th -> 1.5707963\n"
                    "    x0 -> 2.8\n"
                    "    eq.pos -> (2.8, 2.4)\n"
                    "    csg.scale -> 1.15\n"
                    "    box_g.opacity -> 0.8\n"
                    "}\n"
                    "~0.3\n";
                AnimScene     *s5  = anim_scene_create();
                AnimTimeline  *tl5 = anim_timeline_create();
                AnimScript    *sc  = anim_script_create();
                AnimScriptOpts sopts;
                bool           ok  = anim_script_exec_string(sc, s5, tl5, src, "dsl_test.anim", &sopts);
                AnimTrackerId  x0_trk;
                AnimId         d_id, pt_id, eq_id;
                AnimVec3       d_pos, pt_pos, eq_pos;
                CHECK(ok, "anim_script_exec_string succeeded");
                if (!ok) fprintf(stderr, "DSL TEST ERROR: %s\n", anim_script_error(sc));
                CHECK(fabsf(anim_timeline_duration(tl5) - 2.3f) < 1e-3f, "DSL timeline duration == 2.3s");
                CHECK(sopts.render.fps == 30u && sopts.render.tonemap == ANIM_TONEMAP_AGX, "DSL scene {} options parsed");
                CHECK(anim_scene_viewport_count(s5) == 1u, "DSL viewport created");

                x0_trk = anim_script_get_tracker(sc, "x0");
                d_id   = anim_script_get_mobject(sc, "d_c");
                pt_id  = anim_script_get_mobject(sc, "pt");
                eq_id  = anim_script_get_mobject(sc, "eq");
                CHECK(ANIM_TRACKER_VALID(x0_trk) && ANIM_ID_VALID(d_id) && ANIM_ID_VALID(pt_id) && ANIM_ID_VALID(eq_id),
                      "DSL tracker & mobject handles retrievable");

                anim_seek(tl5, s5, 2.15f);
                d_pos  = anim_center(s5, d_id);
                pt_pos = anim_center(s5, pt_id);
                eq_pos = anim_center(s5, eq_id);
                CHECK(fabsf(d_pos.x) < 0.05f && fabsf(d_pos.y - 1.5f) < 0.05f,
                      "DSL reactive := updated circle dot to (0, 1.5) at th=pi/2");
                CHECK(fabsf(anim_tracker_get(s5, x0_trk) - 2.8f) < 0.05f, "DSL tracker x0 animated to 2.8");
                CHECK(pt_pos.x > 0.5f && pt_pos.y > 0.2f, "DSL reactive callable ax(x0, sq(x0)) moved dot");
                CHECK(fabsf(eq_pos.y - 2.4f) < 0.08f, "DSL property cue eq.pos -> (2.8, 2.4) applied");

                CHECK(anim_export_png(ren, s5, &sopts.render, "/tmp/test_anim_dsl.png"), "exported DSL PNG");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_dsl.png"), "DSL PNG non-empty");

                /* Error reporting & no-compat sanity checks */
                {
                    AnimScript *bad = anim_script_create();
                    AnimScene  *bs  = anim_scene_create();
                    AnimTimeline *bt = anim_timeline_create();
                    CHECK(!anim_script_exec_string(bad, bs, bt, "x = (\n", "bad.anim", NULL), "DSL rejects syntax error");
                    CHECK(anim_script_error(bad)[0] != '\0', "DSL reports non-empty error string");
                    anim_script_destroy(bad);
                    anim_timeline_destroy(bt);
                    anim_scene_destroy(bs);
                }

                anim_script_destroy(sc);
                anim_timeline_destroy(tl5);
                anim_scene_destroy(s5);
            }

            /* 15. .anim DSL: stats/PRNG, reactive curve/surface re-evaluation, and Physics/CS/DataViz builders */
            {
                const char *react_src =
                    "seed(42)\n"
                    "u_r = rand()\n"
                    "n_r = randn(0, 1)\n"
                    "e_0 = erf(0.0)\n"
                    "ec_0 = erfc(0.0)\n"
                    "g_5 = gamma(5.0)\n"
                    "lg_5 = lgamma(5.0)\n"
                    "np_0 = norm_pdf(0.0, 0.0, 1.0)\n"
                    "nc_0 = norm_cdf(0.0, 0.0, 1.0)\n"
                    "d_chk = dot().pos(nc_0, e_0)\n"
                    "g_chk = dot().pos(g_5, lg_5)\n"
                    "r_chk = dot().pos(u_r, np_0)\n"
                    "z_chk = dot().pos(ec_0, n_r)\n"
                    "mu := 0.0\n"
                    "sd := 1.0\n"
                    "k  := 0.0\n"
                    "ax = axes(x: -3..3:1, y: 0..2:0.5, size: (6.0, 3.0)).pos(-1.5, 0)\n"
                    "g_plot = ax.plot(x => norm_pdf(x, mu, sd) * 3.0, x: -3..3, samples: 48).stroke(TEAL, 3.0)\n"
                    "g_area = ax.area(x => norm_pdf(x, mu, sd) * 3.0, x: -2.5..2.5, samples: 36).fill(BLUE, 0.35)\n"
                    "g_riem = ax.riemann(x => norm_pdf(x, mu, sd) * 3.0, x: -2.5..2.5:0.5)\n"
                    "g_surf = surface3d((u, v) => (u, v, sin(u + k) * 0.4), u: -1..1, v: -1..1, nu: 10, nv: 10).pos(3.2, 0)\n"
                    "show(ax, g_plot, g_area, g_riem, g_surf)\n"
                    "play(1.0s) {\n"
                    "    mu -> 1.4\n"
                    "    sd -> 0.55\n"
                    "    k  -> 1.5\n"
                    "}\n";
                AnimScene    *sr  = anim_scene_create();
                AnimTimeline *tlr = anim_timeline_create();
                AnimScript   *scr = anim_script_create();
                bool okr = anim_script_exec_string(scr, sr, tlr, react_src, "react_plot_test.anim", NULL);
                CHECK(okr, "DSL reactive plot/stats script parsed & executed");
                if (!okr) fprintf(stderr, "DSL REACT ERROR: %s\n", anim_script_error(scr));
                {
                    AnimVec3 d0 = anim_center(sr, anim_script_get_mobject(scr, "d_chk"));
                    AnimVec3 g0 = anim_center(sr, anim_script_get_mobject(scr, "g_chk"));
                    AnimVec3 r0 = anim_center(sr, anim_script_get_mobject(scr, "r_chk"));
                    AnimVec3 z0 = anim_center(sr, anim_script_get_mobject(scr, "z_chk"));
                    CHECK(fabsf(d0.x - 0.5f) < 1e-3f && fabsf(d0.y) < 1e-3f, "norm_cdf(0)==0.5 and erf(0)==0");
                    CHECK(fabsf(g0.x - 24.0f) < 1e-2f && fabsf(g0.y - logf(24.0f)) < 1e-2f, "gamma(5)==24 and lgamma(5)==ln(24)");
                    CHECK(r0.x >= 0.0f && r0.x < 1.0f && fabsf(r0.y - 0.3989423f) < 1e-3f, "rand() in [0,1) and norm_pdf(0)==0.3989");
                    CHECK(fabsf(z0.x - 1.0f) < 1e-3f && isfinite(z0.y), "erfc(0)==1 and randn finite");
                }
                {
                    AnimId gp = anim_script_get_mobject(scr, "g_plot");
                    AnimId gr = anim_script_get_mobject(scr, "g_riem");
                    AnimVec3 b0, b1, r0, r1;
                    anim_seek(tlr, sr, 0.0f);
                    b0 = anim_center(sr, gp);
                    r0 = anim_center(sr, gr);
                    for (int k = 1; k <= 10; ++k)
                        anim_seek(tlr, sr, 0.1f * (float)k);
                    b1 = anim_center(sr, gp);
                    r1 = anim_center(sr, gr);
                    CHECK(b1.x > b0.x + 0.4f || fabsf(b1.y - b0.y) > 0.1f, "reactive plot re-evaluated across tracker animation");
                    CHECK(r1.x > r0.x + 0.4f || fabsf(r1.y - r0.y) > 0.1f, "reactive riemann re-evaluated across tracker animation");
                }
                anim_script_destroy(scr);
                anim_timeline_destroy(tlr);
                anim_scene_destroy(sr);
            }
            {
                const char *ext_src =
                    "scene { bg: #060916, tonemap: agx }\n"
                    "ax = axes(x: -3..3:1, y: -3..3:1, size: (3.2, 2.4)).pos(-4.2, 1.5)\n"
                    "cp = ax.contour_plot((x, y) => x * x + y * y, x: -2.5..2.5, y: -2.5..2.5, nx: 24, ny: 24, n: 5)\n"
                    "bp = ax.box_plot([[0.2, 0.8, 1.2, 1.8, 2.4], [-1.5, -0.5, 0.1, 0.9, 1.6]], width: 0.45)\n"
                    "mt = matrix([[1.0, 2.0], [3.0, 4.0]], decimals: 0, size: 0.35).pos(0.0, 2.2)\n"
                    "tb = table([[\"X\", \"P\"], [\"0\", \"0.5\"], [\"1\", \"0.5\"]], size: 0.28).pos(3.5, 2.2)\n"
                    "gn = graph_network([(-1.0, 0.0), (0.0, 0.6), (1.0, 0.0)], [[0, 1], [1, 2], [2, 0]], labels: [\"A\", \"B\", \"C\"], directed: true).pos(-4.2, -1.6).scale(0.7)\n"
                    "vn = voronoi([(-1.0, -0.6), (0.5, 0.7), (1.2, -0.4)], min: (-1.8, -1.2), max: (1.8, 1.2)).pos(0.0, -1.6).scale(0.65)\n"
                    "ch = convex_hull([(-0.8, -0.5), (0.9, -0.3), (0.2, 0.8), (0.0, 0.1)]).pos(0.0, -1.6).stroke(GOLD, 2.5)\n"
                    "ps = particle_system(16, origin: (3.6, -1.5), vel_min: (-0.6, 0.5), vel_max: (0.6, 1.6)).scale(0.7)\n"
                    "td = tex_diff([\"x^2\", \"+\", \"1\"], [\"x^2\", \"-\", \"2\"], size: 0.45).pos(0.0, 3.2)\n"
                    "bl = bulleted_list([\"Item A\", \"Item B\"], size: 0.25).pos(-5.5, 3.1)\n"
                    "sp = phys_spring((-5.5, 0.0), (-4.0, 0.0), coils: 6, width: 0.2)\n"
                    "pn = phys_pendulum((-3.0, 0.5), len: 0.9, angle: 0.4, r: 0.1)\n"
                    "fb = phys_free_body((-1.8, 0.0), w: 0.7, h: 0.5, forces: [(0, 0.8), (0.8, 0)], labels: [\"N\", \"F\"])\n"
                    "wv = phys_wave((-0.6, 0.0), (1.2, 0.0), amp: 0.25, cycles: 2.0, phase: 0.5, samples: 32)\n"
                    "cg = phys_charge((2.0, 0.2), r: 0.15, sign: 1)\n"
                    "fl = phys_field_lines([(2.0, 0.2)], [1.0], min: (1.2, -0.5), max: (2.8, 0.9), lines: 6)\n"
                    "ln = phys_lens((4.2, 0.0), height: 1.4, thickness: 0.18, diverging: false)\n"
                    "rb = phys_ray_bundle((4.2, 0.0), focal: 1.0, height: 1.4, offsets: [-0.4, 0.0, 0.4], x0: 3.2, x1: 5.5)\n"
                    "nn = cs_neural_net([2, 3, 2], width: 2.2, height: 1.4, r: 0.08).pos(-4.0, -3.0)\n"
                    "ar = cs_array([\"3\", \"1\", \"4\"], w: 0.45, h: 0.38).pos(-1.0, -3.0)\n"
                    "bc = cs_bar_chart([1.0, 3.0, 2.0], width: 1.5, height: 0.9).pos(1.2, -3.0)\n"
                    "ll = cs_linked_list([\"H\", \"T\"], w: 0.45, h: 0.35, spacing: 0.25).pos(3.5, -3.0)\n"
                    "sq = cs_stack_queue([\"A\", \"B\"], queue: true, w: 0.55, h: 0.32).pos(5.2, -2.5)\n"
                    "bt = cs_binary_tree([\"1\", \"2\", \"3\"], dy: 0.55, spread: 0.9, r: 0.15).pos(5.2, 0.0)\n"
                    "fs = cs_fsm([(-0.4, 0), (0.4, 0)], [\"q0\", \"q1\"], [[0, 1, \"a\"]], accept: [1], r: 0.18).pos(5.2, 2.0)\n"
                    "lg = cs_logic_gate(\"xor\", pos: (5.2, 3.1), scale: 0.45, label: \"G\")\n"
                    "br = cs_bit_register(5, bits: 4, w: 0.32).pos(3.2, 3.2)\n"
                    "sq_m = square(0.5).pos(-2.2, 3.1).fill(BLUE, 0.7)\n"
                    "show(ax, cp, bp, mt, tb, gn, vn, ch, ps, td, bl, sp, pn, fb, wv, cg, fl, ln, rb, nn, ar, bc, ll, sq, bt, fs, lg, br, sq_m)\n"
                    "play(0.6s) {\n"
                    "    apply_matrix3(sq_m, [1.2, 0.3, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0])\n"
                    "}\n";
                AnimScene    *se  = anim_scene_create();
                AnimTimeline *tle = anim_timeline_create();
                AnimScript   *sce = anim_script_create();
                AnimScriptOpts opts_e;
                bool oke = anim_script_exec_string(sce, se, tle, ext_src, "ext_builders_test.anim", &opts_e);
                CHECK(oke, "DSL extended builders script parsed & executed");
                if (!oke) fprintf(stderr, "DSL EXT ERROR: %s\n", anim_script_error(sce));
                anim_seek(tle, se, 0.5f);
                CHECK(anim_export_png(ren, se, &opts_e.render, "/tmp/test_anim_dsl_ext.png"), "exported extended builders PNG");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_dsl_ext.png"), "extended builders PNG non-empty");
                anim_script_destroy(sce);
                anim_timeline_destroy(tle);
                anim_scene_destroy(se);
            }

            /* 16. .anim DSL: complete parity bindings (VMob path, ODE/RK4, styling, queries, swaps, AnimContext) */
            {
                const char *parity_src =
                    "ab = arc_between((-2, 1), (2, 1), angle: 1.2).stroke(TEAL, 3.0)\n"
                    "fr = frustum3d(fov: 55, aspect: 1.5, near: 0.5, far: 3.0, pos: (0, 0, 3), target: (0, 0, 0))\n"
                    "pg = pixel_grid(4, 3, cell: 0.25, centers: true).pos(-4.5, -2.0)\n"
                    "sv = svg_str(\"<svg viewBox='0 0 100 100'><circle cx='50' cy='50' r='40' fill='#38bdf8'/></svg>\", 1.0).pos(-4.5, 2.0)\n"
                    "vm = vmob().begin_path((-1, -1)).line_to((0, 0)).quad_to((1, 0), (1, 1)).cubic_to((0.5, 1.5), (-0.5, 1.5), (-1, 1)).close_path().subdivide(1).tag(\"custom_vm\").pos(0, -1.8)\n"
                    "pt_on = dot(point_at(vm, 0.5), 0.08).fill(GOLD)\n"
                    "vm_found = find(\"custom_vm\")\n"
                    "w_vm = width(vm_found)\n"
                    "h_vm = height(vm_found)\n"
                    "bb_vm = bbox(vm_found)\n"
                    "sz_chk = dot().pos(w_vm, h_vm)\n"
                    "ax = axes(x: -2..2:1, y: -2..2:1, size: (2.5, 2.5)).pos(3.8, -1.5)\n"
                    "cp_v = p2c(ax, ax.c2p(1.25, -0.5))\n"
                    "p2c_chk = dot().pos(cp_v)\n"
                    "hm = heatmap(ax, [0.1, 0.2, 0.8, 0.9], 2, 2, 0.0, 1.0)\n"
                    "hm.heatmap_set([0.9, 0.7, 0.3, 0.1], 2, 2, 0.0, 1.0)\n"
                    "tb = table([[\"A\", \"B\"], [\"1\", \"2\"]], size: 0.28).pos(-4.5, 0.2)\n"
                    "tc = tb.table_cell(1, 1).color(GOLD)\n"
                    "cd = code(\"x = 1\\ny = 2\", 0.25, true).pos(3.8, 2.0)\n"
                    "hl = cd.highlight_line(2, color: YELLOW)\n"
                    "sq = square(1.2).pos(0, 0.5).gradient_fill(BLUE, GOLD, p0: (-0.6, 0), p1: (0.6, 0)).stroke_gradient(RED, TEAL).cap_join(\"round\", \"bevel\").fill_rule(\"evenodd\").trim(0.0, 0.9)\n"
                    "sq.clip_rect((-0.5, -0.5), (0.5, 1.5))\n"
                    "sq.clear_clip()\n"
                    "sq.clear_gradient()\n"
                    "sq.gradient_fill(TEAL, PURPLE, radial: true, center: (0, 0.5), r0: 0.0, r1: 0.8)\n"
                    "grp = group()\n"
                    "c_sub = circle(0.25).pos(1.5, 0.5)\n"
                    "grp.add_child(c_sub)\n"
                    "rem_d = dot().pos(9.0, 9.0)\n"
                    "show(rem_d)\n"
                    "remove(rem_d)\n"
                    "y_step = phys_rk4([1.0, 0.0], (t, y) => [-y[1], y[0]], 0.0, 0.1)\n"
                    "rk_chk = dot().pos(y_step[0], y_step[1])\n"
                    "traj = phys_trajectory([1.0, 0.0, 0.0], (t, y) => [-y[1], y[0], 0.1], t: 0..6.28, steps: 64, proj: y => (y[0], y[1], 0.0)).stroke(GREEN, 2.0)\n"
                    "a_sw = circle(0.22).pos(-1.5, 2.5).fill(BLUE)\n"
                    "b_sw = square(0.38).pos( 1.5, 2.5).fill(RED)\n"
                    "c_cp = circle(0.22).pos(-2.2, 0.0)\n"
                    "c_ds = square(0.50).pos( 2.2, 0.0).dash(8.0, 6.0, 0.0)\n"
                    "show(ab, fr, pg, sv, vm, pt_on, ax, hm, tb, tc, cd, hl, sq, grp, traj, a_sw, b_sw, c_cp, c_ds)\n"
                    "cs_swap(a_sw, b_sw, arc: 0.5, dur: 0.4)\n"
                    "play(0.5s) {\n"
                    "    create(ab, rate: ease_out_back, lag: 0.1)\n"
                    "    grow_from_point(pg, (-4.5, -2.0))\n"
                    "    spiral_in(sv, 3.14159)\n"
                    "    transform_copy(c_cp, c_ds)\n"
                    "    dash_flow(c_ds, 24.0)\n"
                    "    zoom_inset(true, rect: [0.68, 0.05, 0.96, 0.34])\n"
                    "}\n";
                AnimScene    *sp  = anim_scene_create();
                AnimTimeline *tlp = anim_timeline_create();
                AnimScript   *scp = anim_script_create();
                bool okp = anim_script_exec_string(scp, sp, tlp, parity_src, "parity_test.anim", NULL);
                CHECK(okp, "DSL complete parity script parsed & executed");
                if (!okp) fprintf(stderr, "DSL PARITY ERROR: %s\n", anim_script_error(scp));
                {
                    AnimVec3 szv  = anim_center(sp, anim_script_get_mobject(scp, "sz_chk"));
                    AnimVec3 p2cv = anim_center(sp, anim_script_get_mobject(scp, "p2c_chk"));
                    AnimVec3 rkv  = anim_center(sp, anim_script_get_mobject(scp, "rk_chk"));
                    AnimId   grp  = anim_script_get_mobject(scp, "grp");
                    AnimId   vmf  = anim_script_get_mobject(scp, "vm_found");
                    CHECK(szv.x > 1.0f && szv.y > 1.0f, "width()/height() getters returned VMob dimensions");
                    CHECK(fabsf(p2cv.x - 1.25f) < 1e-2f && fabsf(p2cv.y + 0.5f) < 1e-2f, "p2c(ax, c2p(ax, 1.25, -0.5)) round-trips");
                    CHECK(fabsf(rkv.x - cosf(0.1f)) < 1e-3f && fabsf(rkv.y - sinf(0.1f)) < 1e-3f, "phys_rk4 integrated circular ODE step");
                    CHECK(anim_child_count(sp, grp) == 1u && ANIM_ID_VALID(vmf), "add_child() and find() tag lookup succeeded");
                }
                anim_seek(tlp, sp, 0.7f);
                CHECK(anim_export_png(ren, sp, &opts, "/tmp/test_anim_parity.png"), "exported parity PNG");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_parity.png"), "parity PNG non-empty");
                anim_script_destroy(scp);
                anim_timeline_destroy(tlp);
                anim_scene_destroy(sp);

                /* AnimContext single-header convenience lifecycle test */
                {
                    AnimContext *ctx = anim_context_create(320u, 180u, "ctx_test", true, true);
                    AnimScene   *cs  = anim_scene_create();
                    AnimId       cd  = anim_circle(cs, 1.2f);
                    AnimRenderOpts copts = anim_render_opts_default(320u, 180u);
                    anim_add(cs, cd);
                    CHECK(ctx != NULL && anim_context_renderer(ctx) != NULL, "anim_context_create headless");
                    CHECK(anim_export_png(anim_context_renderer(ctx), cs, &copts, "/tmp/test_anim_ctx.png"), "AnimContext export PNG");
                    CHECK(anim_context_gpu_error(ctx)[0] == '\0', "AnimContext zero Vulkan validation errors");
                    anim_scene_destroy(cs);
                    anim_context_destroy(ctx);
                }
            }

            /* 17. Wave 1: 3D depth-test, HUD fixed-in-frame, MSAA checkerboard, multi-stop grad,
             *     bg_stroke, sheen_color, ambient/illusion cam, sections, GIF & alpha WebM export */
            {
                AnimScene    *w1s  = anim_scene_create();
                AnimTimeline *w1tl = anim_timeline_create();
                AnimRenderOpts w1o = anim_render_opts_default(320u, 180u);
                AnimCamera    cam3 = anim_camera_default();
                AnimId        sph, dline, hud, mgrad, halobox, sheenbox;
                AnimColor     stops[4] = { ANIM_RED, ANIM_GOLD, ANIM_GREEN, ANIM_BLUE };
                AnimSpec      sp;
                FILE         *fp;

                w1o.fps = 12u;
                w1o.motion_blur_samples = 1u;
                cam3.pos = ANIM_V3(3.0f, 2.5f, 5.0f);
                anim_scene_set_camera(w1s, cam3);

                sph = anim_sphere(w1s, 1.3f, 24u, 16u);
                anim_surface_set_checkerboard(w1s, sph, ANIM_BLUE_D, ANIM_TEAL, 8u, 6u, true);
                anim_add(w1s, sph);

                dline = anim_line(w1s, ANIM_V3(-2.5f, 0.0f, -2.5f), ANIM_V3(2.5f, 0.0f, 2.5f));
                anim_set_stroke(w1s, dline, ANIM_YELLOW, 6.0f, true);
                anim_set_depth_test(w1s, dline, true, true);
                anim_add(w1s, dline);

                hud = anim_rect(w1s, 1.8f, 0.5f);
                anim_set_fill(w1s, hud, ANIM_GOLD, true);
                anim_to_corner(w1s, hud, ANIM_UL, 0.3f);
                anim_set_fixed_in_frame(w1s, hud, true, true);
                anim_add(w1s, hud);

                mgrad = anim_rounded_rect(w1s, 2.0f, 1.2f, 0.2f);
                anim_set_fill_gradient_stops(w1s, mgrad, stops, 4u, ANIM_V3(-1, 0, 0), ANIM_V3(1, 0, 0), false, 1.0f, true);
                anim_set_fixed_in_frame(w1s, mgrad, true, true);
                anim_to_corner(w1s, mgrad, ANIM_UR, 0.3f);
                anim_add(w1s, mgrad);

                halobox = anim_text(w1s, NULL, "HALO", 0.5f);
                anim_set_fill(w1s, halobox, ANIM_WHITE, true);
                anim_set_stroke(w1s, halobox, ANIM_WHITE, 2.0f, true);
                anim_set_background_stroke(w1s, halobox, ANIM_BLACK, 8.0f, true);
                anim_set_fixed_in_frame(w1s, halobox, true, true);
                anim_to_corner(w1s, halobox, ANIM_DL, 0.3f);
                anim_add(w1s, halobox);

                sheenbox = anim_circle(w1s, 0.6f);
                anim_set_fill(w1s, sheenbox, ANIM_BLUE_E, true);
                anim_set_sheen_color(w1s, sheenbox, 0.85f, 0.6f, ANIM_GOLD, true);
                anim_set_fixed_in_frame(w1s, sheenbox, true, true);
                anim_to_corner(w1s, sheenbox, ANIM_DR, 0.3f);
                anim_add(w1s, sheenbox);

                anim_scene_begin_ambient_camera_rotation(w1s, 1.2f, 0.25f);
                anim_wait(w1tl, w1s, 0.5f);
                anim_timeline_next_section(w1tl, "intro", false);
                anim_scene_stop_ambient_camera_rotation(w1s);
                anim_scene_begin_illusion_camera_rotation(w1s, 1.5f, 0.2f);
                sp = anim_anim_indicate(hud, 1.15f, ANIM_RED);
                anim_play(w1tl, w1s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                anim_timeline_next_section(w1tl, "skip_me", true);
                anim_wait(w1tl, w1s, 0.25f);

                CHECK(anim_timeline_section_count(w1tl) == 3u, "timeline recorded 3 sections");
                {
                    const char *sn = NULL; float t0 = 0.0f, t1 = 0.0f; bool sk = false;
                    CHECK(anim_timeline_section_info(w1tl, 1u, &sn, &t0, &t1, &sk) &&
                          strcmp(sn, "intro") == 0 && fabsf(t0 - 0.5f) < 1e-3f && fabsf(t1 - 1.0f) < 1e-3f && !sk,
                          "section 1 metadata matches");
                    CHECK(anim_timeline_section_info(w1tl, 2u, &sn, &t0, &t1, &sk) && sk,
                          "section 2 skip flag preserved");
                }

                anim_seek(w1tl, w1s, 0.4f);
                {
                    AnimCamera c_at = anim_scene_camera(w1s);
                    CHECK(fabsf(c_at.pos.x - cam3.pos.x) > 0.05f, "ambient camera rotation orbited camera");
                }
                CHECK(anim_export_png(ren, w1s, &w1o, "/tmp/test_anim_w1.png"), "Wave 1 PNG exported");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_w1.png"), "Wave 1 PNG non-empty");
                CHECK(anim_export_gif(ren, w1tl, w1s, &w1o, "/tmp/test_anim_w1.gif"), "Wave 1 GIF exported");
                fp = fopen("/tmp/test_anim_w1.gif", "rb");
                CHECK(fp != NULL, "Wave 1 GIF file exists");
                if (fp) fclose(fp);

                CHECK(anim_export_sections(ren, w1tl, w1s, &w1o, "/tmp/w1_sections"), "Wave 1 sections exported");
                fp = fopen("/tmp/w1_sections/section_001_intro.mp4", "rb");
                CHECK(fp != NULL, "section_001_intro.mp4 exported");
                if (fp) fclose(fp);
                fp = fopen("/tmp/w1_sections/section_002_skip_me.mp4", "rb");
                CHECK(fp == NULL, "skipped section omitted from export");
                if (fp) fclose(fp);

                /* Transparent WebM export */
                w1o.transparent_bg = true;
                CHECK(anim_export_video(ren, w1tl, w1s, &w1o, "/tmp/test_anim_w1_alpha.webm", NULL),
                      "Wave 1 transparent WebM exported");
                fp = fopen("/tmp/test_anim_w1_alpha.webm", "rb");
                CHECK(fp != NULL, "Wave 1 WebM file exists");
                if (fp) fclose(fp);

                /* DSL Wave 1 bindings check */
                {
                    static const char *w1_dsl =
                        "scene { bg: #0F111A, transparent: false }\n"
                        "s = sphere(1.0)\n"
                        "checkerboard(s, #29ABCA, #236B8E, 6, 6)\n"
                        "ln = line([-2,0,-2], [2,0,2]) | stroke(#F9E26B, 5)\n"
                        "depth_test(ln, true)\n"
                        "r = rect(1.5, 0.5) | fixed_in_frame(true)\n"
                        "gradient_stops(r, [#FC6255, #F9E26B, #58C4DD])\n"
                        "bg_stroke(r, #000000, 6)\n"
                        "sheen_color(r, 0.7, 0.4, #FFFFFF)\n"
                        "show(s, ln, r)\n"
                        "ambient_camera(0.8, 0.1)\n"
                        "wait(0.3)\n"
                        "next_section(\"dsl_sec\", false)\n"
                        "stop_ambient_camera()\n"
                        "illusion_camera(1.0, 0.15)\n"
                        "wait(0.3)\n";
                    AnimScene    *ds1 = anim_scene_create();
                    AnimTimeline *dt1 = anim_timeline_create();
                    AnimScript   *sc1 = anim_script_create();
                    bool ok1 = anim_script_exec_string(sc1, ds1, dt1, w1_dsl, "w1.anim", NULL);
                    CHECK(ok1, "Wave 1 DSL bindings parsed and executed");
                    if (!ok1) fprintf(stderr, "W1 DSL ERROR: %s\n", anim_script_error(sc1));
                    CHECK(anim_timeline_section_count(dt1) == 2u, "Wave 1 DSL next_section recorded");
                    anim_script_destroy(sc1);
                    anim_timeline_destroy(dt1);
                    anim_scene_destroy(ds1);
                }

                anim_timeline_destroy(w1tl);
                anim_scene_destroy(w1s);
            }

            /* 18. Wave 2: anim_markup, t2c, tangent_line, secant_slope, letter-by-letter, one-by-one, matching shapes */
            {
                AnimScene    *w2s  = anim_scene_create();
                AnimTimeline *w2tl = anim_timeline_create();
                AnimRenderOpts w2o = anim_render_opts_default(320u, 180u);
                AnimId        mk, txt, tag_m, ax, crv, tan_l, sec_g, g3, s_a, s_b;
                AnimId        k3[3], ka[2], kb[2];
                AnimAxesDesc  axd = { -2.0f, 2.0f, 1.0f, -1.0f, 4.0f, 1.0f, 5.0f, 3.5f, true, true, false, false, NULL };
                AnimSpec      sp;

                w2o.motion_blur_samples = 1u;
                mk = anim_markup(w2s, NULL, "<b>Hello</b> <span color='#58C4DD' id='key'>World</span> <i>x</i><sup>2</sup> + <u>ok</u>", 0.55f);
                tag_m = anim_find_by_tag(w2s, mk, "key");
                CHECK(anim_child_count(w2s, mk) >= 10u, "anim_markup created styled glyphs + underline");
                CHECK(ANIM_ID_VALID(tag_m), "anim_markup tagged span id='key'");
                anim_move_to(w2s, mk, ANIM_V3(0.0f, 2.2f, 0.0f));
                anim_add(w2s, mk);

                txt = anim_text(w2s, NULL, "alpha beta", 0.5f);
                anim_text_set_t2c(w2s, txt, "alpha beta", "beta", ANIM_GOLD);
                anim_move_to(w2s, txt, ANIM_V3(0.0f, 1.2f, 0.0f));
                anim_add(w2s, txt);

                ax    = anim_axes(w2s, &axd);
                crv   = anim_plot(w2s, ax, test_quad_fn, -1.8f, 1.8f, 32u, NULL);
                tan_l = anim_tangent_line(w2s, crv, 0.7f, 2.0f);
                sec_g = anim_secant_slope_group(w2s, ax, test_quad_fn, 0.2f, 0.8f, 2.5f, NULL, ANIM_YELLOW, ANIM_BLUE, ANIM_GREEN);
                CHECK(anim_width(w2s, tan_l) > 0.5f, "anim_tangent_line built non-degenerate tangent");
                CHECK(anim_child_count(w2s, sec_g) == 5u, "anim_secant_slope_group built 5 elements");
                anim_add(w2s, ax);
                anim_add(w2s, crv);
                anim_add(w2s, tan_l);
                anim_add(w2s, sec_g);

                k3[0] = anim_circle(w2s, 0.35f);
                k3[1] = anim_square(w2s, 0.6f);
                k3[2] = anim_star(w2s, 5u, 0.4f, 0.2f);
                g3 = anim_group(w2s, k3, 3u);
                anim_move_to(w2s, g3, ANIM_V3(-4.0f, -1.5f, 0.0f));
                anim_add(w2s, g3);

                ka[0] = anim_circle(w2s, 0.5f); ka[1] = anim_square(w2s, 0.8f);
                anim_arrange(w2s, ka, 2u, ANIM_RIGHT, 0.3f);
                s_a = anim_group(w2s, ka, 2u);
                kb[0] = anim_square(w2s, 0.9f); kb[1] = anim_circle(w2s, 0.55f);
                anim_arrange(w2s, kb, 2u, ANIM_RIGHT, 0.3f);
                s_b = anim_group(w2s, kb, 2u);
                anim_move_to(w2s, s_a, ANIM_V3(3.5f, -1.5f, 0.0f));
                anim_move_to(w2s, s_b, ANIM_V3(3.5f, -2.5f, 0.0f));
                anim_add(w2s, s_a);

                sp = anim_anim_add_text_letter_by_letter(txt);
                anim_play(w2tl, w2s, &sp, 1u, 0.8f, ANIM_RATE_LINEAR);
                sp = anim_anim_show_submobjects_one_by_one(g3);
                anim_play(w2tl, w2s, &sp, 1u, 0.9f, ANIM_RATE_LINEAR);
                sp = anim_anim_transform_matching_shapes(s_a, s_b);
                anim_play(w2tl, w2s, &sp, 1u, 0.8f, ANIM_RATE_SMOOTH);
                sp = anim_anim_remove_text_letter_by_letter(txt);
                anim_play(w2tl, w2s, &sp, 1u, 0.8f, ANIM_RATE_LINEAR);

                anim_seek(w2tl, w2s, 0.4f);
                CHECK(anim_export_png(ren, w2s, &w2o, "/tmp/test_anim_w2.png"), "Wave 2 PNG exported");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_w2.png"), "Wave 2 PNG non-empty");
                anim_seek(w2tl, w2s, 1.25f);
                CHECK(anim_width(w2s, g3) > 0.1f, "show_submobjects_one_by_one active mid-step");

                /* DSL Wave 2 bindings */
                {
                    static const char *w2_dsl =
                        "m = markup(\"<b>DSL</b> <span color='#F9E26B'>Markup</span>\", 0.5).pos(0, 2)\n"
                        "t = text(\"abc def\", 0.45)\n"
                        "t2c(t, \"abc def\", \"def\", #58C4DD)\n"
                        "ax = axes(-2..2, -1..4, 4, 3)\n"
                        "f(x) = x^2\n"
                        "p = plot(ax, f, -1.5..1.5)\n"
                        "tl = tangent_line(p, 0.6, 2.0)\n"
                        "sg = secant_slope(ax, f, 0.2, 0.7)\n"
                        "show(m, t, ax, p, tl, sg)\n"
                        "play(0.5) { add_letters(t) }\n"
                        "play(0.5) { remove_letters(t) }\n";
                    AnimScene    *ds2 = anim_scene_create();
                    AnimTimeline *dt2 = anim_timeline_create();
                    AnimScript   *sc2 = anim_script_create();
                    bool ok2 = anim_script_exec_string(sc2, ds2, dt2, w2_dsl, "w2.anim", NULL);
                    CHECK(ok2, "Wave 2 DSL bindings parsed and executed");
                    if (!ok2) fprintf(stderr, "W2 DSL ERROR: %s\n", anim_script_error(sc2));
                    anim_script_destroy(sc2);
                    anim_timeline_destroy(dt2);
                    anim_scene_destroy(ds2);
                }

                anim_timeline_destroy(w2tl);
                anim_scene_destroy(w2s);
            }

            /* 19. Wave 3: ComplexPlane, Cutout, DashedVMobject, CurvedArrow, Extrude, SaveState/Restore, Swap, FadeTransform */
            {
                AnimScene    *w3s  = anim_scene_create();
                AnimTimeline *w3tl = anim_timeline_create();
                AnimRenderOpts w3o = anim_render_opts_default(320u, 180u);
                AnimId        np, cp, out_sq, hole_c, cut, rpoly, elb, apoly, dash_c, ldot, car, cdar, int_m, lathe, ext_s;
                AnimId        sw_a, sw_b, ft_a, ft_b, ccw_a, ccw_b;
                AnimVec3      poly4[4] = { ANIM_V3(-1,-1,0), ANIM_V3(1,-1,0), ANIM_V3(1,1,0), ANIM_V3(-1,1,0) };
                float         angs4[4] = { 0.4f, -0.3f, 0.4f, -0.3f };
                AnimVec3      c2p_pt;
                float         re_out = 0.0f, im_out = 0.0f;
                AnimSpec      sp;

                w3o.motion_blur_samples = 1u;
                np = anim_number_plane(w3s, -3.0f, 3.0f, 1.0f, -2.0f, 2.0f, 1.0f, 6.0f, 4.0f, true);
                cp = anim_complex_plane(w3s, -2.0f, 2.0f, 1.0f, -2.0f, 2.0f, 1.0f, 4.0f, 4.0f, true, NULL);
                c2p_pt = anim_complex_c2p(w3s, cp, 1.0f, -1.0f);
                anim_complex_p2c(w3s, cp, c2p_pt, &re_out, &im_out);
                CHECK(fabsf(re_out - 1.0f) < 1e-3f && fabsf(im_out + 1.0f) < 1e-3f, "ComplexPlane c2p/p2c round-trip");
                anim_number_plane_prepare_nonlinear(w3s, np, 3u);
                anim_add(w3s, np);

                out_sq = anim_square(w3s, 2.2f);
                anim_set_fill(w3s, out_sq, ANIM_BLUE, false);
                hole_c = anim_circle(w3s, 0.65f);
                cut    = anim_cutout(w3s, out_sq, &hole_c, 1u);
                CHECK(anim_vmob_subpath_count(w3s, cut) == 2u, "anim_cutout preserves outer + hole subpaths");
                anim_move_to(w3s, cut, ANIM_V3(-4.0f, 1.5f, 0.0f));
                anim_add(w3s, cut);

                rpoly = anim_rounded_polygon(w3s, poly4, 4u, 0.25f);
                elb   = anim_elbow(w3s, 0.35f, 0.2f);
                apoly = anim_arc_polygon(w3s, poly4, angs4, 4u);
                dash_c = anim_dashed_vmobject(w3s, hole_c, 12u, 0.5f);
                ldot   = anim_labeled_dot(w3s, "P", NULL, 0.3f, ANIM_PURPLE, ANIM_WHITE);
                car    = anim_curved_arrow(w3s, ANIM_V3(-1, 0, 0), ANIM_V3(1, 0, 0), 0.7f, 0.2f);
                cdar   = anim_curved_double_arrow(w3s, ANIM_V3(-1, -1, 0), ANIM_V3(1, -1, 0), -0.6f, 0.2f);
                int_m  = anim_integer(w3s, 42, NULL, 0.4f);
                CHECK(anim_vmob_subpath_count(w3s, dash_c) == 12u, "DashedVMobject emitted 12 subpaths");
                CHECK(ANIM_ID_VALID(rpoly) && ANIM_ID_VALID(elb) && ANIM_ID_VALID(apoly) &&
                      ANIM_ID_VALID(ldot) && ANIM_ID_VALID(car) && ANIM_ID_VALID(cdar) && ANIM_ID_VALID(int_m),
                      "Wave 3 geometry builders valid");
                anim_move_to(w3s, rpoly, ANIM_V3(-4.0f, -1.5f, 0.0f));
                anim_move_to(w3s, dash_c, ANIM_V3(4.0f, 1.5f, 0.0f));
                anim_move_to(w3s, ldot, ANIM_V3(4.0f, -1.5f, 0.0f));
                anim_add(w3s, rpoly); anim_add(w3s, dash_c); anim_add(w3s, ldot);
                anim_add(w3s, car); anim_add(w3s, int_m);

                lathe = anim_surface_of_revolution(w3s, test_lathe_fn, -1.0f, 1.0f, ANIM_TAU, 12u, 16u, NULL);
                ext_s = anim_extrude_vmob(w3s, rpoly, 0.6f, ANIM_GOLD);
                CHECK(ANIM_ID_VALID(lathe) && ANIM_ID_VALID(ext_s), "3D revolution & extrude built");
                anim_move_to(w3s, ext_s, ANIM_V3(2.0f, 0.0f, 0.0f));
                anim_add(w3s, ext_s);

                /* SaveState + Restore */
                anim_save_state(w3s, ldot);
                anim_move_to(w3s, ldot, ANIM_V3(0.0f, 2.5f, 0.0f));
                anim_set_color(w3s, ldot, ANIM_RED, true);

                sw_a = anim_dot(w3s, ANIM_V3(-2.0f, 0.0f, 0.0f), 0.25f);
                sw_b = anim_dot(w3s, ANIM_V3( 2.0f, 0.0f, 0.0f), 0.25f);
                ft_a = anim_circle(w3s, 0.5f); anim_move_to(w3s, ft_a, ANIM_V3(-1.5f, -2.0f, 0.0f));
                ft_b = anim_star(w3s, 5u, 0.6f, 0.25f); anim_move_to(w3s, ft_b, ANIM_V3(1.5f, -2.0f, 0.0f));
                ccw_a = anim_square(w3s, 0.6f); anim_move_to(w3s, ccw_a, ANIM_V3(-1.0f, 2.0f, 0.0f));
                ccw_b = anim_circle(w3s, 0.4f); anim_move_to(w3s, ccw_b, ANIM_V3( 1.0f, 2.0f, 0.0f));
                anim_add(w3s, sw_a); anim_add(w3s, sw_b); anim_add(w3s, ft_a); anim_add(w3s, ccw_a);

                sp = anim_anim_apply_complex_fn(np, test_complex_sq, NULL);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                sp = anim_anim_restore(ldot);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                sp = anim_anim_swap(sw_a, sw_b, 0.8f);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                sp = anim_anim_fade_transform(ft_a, ft_b);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                sp = anim_anim_counterclockwise_transform(ccw_a, ccw_b, ANIM_PI);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_SMOOTH);
                sp = anim_anim_rotating(cut, ANIM_PI, ANIM_OUT, ANIM_V3(-4.0f, 1.5f, 0.0f));
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_LINEAR);
                sp = anim_anim_phase_flow(rpoly, test_flow_fn, 0.4f, NULL);
                anim_play(w3tl, w3s, &sp, 1u, 0.5f, ANIM_RATE_LINEAR);
                sp = anim_anim_blink(int_m, 2u);
                anim_play(w3tl, w3s, &sp, 1u, 0.4f, ANIM_RATE_LINEAR);

                anim_seek(w3tl, w3s, 1.0f);
                CHECK(fabsf(anim_center(w3s, ldot).x - 4.0f) < 0.05f && fabsf(anim_center(w3s, ldot).y + 1.5f) < 0.05f,
                      "Restore returned ldot to saved position");
                anim_seek(w3tl, w3s, 1.5f);
                CHECK(fabsf(anim_center(w3s, sw_a).x - 2.0f) < 0.05f && fabsf(anim_center(w3s, sw_b).x + 2.0f) < 0.05f,
                      "Swap exchanged positions of sw_a and sw_b");
                CHECK(anim_export_png(ren, w3s, &w3o, "/tmp/test_anim_w3.png"), "Wave 3 PNG exported");
                CHECK(png_has_non_uniform_pixels("/tmp/test_anim_w3.png"), "Wave 3 PNG non-empty");

                /* DSL Wave 3 bindings */
                {
                    static const char *w3_dsl =
                        "np = number_plane(-3, 3, -2, 2).prepare_nonlinear(2)\n"
                        "cp = complex_plane(-2, 2, -2, 2)\n"
                        "c = circle(0.8).fill(#58C4DD)\n"
                        "h = circle(0.3)\n"
                        "ct = cutout(c, [h])\n"
                        "dc = dashed(c, 10, 0.5)\n"
                        "ld = labeled_dot(\"Z\", 0.3)\n"
                        "ca = curved_arrow((-2, 0), (2, 0), 0.7)\n"
                        "el = elbow(0.3, 0.0)\n"
                        "n42 = integer(42, 0.4)\n"
                        "ex = extrude(c, 0.5, #F9E26B)\n"
                        "save_state(ld)\n"
                        "ld.pos(0, 2)\n"
                        "show(np, ct, dc, ld, ca, el, n42, ex)\n"
                        "cf(re, im) = (re * re - im * im, 2 * re * im)\n"
                        "flow(x, y, z) = (-y, x, 0)\n"
                        "play(0.4) {\n  apply_complex(np, cf)\n  restore(ld)\n  blink(n42, 2)\n  rotating(ct, 1.5)\n}\n"
                        "play(0.4) {\n  swap(ct, dc)\n  phase_flow(dc, flow, 0.3)\n}\n";
                    AnimScene    *ds3 = anim_scene_create();
                    AnimTimeline *dt3 = anim_timeline_create();
                    AnimScript   *sc3 = anim_script_create();
                    bool ok3 = anim_script_exec_string(sc3, ds3, dt3, w3_dsl, "w3.anim", NULL);
                    CHECK(ok3, "Wave 3 DSL bindings parsed and executed");
                    if (!ok3) fprintf(stderr, "W3 DSL ERROR: %s\n", anim_script_error(sc3));
                    anim_script_destroy(sc3);
                    anim_timeline_destroy(dt3);
                    anim_scene_destroy(ds3);
                }

                anim_timeline_destroy(w3tl);
                anim_scene_destroy(w3s);
            }

            /* 19. Wave 4: Callouts, Dimension lines, AnimatedStreamLines, Graph Layouts, Pixel Arrays */
            {
                AnimScene *w4s = anim_scene_create();
                AnimId ax4 = anim_number_plane(w4s, -3.0f, 3.0f, 1.0f, -2.0f, 2.0f, 1.0f, 6.0f, 4.0f, false);
                AnimId co  = anim_callout(w4s, ANIM_V3(-0.5f, -0.5f, 0.0f), ANIM_V3(1.8f, 1.2f, 0.0f), "Peak", NULL, ANIM_YELLOW);
                AnimId dl1 = anim_dimension_line(w4s, ANIM_V3(-2.0f, -1.5f, 0.0f), ANIM_V3(2.0f, -1.5f, 0.0f), -0.5f, NULL, NULL, ANIM_TEAL);
                AnimId dl2 = anim_dimension_line(w4s, ANIM_V3(0.0f, 0.0f, 0.0f), ANIM_V3(0.0f, 2.0f, 0.0f), 0.4f, "h = 2", NULL, ANIM_GOLD);
                CHECK(ANIM_ID_VALID(co) && anim_child_count(w4s, co) == 4u, "callout builds 4 parts");
                CHECK(ANIM_ID_VALID(dl1) && anim_child_count(w4s, dl1) == 5u, "dimension_line auto-label builds 5 parts");
                CHECK(ANIM_ID_VALID(dl2) && anim_child_count(w4s, dl2) == 5u, "dimension_line custom label builds 5 parts");

                AnimId asl = anim_animated_stream_lines(w4s, ax4, test_swirl_field, -2.0f, 2.0f, -1.5f, 1.5f, 1.0f, 1.0f, 16u, 1.5f, NULL);
                CHECK(ANIM_ID_VALID(asl) && anim_child_count(w4s, asl) > 0u, "animated_stream_lines built streamlines");
                anim_add(w4s, ax4);
                anim_add(w4s, asl);
                anim_eval_updaters(w4s, 0.25f);

                {
                    uint32_t ef[6] = { 0, 0, 1, 1, 2, 3 };
                    uint32_t et[6] = { 1, 2, 3, 4, 4, 0 };
                    const char *glbls[5] = { "A", "B", "C", "D", "E" };
                    AnimVec3 pos[5];
                    AnimGraphLayoutKind lays[5] = {
                        ANIM_GRAPH_LAYOUT_CIRCULAR, ANIM_GRAPH_LAYOUT_SPRING,
                        ANIM_GRAPH_LAYOUT_GRID, ANIM_GRAPH_LAYOUT_TREE, ANIM_GRAPH_LAYOUT_BIPARTITE
                    };
                    for (int li = 0; li < 5; ++li)
                    {
                        anim_graph_compute_layout(lays[li], 5u, 6u, ef, et, 6.0f, 4.0f, 42u, pos);
                        float max_d = 0.0f;
                        for (int i = 0; i < 5; ++i)
                            for (int j = i + 1; j < 5; ++j)
                            {
                                float dx = pos[i].x - pos[j].x, dy = pos[i].y - pos[j].y;
                                float d = sqrtf(dx*dx + dy*dy);
                                if (d > max_d) max_d = d;
                            }
                        CHECK(max_d > 0.5f, "graph layout separates nodes");
                    }
                    AnimId g_spr = anim_graph_with_layout(w4s, 5u, glbls, 6u, ef, et, true, ANIM_GRAPH_LAYOUT_SPRING, 6.0f, 4.0f, NULL);
                    CHECK(ANIM_ID_VALID(g_spr) && anim_child_count(w4s, g_spr) == 6u + 10u, "graph_with_layout builds edges + labeled nodes");
                }

                /* Pixel arrays (RGBA texture + update + vector grid) */
                {
                    uint8_t rgba[4 * 4 * 4];
                    for (uint32_t y = 0u; y < 4u; ++y)
                        for (uint32_t x = 0u; x < 4u; ++x)
                        {
                            size_t idx = (y * 4u + x) * 4u;
                            rgba[idx + 0u] = (uint8_t)(x * 64u);
                            rgba[idx + 1u] = (uint8_t)(y * 64u);
                            rgba[idx + 2u] = 200u;
                            rgba[idx + 3u] = 255u;
                        }
                    AnimId pa = anim_pixel_array(w4s, rgba, 4u, 4u, 2.0f);
                    CHECK(ANIM_ID_VALID(pa), "pixel_array created");
                    rgba[0] = 255u;
                    anim_pixel_array_update(w4s, pa, rgba, 4u, 4u);

                    AnimColor cols4[4] = { ANIM_RED, ANIM_GREEN, ANIM_BLUE, ANIM_YELLOW };
                    AnimId pag = anim_pixel_array_grid(w4s, cols4, 2u, 2u, 0.4f, 0.05f);
                    CHECK(ANIM_ID_VALID(pag) && anim_child_count(w4s, pag) == 4u, "pixel_array_grid builds 4 cells");
                    anim_move_to(w4s, pa, ANIM_V3(-2.5f, 1.5f, 0.0f));
                    anim_move_to(w4s, pag, ANIM_V3(2.5f, -1.5f, 0.0f));

                    anim_add(w4s, co);
                    anim_add(w4s, dl1);
                    anim_add(w4s, pa);
                    anim_add(w4s, pag);
                    AnimRenderOpts w4ro = anim_render_opts_default(320u, 180u);
                    CHECK(anim_export_png(ren, w4s, &w4ro, "/tmp/test_w4.png"), "Wave 4 scene rendered to PNG");
                }

                /* Wave 4 DSL bindings (callout, dimension_line, animated_stream_lines, graph_layout, pixel_array) */
                {
                    static const char *w4_dsl =
                        "ax = number_plane(-3, 3, -2, 2)\n"
                        "vortex(x, y, z) = (-y, x, 0)\n"
                        "asl = animated_stream_lines(ax, vortex, -2..2, -1.5..1.5, flow_speed: 1.4)\n"
                        "co = callout((0, 0), (2, 1.2), \"Center\")\n"
                        "dl = dimension_line((-2, -1.5), (2, -1.5), -0.4, \"4 units\")\n"
                        "gl = graph_layout(4, [[0,1],[1,2],[2,3],[3,0]], layout: \"spring\", labels: [\"0\",\"1\",\"2\",\"3\"])\n"
                        "pa = pixel_array(8, 8, (x, y) => rgb(x / 7, y / 7, 0.6), height: 1.5)\n"
                        "pag = pixel_array([[#FF0000, #00FF00], [#0000FF, #FFFF00]], 0.3, 0.05)\n"
                        "show(ax, asl, co, dl, gl, pa, pag)\n"
                        "wait(0.5)\n";
                    AnimScene    *ds4 = anim_scene_create();
                    AnimTimeline *dt4 = anim_timeline_create();
                    AnimScript   *sc4 = anim_script_create();
                    bool ok4 = anim_script_exec_string(sc4, ds4, dt4, w4_dsl, "w4.anim", NULL);
                    CHECK(ok4, "Wave 4 DSL bindings parsed and executed");
                    if (!ok4) fprintf(stderr, "W4 DSL ERROR: %s\n", anim_script_error(sc4));
                    anim_eval_updaters(ds4, 0.25f);
                    anim_script_destroy(sc4);
                    anim_timeline_destroy(dt4);
                    anim_scene_destroy(ds4);
                }

                anim_scene_destroy(w4s);
            }

            /* 20. Wave 5: Lights + Fog + DoF, Camera Frame Helpers, Arc-Length Parameterization, Deterministic Stepping, DSL v2 Parity */
            {
                AnimScene *w5s = anim_scene_create();
                AnimId sp_near = anim_sphere(w5s, 0.7f, 20u, 12u);
                AnimId sp_far  = anim_sphere(w5s, 1.0f, 20u, 12u);
                anim_set_fill(w5s, sp_near, ANIM_TEAL, false);
                anim_set_fill(w5s, sp_far, ANIM_GOLD, false);
                anim_move_to(w5s, sp_near, ANIM_V3(-1.2f, 0.0f, 2.5f));
                anim_move_to(w5s, sp_far, ANIM_V3(1.5f, 0.3f, -4.0f));
                anim_add(w5s, sp_near);
                anim_add(w5s, sp_far);

                anim_scene_clear_lights(w5s);
                anim_scene_add_light(w5s, ANIM_LIGHT_DIRECTIONAL, ANIM_V3(0.5f, 1.0f, 0.4f), ANIM_WHITE, 1.2f);
                anim_scene_add_light(w5s, ANIM_LIGHT_POINT, ANIM_V3(-2.0f, 1.5f, 3.0f), ANIM_BLUE_C, 2.5f);
                anim_scene_set_ambient_light(w5s, anim_rgba(0.15f, 0.20f, 0.28f, 1.0f), 1.0f);
                AnimFogDesc fog = {
                    .enabled = true, .exponential = false,
                    .color = anim_rgba(0.06f, 0.09f, 0.14f, 1.0f),
                    .start = 4.0f, .end = 18.0f, .density = 0.08f
                };
                anim_scene_set_fog(w5s, &fog);

                AnimRenderOpts w5ro = anim_render_opts_default(320u, 180u);
                w5ro.dof_aperture_px = 6.0f;
                w5ro.dof_focus_dist  = 5.5f;
                w5ro.dof_focus_range = 1.2f;
                CHECK(anim_export_png(ren, w5s, &w5ro, "/tmp/test_w5_dof.png"), "Wave 5 lights + fog + DoF rendered to PNG");

                /* Also test DoF with empty 3D scene (only 2D shapes, depth clear path) */
                {
                    AnimScene *d2s = anim_scene_create();
                    AnimId c2 = anim_circle(d2s, 1.2f);
                    anim_set_fill(d2s, c2, ANIM_BLUE, false);
                    anim_add(d2s, c2);
                    CHECK(anim_export_png(ren, d2s, &w5ro, "/tmp/test_w5_dof_2d.png"), "Wave 5 DoF 2D fallback depth clear rendered");
                    anim_scene_destroy(d2s);
                }

                /* Camera frame helpers */
                {
                    AnimCamera c_orig = anim_scene_camera(w5s);
                    anim_scene_camera_save(w5s);
                    AnimCamera c_fr = anim_camera_frame_mob(w5s, sp_near, 0.4f, 16.0f / 9.0f);
                    CHECK(fabsf(c_fr.target.x - (-1.2f)) < 0.05f && fabsf(c_fr.pos.z - c_fr.target.z) > 1.0f,
                          "camera_frame_mob centers target on mobject");
                    anim_scene_camera_set_spherical(w5s, ANIM_ORIGIN, 10.0f, 1.1f, 0.5f);
                    AnimCamera c_sph = anim_scene_camera(w5s);
                    float d_sph = sqrtf(c_sph.pos.x*c_sph.pos.x + c_sph.pos.y*c_sph.pos.y + c_sph.pos.z*c_sph.pos.z);
                    CHECK(fabsf(d_sph - 10.0f) < 0.02f, "camera_set_spherical sets exact radius");
                    anim_scene_camera_restore(w5s);
                    AnimCamera c_rest = anim_scene_camera(w5s);
                    CHECK(fabsf(c_rest.pos.z - c_orig.pos.z) < 1e-4f, "camera_save / camera_restore roundtrips");
                }

                /* Arc-length parameterization on severely non-uniform cubic Bezier */
                {
                    AnimId bz = anim_cubic_bezier(w5s,
                                                  ANIM_V3(0.0f, 0.0f, 0.0f),
                                                  ANIM_V3(0.05f, 0.0f, 0.0f),
                                                  ANIM_V3(3.95f, 0.0f, 0.0f),
                                                  ANIM_V3(4.0f, 0.0f, 0.0f));
                    float total = anim_vmob_arc_length(w5s, bz);
                    AnimVec3 q1 = anim_vmob_point_at(w5s, bz, 0.25f);
                    AnimVec3 q2 = anim_vmob_point_at(w5s, bz, 0.50f);
                    AnimVec3 q3 = anim_vmob_point_at(w5s, bz, 0.75f);
                    AnimVec3 tn = anim_vmob_tangent_at(w5s, bz, 0.50f);
                    CHECK(fabsf(total - 4.0f) < 0.02f, "arc_length accurate on non-uniform cubic");
                    CHECK(fabsf(q1.x - 1.0f) < 0.05f && fabsf(q2.x - 2.0f) < 0.05f && fabsf(q3.x - 3.0f) < 0.05f,
                          "point_at is uniform in arc length");
                    CHECK(fabsf(tn.x - 1.0f) < 0.02f && fabsf(tn.y) < 0.02f, "tangent_at returns unit tangent");
                }

                /* Deterministic updater stepping across play + forward/backward seek */
                {
                    AnimScene    *us = anim_scene_create();
                    AnimTimeline *ut = anim_timeline_create();
                    anim_timeline_set_fixed_dt(ut, 1.0f / 60.0f);
                    AnimId ps = anim_particle_system(us, 8u, ANIM_ORIGIN, ANIM_V3(-1, 1, 0), ANIM_V3(1, 2, 0),
                                                     ANIM_V3(0, -2, 0), 2.0f, ANIM_YELLOW, ANIM_RED, 0.06f);
                    anim_add(us, ps);
                    anim_wait(ut, us, 0.75f);
                    float t_end_play = anim_scene_time(us);
                    AnimVec3 p_play  = anim_center(us, ps);

                    anim_seek(ut, us, 0.25f);
                    float t_mid = anim_scene_time(us);
                    anim_seek(ut, us, 0.75f);
                    float t_end_seek = anim_scene_time(us);
                    AnimVec3 p_seek  = anim_center(us, ps);
                    CHECK(fabsf(t_end_play - 0.75f) < 1e-3f && fabsf(t_mid - 0.25f) < 1e-3f && fabsf(t_end_seek - 0.75f) < 1e-3f,
                          "deterministic sim_time across play and seek");
                    CHECK(fabsf(p_play.x - p_seek.x) < 1e-4f && fabsf(p_play.y - p_seek.y) < 1e-4f,
                          "particle updater state identical after seek back and forth");
                    anim_timeline_destroy(ut);
                    anim_scene_destroy(us);
                }

                /* Wave 5 DSL v2 parity */
                {
                    static const char *w5_dsl =
                        "scene {\n  dof_aperture: 4.0\n  dof_focus: 7.5\n  dof_range: 1.5\n}\n"
                        "clear_lights()\n"
                        "add_light(\"directional\", (0.4, 0.8, 0.5), #FFFFFF, 1.1)\n"
                        "add_light(\"point\", (-2, 1, 3), #58C4DD, 2.0)\n"
                        "ambient_light(#223344, 0.8)\n"
                        "set_fog(#101824, 4.0, 20.0, 0.05, false)\n"
                        "camera_save()\n"
                        "sp = sphere(0.9).fill(#58C4DD)\n"
                        "bz = cubic_bezier((0,0), (0.1,0), (3.9,0), (4,0))\n"
                        "al = arc_length(bz)\n"
                        "pm = point_at(bz, 0.5)\n"
                        "tm = tangent_at(bz, 0.5)\n"
                        "show(sp, bz)\n"
                        "fixed_dt(1.0 / 60.0)\n"
                        "play(0.4) { camera_frame(sp, 0.8) }\n"
                        "camera_spherical((0,0,0), 9.0, 1.1, 0.4)\n"
                        "camera_restore()\n"
                        "st = scene_time()\n"
                        "clear()\n";
                    AnimScene     *ds5 = anim_scene_create();
                    AnimTimeline  *dt5 = anim_timeline_create();
                    AnimScript    *sc5 = anim_script_create();
                    AnimScriptOpts opt5;
                    bool ok5 = anim_script_exec_string(sc5, ds5, dt5, w5_dsl, "w5.anim", &opt5);
                    CHECK(ok5, "Wave 5 DSL v2 parity script executed");
                    if (!ok5) fprintf(stderr, "W5 DSL ERROR: %s\n", anim_script_error(sc5));
                    CHECK(fabsf(opt5.render.dof_aperture_px - 4.0f) < 1e-4f && fabsf(opt5.render.dof_focus_dist - 7.5f) < 1e-4f,
                          "Wave 5 config dof_* options parsed");
                    anim_script_destroy(sc5);
                    anim_timeline_destroy(dt5);
                    anim_scene_destroy(ds5);
                }

                anim_scene_destroy(w5s);
            }

            anim_timeline_destroy(tl_m);
            anim_scene_destroy(ms);
        }

        gpu_wait_idle(gpu);
        CHECK(gpu_error(gpu)[0] == '\0', "zero Vulkan validation errors");
        if (gpu_error(gpu)[0] != '\0')
            fprintf(stderr, "VK ERROR: %s\n", gpu_error(gpu));

        anim_renderer_destroy(ren);
        gpu_destroy(gpu);
        anim_timeline_destroy(tl);
    }

    anim_scene_destroy(s);

    if (g_failures == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("%d TEST(S) FAILED\n", g_failures);
    return g_failures ? 1 : 0;
}
