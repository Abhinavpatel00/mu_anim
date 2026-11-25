/*
 * main.c — flagship 4-act beyond-Manim showcase for mu_anim.
 *
 * Usage:
 *   ./mu_anim                          Interactive preview (Space/Left/Right/B/H/C/Drag/Wheel/F2/Esc)
 *   ./mu_anim --shot out.png           Render single frame to PNG (headless)
 *   ./mu_anim --shot out.png --time 9  Render frame at t=9.0s to PNG
 *   ./mu_anim --video demo.mp4         Export full MP4 via triple-buffered ffmpeg pipe (headless)
 *   options: --width N --height N --fps N --blur N --preset 720p|1080p|4k|vertical|square
 *            --validate 0|1 --stats 0|1 --alpha 0|1
 */
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_common.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "third_party/mugfx/desktop/platform.h"
#include "third_party/mugfx/desktop/vk.h"
#include "anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float demo_fn(float x, void *user)
{
    (void)user;
    return 0.25f * x * x + 0.4f;
}

static float demo_fn_deriv(float x)
{
    return 0.5f * x;
}

static AnimVec3 demo_surface_fn(float u, float v, void *user)
{
    (void)user;
    float r = sqrtf(u * u + v * v);
    float y = 1.10f * cosf(r * 1.45f) * expf(-0.05f * r * r);
    return ANIM_V3(u, y, v);
}

static AnimColor demo_surface_color_fn(float u, float v, AnimVec3 pos, AnimVec3 normal, void *user)
{
    (void)u;
    (void)v;
    (void)normal;
    (void)user;
    float t = MU_CLAMP((pos.y + 0.7f) / 1.8f, 0.0f, 1.0f);
    return (t < 0.5f)
        ? anim_color_lerp_oklab(ANIM_BLUE_D, ANIM_TEAL, t * 2.0f)
        : anim_color_lerp_oklab(ANIM_TEAL, ANIM_GOLD, (t - 0.5f) * 2.0f);
}

static AnimVec3 demo_helix3d_fn(float t, void *user)
{
    (void)user;
    return ANIM_V3(1.85f * cosf(t), 0.75f + 0.22f * t, 1.85f * sinf(t));
}

static AnimVec3 wave_warp_fn(AnimVec3 p, void *user)
{
    (void)user;
    return ANIM_V3(p.x + 0.14f * sinf(p.y * 1.8f), p.y + 0.22f * sinf(p.x * 1.4f), p.z);
}

typedef struct DemoUpdaterCtx {
    AnimTrackerId x_tracker;
    AnimId        axes;
    AnimId        dot;
} DemoUpdaterCtx;

static void update_dot_on_curve(AnimScene *s, AnimId target, float dt, void *user)
{
    DemoUpdaterCtx *ctx = (DemoUpdaterCtx *)user;
    float           x   = anim_tracker_get(s, ctx->x_tracker);
    (void)dt;
    anim_move_to(s, target, anim_axes_c2p(s, ctx->axes, x, demo_fn(x, NULL)));
}

static void update_tangent_arrow(AnimScene *s, AnimId target, float dt, void *user)
{
    DemoUpdaterCtx *ctx = (DemoUpdaterCtx *)user;
    float           x   = anim_tracker_get(s, ctx->x_tracker);
    float           y   = demo_fn(x, NULL);
    float           m   = demo_fn_deriv(x);
    AnimVec3        p0  = anim_axes_c2p(s, ctx->axes, x - 0.55f, y - 0.55f * m);
    AnimVec3        p1  = anim_axes_c2p(s, ctx->axes, x + 0.55f, y + 0.55f * m);
    (void)dt;
    anim_vmob_clear_paths(s, target);
    anim_vmob_begin_subpath(s, target, p0);
    anim_vmob_line_to(s, target, p1);
}

static void update_readout_num(AnimScene *s, AnimId target, float dt, void *user)
{
    DemoUpdaterCtx *ctx = (DemoUpdaterCtx *)user;
    float           x   = anim_tracker_get(s, ctx->x_tracker);
    (void)dt;
    anim_decimal_number_set(s, target, demo_fn(x, NULL));
}

static void write_ambient_wav(const char *path, float duration_sec)
{
    enum { SR = 44100 };
    uint32_t total = (uint32_t)(duration_sec * (float)SR);
    FILE    *f     = fopen(path, "wb");
    uint32_t data_bytes = total * 4u;
    uint32_t riff_bytes = 36u + data_bytes;
    uint16_t fmt_pcm    = 1u;
    uint16_t ch         = 2u;
    uint32_t sr         = SR;
    uint32_t byte_rate  = SR * 4u;
    uint16_t align      = 4u;
    uint16_t bps        = 16u;
    uint32_t i;

    if (!f)
        return;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_bytes, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    { uint32_t s16 = 16u; fwrite(&s16, 4, 1, f); }
    fwrite(&fmt_pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);

    for (i = 0u; i < total; ++i)
    {
        float    t   = (float)i / (float)SR;
        float    env = MU_MIN(t / 0.8f, 1.0f) * MU_MIN((duration_sec - t) / 1.0f, 1.0f);
        float    pad = 0.08f * sinf(ANIM_TAU * 130.81f * t)
                     + 0.06f * sinf(ANIM_TAU * 196.00f * t)
                     + 0.05f * sinf(ANIM_TAU * 246.94f * t)
                     + 0.04f * sinf(ANIM_TAU * 293.66f * t);
        float    bell = 0.0f;
        static const float CUES[4] = { 0.2f, 3.6f, 8.2f, 12.2f };
        int      k;
        int16_t  lr[2];

        for (k = 0; k < 4; ++k)
        {
            if (t >= CUES[k])
            {
                float dt = t - CUES[k];
                bell += 0.10f * expf(-dt * 2.8f) * sinf(ANIM_TAU * (523.25f + (float)k * 65.4f) * dt);
            }
        }
        {
            float   v  = MU_CLAMP((pad + bell) * env, -0.95f, 0.95f);
            int16_t sv = (int16_t)(v * 32767.0f);
            lr[0] = sv;
            lr[1] = sv;
            fwrite(lr, sizeof(int16_t), 2, f);
        }
    }
    fclose(f);
}

static void build_demo_scene(AnimScene *s, AnimTimeline *tl, DemoUpdaterCtx *ctx)
{
    /* ---- Act 1 (0..3.6s): Styled Vector Cards + Typst Morph + LaTeX ------ */
    AnimId title = anim_text(s, NULL, "Geometry, Code & 3D Fields", 0.56f);
    AnimId ul    = anim_line(s, ANIM_V3(-4.5f, 0.0f, 0.0f), ANIM_V3(4.5f, 0.0f, 0.0f));
    AnimId card1 = anim_rounded_rect(s, 8.6f, 2.25f, 0.24f);
    AnimId card2 = anim_rounded_rect(s, 8.6f, 1.85f, 0.24f);
    AnimId eq1   = anim_tex(s, ANIM_TEX_TYPST, "{{a^2}} + {{b^2}} = {{c^2}}", 0.80f);
    AnimId eq2   = anim_tex(s, ANIM_TEX_TYPST, "{{c^2}} - {{b^2}} = {{a^2}}", 0.80f);
    AnimId ltx   = anim_tex(s, ANIM_TEX_LATEX, "\\int_0^3 \\left(\\frac{1}{4}x^2 + 0.4\\right)\\,dx = 3.45", 0.92f);

    anim_set_color(s, title, ANIM_BLUE_A, true);
    anim_to_edge(s, title, ANIM_UP, 0.42f);
    anim_set_stroke(s, ul, ANIM_TEAL, 2.5f, false);
    anim_set_glow(s, ul, 0.7f, false);
    anim_next_to(s, ul, title, ANIM_DOWN, 0.14f);

    anim_move_to(s, card1, ANIM_V3(0.0f, 1.05f, 0.0f));
    anim_set_fill_gradient_linear(s, card1, anim_hex_a(0x162238, 0.90f), anim_hex_a(0x132F3D, 0.90f),
                                  ANIM_V3(-4.3f, 2.1f, 0.0f), ANIM_V3(4.3f, 0.0f, 0.0f), false);
    anim_set_stroke(s, card1, ANIM_BLUE_C, 2.4f, false);
    anim_set_dash(s, card1, 20.0f, 10.0f, 0.0f, false);
    anim_set_shadow(s, card1, 16.0f, false);
    anim_set_z_index(s, card1, -0.5f, false);

    anim_move_to(s, card2, ANIM_V3(0.0f, -1.55f, 0.0f));
    anim_set_fill_gradient_linear(s, card2, anim_hex_a(0x1D1B30, 0.88f), anim_hex_a(0x18253A, 0.88f),
                                  ANIM_V3(-4.3f, -0.6f, 0.0f), ANIM_V3(4.3f, -2.5f, 0.0f), false);
    anim_set_stroke(s, card2, ANIM_GOLD, 2.0f, false);
    anim_set_shadow(s, card2, 14.0f, false);
    anim_set_z_index(s, card2, -0.5f, false);

    {
        AnimId ta1 = anim_find_by_tag(s, eq1, "a^2");
        AnimId tb1 = anim_find_by_tag(s, eq1, "b^2");
        AnimId tc1 = anim_find_by_tag(s, eq1, "c^2");
        AnimId ta2 = anim_find_by_tag(s, eq2, "a^2");
        AnimId tb2 = anim_find_by_tag(s, eq2, "b^2");
        AnimId tc2 = anim_find_by_tag(s, eq2, "c^2");
        anim_set_color(s, ta1, ANIM_BLUE, true);  anim_set_color(s, ta2, ANIM_BLUE, true);
        anim_set_color(s, tb1, ANIM_TEAL, true);  anim_set_color(s, tb2, ANIM_TEAL, true);
        anim_set_color(s, tc1, ANIM_GOLD, true);  anim_set_color(s, tc2, ANIM_GOLD, true);
    }
    anim_move_to(s, eq1, ANIM_V3(0.0f, 1.05f, 0.0f));
    anim_move_to(s, eq2, ANIM_V3(0.0f, 1.05f, 0.0f));
    anim_move_to(s, ltx, ANIM_V3(0.0f, -1.55f, 0.0f));
    anim_set_color(s, ltx, ANIM_GREY_A, true);

    {
        AnimSpec s0[4] = {
            anim_anim_write(title),
            anim_anim_create(ul),
            anim_anim_fade_in(card1, ANIM_UP, 0.94f),
            anim_anim_write(eq1),
        };
        anim_play_at(tl, s, 0.0f, s0, 4u, 1.25f, ANIM_RATE_SPRING);
    }
    {
        AnimSpec dash_sp = anim_anim_dash_flow(card1, 180.0f);
        anim_play_at(tl, s, 0.4f, &dash_sp, 1u, 3.0f, ANIM_RATE_LINEAR);
    }
    {
        AnimSpec s1[3] = {
            anim_anim_transform_matching_tex(eq1, eq2),
            anim_anim_fade_in(card2, ANIM_UP, 0.94f),
            anim_anim_write(ltx),
        };
        anim_play_at(tl, s, 1.35f, s1, 3u, 1.35f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec s2[1] = { anim_anim_circumscribe(ltx, ANIM_GOLD, 0.16f) };
        anim_play_at(tl, s, 2.75f, s2, 1u, 0.75f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.10f);

    /* ---- Act 2 (3.6..8.2s): Riemann + ValueTracker DAG + Code & Table ---- */
    {
        AnimAxesDesc ad = { -0.5f, 3.8f, 1.0f, -0.4f, 3.6f, 1.0f, 6.2f, 4.4f, true, true, true, true, NULL };
        AnimId       axes  = anim_axes(s, &ad);
        AnimId       curve, area, riem1, riem2;
        AnimId       dot   = anim_dot(s, ANIM_ORIGIN, 0.11f);
        AnimId       tang  = anim_line(s, ANIM_ORIGIN, ANIM_RIGHT);
        AnimId       lbl_x = anim_text(s, NULL, "f(x) =", 0.36f);
        AnimId       num   = anim_decimal_number(s, demo_fn(0.4f, NULL), 2u, NULL, 0.36f);
        AnimId       badge = anim_rounded_rect(s, 2.3f, 0.72f, 0.12f);
        const char  *code_src =
            "float quad(float x) {\n"
            "    return 0.25*x*x + 0.4;\n"
            "}";
        AnimId       code  = anim_code(s, NULL, code_src, 0.24f, true);
        AnimId       hil   = anim_code_highlight_line(s, code, 2u, anim_hex_a(0xF9E26B, 0.22f));
        const char  *tbl_c[6] = { "Method", "Result", "Riemann", "3.45", "Exact", "3.45" };
        AnimId       tbl   = anim_table(s, tbl_c, 3u, 2u, NULL, 0.26f, true);
        AnimTrackerId tr   = anim_tracker_create(s, 0.4f);

        anim_move_to(s, axes, ANIM_V3(-2.9f, -0.35f, 0.0f));
        curve = anim_plot(s, axes, demo_fn, -0.3f, 3.4f, 48u, NULL);
        area  = anim_area_under_curve(s, axes, demo_fn, 0.0f, 3.0f, 36u, NULL);
        riem1 = anim_riemann_rectangles(s, axes, demo_fn, 0.0f, 3.0f, 0.50f, NULL, ANIM_BLUE_D, ANIM_TEAL);
        riem2 = anim_riemann_rectangles(s, axes, demo_fn, 0.0f, 3.0f, 0.16f, NULL, ANIM_BLUE_C, ANIM_GREEN);

        anim_set_stroke(s, curve, ANIM_GOLD, 4.2f, false);
        anim_set_glow(s, curve, 0.7f, false);
        anim_set_fill(s, dot, ANIM_YELLOW, false);
        anim_set_glow(s, dot, 1.0f, false);
        anim_set_stroke(s, tang, ANIM_RED, 3.6f, false);
        anim_set_glow(s, tang, 0.55f, false);

        anim_move_to(s, badge, ANIM_V3(-4.2f, 2.35f, 0.0f));
        anim_set_fill(s, badge, anim_hex_a(0x182032, 0.90f), false);
        anim_set_stroke(s, badge, ANIM_BLUE, 1.8f, false);
        anim_set_z_index(s, badge, -0.2f, false);
        anim_move_to(s, lbl_x, ANIM_V3(-4.72f, 2.35f, 0.0f));
        anim_set_color(s, lbl_x, ANIM_GREY_A, true);
        anim_next_to(s, num, lbl_x, ANIM_RIGHT, 0.14f);
        anim_set_color(s, num, ANIM_YELLOW, true);

        anim_move_to(s, code, ANIM_V3(3.35f, 1.35f, 0.0f));
        anim_move_to(s, hil, ANIM_V3(3.35f, 1.35f, 0.0f));
        anim_move_to(s, tbl, ANIM_V3(3.35f, -1.45f, 0.0f));

        {
            AnimSpec fade_act1[8] = {
                anim_anim_fade_out(eq1, ANIM_UP, 0.9f),
                anim_anim_fade_out(ltx, ANIM_DOWN, 0.9f),
                anim_anim_fade_out(card1, ANIM_UP, 0.9f),
                anim_anim_fade_out(card2, ANIM_DOWN, 0.9f),
                anim_anim_create(axes),
                anim_anim_create(curve),
                anim_anim_fade_in(code, ANIM_LEFT, 0.95f),
                anim_anim_fade_in(tbl, ANIM_LEFT, 0.95f),
            };
            anim_play(tl, s, fade_act1, 8u, 1.15f, ANIM_RATE_SMOOTH);
        }
        {
            AnimSpec show_riem[5] = {
                anim_anim_fade_in(area, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(riem1, ANIM_UP, 0.95f),
                anim_anim_fade_in(badge, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(lbl_x, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(hil, ANIM_ORIGIN, 1.0f),
            };
            anim_play(tl, s, show_riem, 5u, 0.75f, ANIM_RATE_SMOOTH);
        }
        {
            AnimSpec refine[1] = { anim_anim_replacement_transform(riem1, riem2) };
            anim_play(tl, s, refine, 1u, 1.0f, ANIM_RATE_SMOOTH);
        }

        ctx->x_tracker = tr;
        ctx->axes      = axes;
        ctx->dot       = dot;
        anim_add(s, dot);
        anim_add(s, tang);
        anim_add(s, num);
        anim_add_updater(s, dot, update_dot_on_curve, ctx, NULL, 0u, &tr, 1u);
        anim_add_updater(s, tang, update_tangent_arrow, ctx, &dot, 1u, &tr, 1u);
        anim_add_updater(s, num, update_readout_num, ctx, &dot, 1u, &tr, 1u);

        {
            AnimSpec sweep[1] = { anim_anim_tracker_to(tr, 3.0f) };
            anim_play(tl, s, sweep, 1u, 1.7f, ANIM_RATE_SMOOTH);
        }

        anim_clear_updaters(s, dot);
        anim_clear_updaters(s, tang);
        anim_clear_updaters(s, num);

        /* ---- Act 3 (8.2..12.2s): True Z-Buffered 3D Lit Parametric Surface + Helix + Orbit */
        {
            AnimId ax3d  = anim_axes3d(s, 2.8f, 1.6f, 2.8f);
            AnimId surf  = anim_surface3d(s, demo_surface_fn, demo_surface_color_fn,
                                          -2.4f, 2.4f, -2.4f, 2.4f, 40u, 40u, NULL);
            AnimId helix = anim_parametric_curve(s, demo_helix3d_fn, 0.0f, ANIM_TAU * 1.5f, 72u, NULL);
            AnimId tracer = anim_dot(s, ANIM_ORIGIN, 0.11f);
            AnimId cap3d  = anim_text(s, NULL, "Z-Buffered 3D Surface + fwidth Grid", 0.44f);
            AnimCamera cam_3d_start = anim_camera_default();

            anim_surface_set_wireframe(s, surf, 10.0f, 0.42f, false);
            anim_surface_set_roughness(s, surf, 0.24f, false);
            anim_set_stroke(s, helix, ANIM_GOLD, 3.8f, false);
            anim_set_glow(s, helix, 0.85f, false);
            anim_set_fill(s, tracer, ANIM_YELLOW, false);
            anim_set_glow(s, tracer, 1.2f, false);
            anim_set_color(s, cap3d, ANIM_BLUE_A, true);
            anim_to_edge(s, cap3d, ANIM_UP, 0.45f);

            cam_3d_start.pos    = ANIM_V3(5.6f, 3.8f, 6.2f);
            cam_3d_start.target = ANIM_V3(0.0f, 0.2f, 0.0f);

            {
                AnimSpec trans_act3[12] = {
                    anim_anim_fade_out(axes, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(curve, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(area, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(riem2, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(dot, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(tang, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(badge, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(lbl_x, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(num, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(code, ANIM_RIGHT, 0.9f),
                    anim_anim_fade_out(hil, ANIM_RIGHT, 0.9f),
                    anim_anim_fade_out(tbl, ANIM_RIGHT, 0.9f),
                };
                anim_play(tl, s, trans_act3, 12u, 0.70f, ANIM_RATE_SMOOTH);
            }
            {
                AnimSpec fade_hdr[2] = {
                    anim_anim_fade_out(title, ANIM_UP, 0.95f),
                    anim_anim_fade_out(ul, ANIM_UP, 0.95f),
                };
                anim_play(tl, s, fade_hdr, 2u, 0.35f, ANIM_RATE_SMOOTH);
            }
            {
                AnimSpec enter3d[5] = {
                    anim_anim_camera_to(cam_3d_start),
                    anim_anim_fade_in(ax3d, ANIM_ORIGIN, 0.95f),
                    anim_anim_fade_in(surf, ANIM_ORIGIN, 0.90f),
                    anim_anim_create(helix),
                    anim_anim_write(cap3d),
                };
                anim_play(tl, s, enter3d, 5u, 1.15f, ANIM_RATE_SMOOTH);
            }
            {
                AnimSpec orbit_sp[2] = {
                    anim_anim_camera_orbit(1.45f, -0.15f, 0.92f),
                    anim_anim_follow_path(tracer, helix, false),
                };
                anim_play(tl, s, orbit_sp, 2u, 1.80f, ANIM_RATE_SMOOTH);
            }

            /* ---- Act 4 (12.2..16.0s): Network Graph + Warped Rings + Finale */
            {
                AnimVec3 g_pos[5] = {
                    ANIM_V3(-4.2f,  1.0f, 0.0f),
                    ANIM_V3(-2.6f,  1.9f, 0.0f),
                    ANIM_V3(-2.6f,  0.1f, 0.0f),
                    ANIM_V3(-1.0f,  1.2f, 0.0f),
                    ANIM_V3(-1.0f, -0.5f, 0.0f),
                };
                const char *g_lbls[5] = { "In", "H1", "H2", "Out", "Loss" };
                uint32_t    g_from[6] = { 0u, 0u, 1u, 1u, 2u, 2u };
                uint32_t    g_to[6]   = { 1u, 2u, 3u, 4u, 3u, 4u };
                AnimId      gnet      = anim_graph_network(s, 5u, g_pos, g_lbls, 6u, g_from, g_to, true, NULL);
                AnimId      ring_w    = anim_circle(s, 1.65f);
                AnimId      logo      = anim_text(s, NULL, "mu_anim", 0.96f);
                AnimId      br;
                AnimCamera  cam_home  = anim_camera_default();

                anim_move_to(s, ring_w, ANIM_V3(2.8f, 0.4f, 0.0f));
                anim_set_stroke(s, ring_w, ANIM_TEAL, 4.2f, false);
                anim_set_glow(s, ring_w, 0.95f, false);
                anim_vmob_apply_warp(s, ring_w, wave_warp_fn, NULL, 6u);

                anim_set_color(s, logo, ANIM_WHITE, true);
                anim_set_glow(s, logo, 0.7f, true);
                anim_move_to(s, logo, ANIM_V3(0.0f, -2.0f, 0.0f));
                br = anim_brace(s, logo, ANIM_DOWN, 0.20f);
                anim_set_stroke(s, br, ANIM_GOLD, 3.5f, false);
                anim_set_glow(s, br, 0.8f, false);

                {
                    AnimSpec to_act4[8] = {
                        anim_anim_camera_to(cam_home),
                        anim_anim_fade_out(ax3d, ANIM_ORIGIN, 0.85f),
                        anim_anim_fade_out(surf, ANIM_ORIGIN, 0.85f),
                        anim_anim_fade_out(helix, ANIM_ORIGIN, 0.85f),
                        anim_anim_fade_out(tracer, ANIM_ORIGIN, 0.85f),
                        anim_anim_fade_out(cap3d, ANIM_UP, 0.9f),
                        anim_anim_create(gnet),
                        anim_anim_create(ring_w),
                    };
                    anim_play(tl, s, to_act4, 8u, 1.35f, ANIM_RATE_SMOOTH);
                }
                {
                    AnimSpec finale[4] = {
                        anim_anim_show_passing_flash(ring_w, 0.30f),
                        anim_anim_write(logo),
                        anim_anim_create(br),
                        anim_anim_indicate(logo, 1.06f, ANIM_GOLD),
                    };
                    anim_play(tl, s, finale, 4u, 1.45f, ANIM_RATE_SPRING);
                }
                anim_wait(tl, s, 0.6f);
            }
        }
    }
}

int main(int argc, char **argv)
{
    uint32_t       w          = 1280u;
    uint32_t       h          = 720u;
    uint32_t       fps        = 30u;
    uint32_t       blur_samp  = 1u;
    bool           use_preset = false;
    AnimResolutionPreset preset = ANIM_RES_720P;
    bool           validate   = false;
    bool           print_stat = false;
    bool           alpha_bg   = false;
    const char    *shot_path  = NULL;
    const char    *video_path = NULL;
    float          shot_time  = -1.0f;
    int            i;

    Platform      *platform   = NULL;
    Gpu           *gpu        = NULL;
    AnimRenderer  *ren        = NULL;
    AnimScene     *scene      = NULL;
    AnimTimeline  *tl         = NULL;
    AnimRenderOpts opts;
    DemoUpdaterCtx ctx        = { 0 };
    int            rc         = 0;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)        shot_path  = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) video_path = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc)  shot_time  = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) w          = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) h         = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)   fps        = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)  blur_samp  = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) print_stat = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--alpha") == 0 && i + 1 < argc) alpha_bg   = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc)
        {
            const char *p = argv[++i];
            use_preset = true;
            if (strcmp(p, "1080p") == 0) preset = ANIM_RES_1080P;
            else if (strcmp(p, "4k") == 0) preset = ANIM_RES_4K;
            else if (strcmp(p, "vertical") == 0) preset = ANIM_RES_VERTICAL_1080X1920;
            else if (strcmp(p, "square") == 0) preset = ANIM_RES_SQUARE_1080;
            else preset = ANIM_RES_720P;
        }
    }

    if (use_preset)
    {
        opts = anim_render_opts_preset(preset);
        w    = opts.width;
        h    = opts.height;
        fps  = opts.fps;
    }
    else
    {
        opts      = anim_render_opts_default(w, h);
        opts.fps  = fps;
    }
    opts.motion_blur_samples = MU_MAX(blur_samp, 1u);
    opts.transparent_bg      = alpha_bg;

    scene = anim_scene_create();
    tl    = anim_timeline_create();
    build_demo_scene(scene, tl, &ctx);
    log_info("built demo timeline: duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim — Beyond-Manim C11/Vulkan Showcase",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });

    gpu = gpu_create(&(GpuDesc){
        .name                     = "mu_anim",
        .width                    = w,
        .height                   = h,
        .enable_validation        = validate,
        .enable_graphics_profiler = print_stat,
        .size_of_cpu_pool         = MU_MB(32),
        .size_of_gpu_pool         = MU_MB(32),
        .size_of_staging_pool     = MU_MB(48),
        .vsync                    = true,
    });

    if (!shot_path && !video_path)
        gpu_set_surface(gpu, platform_create_surface(platform, gpu_raw_instance(gpu)));

    ren = anim_renderer_create(gpu, w, h);

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path))
            rc = 1;
        else
            log_info("wrote %s at t=%.2fs", shot_path, (double)t);
    }
    else if (video_path)
    {
        const char *wav_path = "/tmp/mu_anim_demo_audio.wav";
        write_ambient_wav(wav_path, anim_timeline_duration(tl));
        if (!anim_export_video(ren, tl, scene, &opts, video_path, wav_path))
            rc = 1;
        else
            log_info("exported %s (%ux%u @ %u fps, blur=%u, %.2f s)", video_path, w, h, fps, opts.motion_blur_samples, (double)anim_timeline_duration(tl));
        remove(wav_path);
    }
    else
    {
        anim_run_interactive(platform, gpu, ren, tl, scene, opts);
    }

    gpu_wait_idle(gpu);
    if (print_stat)
        gpu_dump_stats(gpu, NULL);
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
