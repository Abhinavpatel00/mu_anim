/*
 * examples/graphics_pipeline_explainer.c
 *
 * A visual explainer of the Real-Time Graphics Pipeline — from 3D vertices
 * and perspective frustums to rasterization, Cook-Torrance GGX PBR shading,
 * and HDR Bloom + AgX tone mapping — built entirely with mu_anim.
 *
 * Usage:
 *   ./graphics_pipeline_demo                         Interactive preview
 *   ./graphics_pipeline_demo --shot f.png --time 6.5 Headless PNG shot
 *   ./graphics_pipeline_demo --video pipeline.mp4    Headless MP4 export
 */
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "../third_party/mugfx/desktop/platform.h"
#include "../third_party/mugfx/desktop/vk.h"
#include "../anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float linear_clamp_fn(float x, void *user)
{
    (void)user;
    return x < 1.0f ? (x > 0.0f ? x : 0.0f) : 1.0f;
}

static float agx_curve_fn(float x, void *user)
{
    float a = x * 0.95f;
    (void)user;
    return 1.0f - expf(-1.25f * powf(a > 0.0f ? a : 0.0f, 1.08f));
}

static float bloom_ring_fn(float x, float y, void *user)
{
    (void)user;
    return sqrtf(x * x + y * y);
}

static float edge2d(AnimVec3 a, AnimVec3 b, float px, float py)
{
    return (px - a.x) * (b.y - a.y) - (py - a.y) * (b.x - a.x);
}

static void build_pipeline_scene(AnimScene *s, AnimTimeline *tl)
{
    /* ================================================================ */
    /* ACT 1 (~0.0s - 4.7s): Overview — "The Real-Time Graphics Pipeline" */
    /* ================================================================ */
    AnimId title    = anim_text(s, NULL, "The Real-Time Graphics Pipeline", 0.54f);
    AnimId subtitle = anim_text(s, NULL, "From 3D Vertices & Beziers to Final Pixels", 0.28f);
    AnimId banner   = anim_text(s, NULL, "Vulkan + Slang: Bezier Bands + Cook-Torrance PBR + HDR AgX", 0.21f);
    static const char *stage_names[5] = {
        "1. Vertices", "2. Clip/MVP", "3. Rasterize", "4. PBR Shade", "5. Post/AgX"
    };
    const AnimColor stage_cols[5] = { ANIM_BLUE, ANIM_TEAL, ANIM_GOLD, ANIM_MAROON, ANIM_PINK };
    AnimId cards[5], arrows[4], act1_all[16];
    AnimId pulse = anim_dot(s, ANIM_V3(-4.6f, 0.95f, 0.0f), 0.11f);
    AnimId ppath = anim_line(s, ANIM_V3(-4.6f, 0.95f, 0.0f), ANIM_V3(4.6f, 0.95f, 0.0f));
    uint32_t i, ak = 0u;

    anim_move_to(s, title, ANIM_V3(0.0f, 2.45f, 0.0f));
    anim_set_fill(s, title, ANIM_WHITE, false);
    anim_set_glow(s, title, 0.35f, false);
    anim_move_to(s, subtitle, ANIM_V3(0.0f, 1.76f, 0.0f));
    anim_set_fill(s, subtitle, ANIM_TEAL, false);
    anim_move_to(s, banner, ANIM_V3(0.0f, -1.85f, 0.0f));
    anim_set_fill(s, banner, ANIM_GREY_B, false);

    anim_add(s, title);
    anim_add(s, subtitle);
    {
        AnimSpec s_in[2] = {
            anim_anim_write(title),
            anim_anim_fade_in(subtitle, ANIM_V3(0.0f, 0.2f, 0.0f), 0.98f)
        };
        anim_play(tl, s, s_in, 2u, 0.9f, ANIM_RATE_SMOOTH);
    }

    for (i = 0u; i < 5u; ++i)
    {
        float  cx  = -4.6f + (float)i * 2.3f;
        AnimId box = anim_rounded_rect(s, 1.85f, 1.15f, 0.16f);
        AnimId lbl = anim_text(s, NULL, stage_names[i], 0.22f);
        AnimId grp;
        anim_move_to(s, box, ANIM_V3(cx, 0.05f, 0.0f));
        anim_set_fill(s, box, anim_rgba(stage_cols[i].r * 0.25f, stage_cols[i].g * 0.25f, stage_cols[i].b * 0.25f, 0.85f), false);
        anim_set_stroke(s, box, stage_cols[i], 2.6f, false);
        anim_move_to(s, lbl, ANIM_V3(cx, 0.05f, 0.0f));
        anim_set_fill(s, lbl, ANIM_WHITE, false);
        {
            AnimId k2[2] = { box, lbl };
            grp = anim_group(s, k2, 2u);
        }
        cards[i] = grp;
        anim_add(s, grp);
        act1_all[ak++] = grp;

        if (i < 4u)
        {
            arrows[i] = anim_arrow(s, ANIM_V3(cx + 0.96f, 0.05f, 0.0f), ANIM_V3(cx + 1.34f, 0.05f, 0.0f), 0.14f, 0.11f);
            anim_set_fill(s, arrows[i], ANIM_GREY_A, false);
            anim_set_stroke(s, arrows[i], ANIM_GREY_A, 2.0f, false);
            anim_add(s, arrows[i]);
            act1_all[ak++] = arrows[i];
        }
    }
    anim_add(s, banner);
    act1_all[ak++] = title;
    act1_all[ak++] = subtitle;
    act1_all[ak++] = banner;

    {
        AnimSpec lag[6];
        for (i = 0u; i < 5u; ++i)
            lag[i] = anim_anim_grow_from_center(cards[i]);
        lag[5] = anim_anim_fade_in(banner, ANIM_V3(0.0f, -0.2f, 0.0f), 1.0f);
        anim_play_lagged(tl, s, lag, 6u, 1.35f, 0.18f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec ar[4];
        for (i = 0u; i < 4u; ++i)
            ar[i] = anim_anim_create(arrows[i]);
        anim_play(tl, s, ar, 4u, 0.45f, ANIM_RATE_SMOOTH);
    }

    anim_set_fill(s, pulse, ANIM_YELLOW, false);
    anim_set_glow(s, pulse, 1.0f, false);
    anim_add(s, pulse);
    act1_all[ak++] = pulse;
    {
        AnimSpec trav[2] = {
            anim_anim_follow_path(pulse, ppath, false),
            anim_anim_circumscribe(cards[2], ANIM_YELLOW, 0.12f)
        };
        anim_play(tl, s, trav, 2u, 1.25f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.30f);
    {
        AnimSpec fout[16];
        for (i = 0u; i < ak; ++i)
            fout[i] = anim_anim_fade_out(act1_all[i], ANIM_ORIGIN, 0.96f);
        anim_play(tl, s, fout, ak, 0.45f, ANIM_RATE_SMOOTH);
    }
    for (i = 0u; i < ak; ++i)
        anim_remove(s, act1_all[i]);
    log_info("Act 1 ends at %.2fs", (double)anim_timeline_duration(tl));

    /* ================================================================ */
    /* ACT 2 (~4.7s - 9.9s): 3D Space, MVP Matrix & Perspective Frustum */
    /* ================================================================ */
    {
        AnimId h2      = anim_text(s, NULL, "Stage 1-2: 3D Space, MVP & Perspective Frustum", 0.40f);
        AnimVec3 eye   = ANIM_V3(-4.5f, 0.15f, 0.85f);
        AnimVec3 look  = ANIM_V3(-0.3f, -0.05f, -0.35f);
        AnimId frust   = anim_frustum3d(s, 46.0f, 1.38f, 1.15f, 4.1f, eye, look);
        AnimId ico     = anim_polyhedron(s, ANIM_POLY_ICOSAHEDRON, 0.68f);
        AnimId torus   = anim_torus(s, 0.72f, 0.25f, 36u, 18u);
        AnimId l_eye   = anim_text(s, NULL, "Eye", 0.23f);
        AnimId l_near  = anim_text(s, NULL, "Near", 0.23f);
        AnimId l_far   = anim_text(s, NULL, "Far", 0.23f);
        AnimId eq_bg   = anim_rounded_rect(s, 4.7f, 3.8f, 0.18f);
        AnimId eq_hdr  = anim_text(s, NULL, "Clip & NDC Projection", 0.28f);
        AnimId eq1     = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{v}_{\\text{clip}} = P\\,V\\,M\\,\\mathbf{v}", 0.56f);
        AnimId eq2     = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{v}_{\\text{ndc}} = \\frac{\\mathbf{v}_{\\text{clip}}}{w}", 0.68f);
        AnimId ndc_cap = anim_text(s, NULL, "Keep vertices inside [-1, 1]^3", 0.22f);

        anim_move_to(s, h2, ANIM_V3(0.0f, 2.95f, 0.0f));
        anim_set_fill(s, h2, ANIM_WHITE, false);

        anim_move_to(s, ico, ANIM_V3(-1.85f, 0.25f, -0.15f));
        anim_set_fill(s, ico, ANIM_BLUE, false);
        anim_surface_set_roughness(s, ico, 0.24f, true);
        anim_surface_set_wireframe(s, ico, 8.0f, 0.35f, true);

        anim_move_to(s, torus, ANIM_V3(-0.35f, -0.35f, -0.55f));
        anim_rotate(s, torus, 0.65f, ANIM_V3(1.0f, 0.4f, 0.0f));
        anim_set_fill(s, torus, ANIM_GOLD, false);
        anim_surface_set_roughness(s, torus, 0.18f, true);
        anim_surface_set_wireframe(s, torus, 12.0f, 0.32f, true);

        anim_move_to(s, l_eye, ANIM_V3(-4.5f, 0.58f, 0.85f));
        anim_set_fill(s, l_eye, ANIM_WHITE, false);
        anim_set_billboard(s, l_eye, true, true);

        anim_move_to(s, l_near, ANIM_V3(-3.3f, 0.92f, 0.45f));
        anim_set_fill(s, l_near, ANIM_TEAL, false);
        anim_set_billboard(s, l_near, true, true);

        anim_move_to(s, l_far, ANIM_V3(-0.55f, 1.95f, -0.25f));
        anim_set_fill(s, l_far, ANIM_GOLD, false);
        anim_set_billboard(s, l_far, true, true);

        anim_move_to(s, eq_bg, ANIM_V3(3.65f, -0.15f, -0.08f));
        anim_set_fill(s, eq_bg, anim_rgba(0.08f, 0.11f, 0.18f, 0.88f), false);
        anim_set_stroke(s, eq_bg, ANIM_BLUE_D, 2.0f, false);
        anim_move_to(s, eq_hdr, ANIM_V3(3.65f, 1.28f, 0.0f));
        anim_set_fill(s, eq_hdr, ANIM_TEAL, false);
        anim_move_to(s, eq1, ANIM_V3(3.65f, 0.35f, 0.0f));
        anim_set_fill(s, eq1, ANIM_WHITE, true);
        anim_move_to(s, eq2, ANIM_V3(3.65f, 0.35f, 0.0f));
        anim_set_fill(s, eq2, ANIM_YELLOW, true);
        anim_move_to(s, ndc_cap, ANIM_V3(3.65f, -1.15f, 0.0f));
        anim_set_fill(s, ndc_cap, ANIM_GREY_B, false);

        anim_add(s, h2);
        anim_add(s, frust);
        anim_add(s, ico);
        anim_add(s, torus);
        anim_add(s, eq_bg);

        {
            AnimCamera cam3d = anim_camera_default();
            cam3d.pos    = ANIM_V3(0.4f, 1.7f, 9.6f);
            cam3d.target = ANIM_V3(0.0f, 0.0f, 0.0f);
            {
                AnimSpec s_enter[6] = {
                    anim_anim_fade_in(h2, ANIM_ORIGIN, 1.0f),
                    anim_anim_create(frust),
                    anim_anim_fade_in(ico, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_in(torus, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_in(eq_bg, ANIM_ORIGIN, 0.98f),
                    anim_anim_camera_to(cam3d)
                };
                anim_play(tl, s, s_enter, 6u, 1.05f, ANIM_RATE_SMOOTH);
            }
        }

        anim_add(s, eq_hdr);
        anim_add(s, eq1);
        anim_add(s, l_eye);
        anim_add(s, l_near);
        anim_add(s, l_far);
        {
            AnimSpec s_eq[5] = {
                anim_anim_write(eq1),
                anim_anim_fade_in(eq_hdr, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(l_eye, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(l_near, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(l_far, ANIM_ORIGIN, 1.0f)
            };
            anim_play(tl, s, s_eq, 5u, 0.85f, ANIM_RATE_SMOOTH);
        }

        anim_add(s, ndc_cap);
        {
            AnimCamera cam_orb = anim_camera_default();
            cam_orb.pos    = ANIM_V3(-2.1f, 2.25f, 9.1f);
            cam_orb.target = ANIM_V3(-0.3f, 0.0f, -0.2f);
            {
                AnimSpec s_orb[5] = {
                    anim_anim_camera_to(cam_orb),
                    anim_anim_rotate(ico, 1.5f, ANIM_V3(0.2f, 1.0f, 0.1f)),
                    anim_anim_rotate(torus, 1.4f, ANIM_V3(0.0f, 1.0f, 0.3f)),
                    anim_anim_replacement_transform(eq1, eq2),
                    anim_anim_fade_in(ndc_cap, ANIM_ORIGIN, 1.0f)
                };
                anim_play(tl, s, s_orb, 5u, 1.75f, ANIM_RATE_SMOOTH);
            }
        }
        anim_wait(tl, s, 0.75f);
        {
            AnimCamera cam_2d = anim_camera_default();
            AnimSpec s_out[11] = {
                anim_anim_fade_out(h2, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(frust, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(ico, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(torus, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(l_eye, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(l_near, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(l_far, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(eq_bg, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(eq_hdr, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(eq2, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(ndc_cap, ANIM_ORIGIN, 1.0f)
            };
            anim_play(tl, s, s_out, 11u, 0.55f, ANIM_RATE_SMOOTH);
            {
                AnimSpec c_rst = anim_anim_camera_to(cam_2d);
                anim_play(tl, s, &c_rst, 1u, 0.25f, ANIM_RATE_SMOOTH);
            }
        }
        anim_remove(s, h2); anim_remove(s, frust); anim_remove(s, ico); anim_remove(s, torus);
        anim_remove(s, l_eye); anim_remove(s, l_near); anim_remove(s, l_far);
        anim_remove(s, eq_bg); anim_remove(s, eq_hdr); anim_remove(s, eq2); anim_remove(s, ndc_cap);
        log_info("Act 2 ends at %.2fs", (double)anim_timeline_duration(tl));
    }

    /* ================================================================ */
    /* ACT 3 (~9.9s - 15.0s): Rasterization, Pixel Grid & Bezier Bands  */
    /* ================================================================ */
    {
        AnimId   h3     = anim_text(s, NULL, "Stage 3: Rasterization & GPU Bezier Bands", 0.40f);
        AnimId   cells[63];
        AnimId   pgrid  = anim_pixel_grid(s, 9u, 7u, 0.44f, true, cells);
        AnimVec3 tri[3] = {
            ANIM_V3(-4.55f, -1.15f, 0.0f),
            ANIM_V3(-1.35f, -0.85f, 0.0f),
            ANIM_V3(-2.85f,  1.25f, 0.0f)
        };
        AnimId   tri_m  = anim_polygon(s, tri, 3u);
        AnimId   r_bg   = anim_rounded_rect(s, 4.9f, 2.95f, 0.18f);
        AnimId   r_hdr  = anim_text(s, NULL, "Edge Winding + mu_anim Bands", 0.25f);
        AnimId   r_eq   = anim_tex(s, ANIM_TEX_LATEX, "E_i(x,y) > 0 \\;\\Rightarrow\\; \\text{Fill Pixel}", 0.34f);
        AnimId   b_lbl  = anim_text(s, NULL, "Quadratic Bezier Sliced into GPU Bands:", 0.19f);
        AnimId   b_arc  = anim_arc(s, 0.92f, 0.25f, 2.60f);
        AnimId   b_l0   = anim_dashed_line(s, ANIM_V3(1.5f, -1.25f, 0.0f), ANIM_V3(4.8f, -1.25f, 0.0f), 0.16f, 0.10f);
        AnimId   b_l1   = anim_dashed_line(s, ANIM_V3(1.5f, -1.60f, 0.0f), ANIM_V3(4.8f, -1.60f, 0.0f), 0.16f, 0.10f);
        AnimId   b_l2   = anim_dashed_line(s, ANIM_V3(1.5f, -1.95f, 0.0f), ANIM_V3(4.8f, -1.95f, 0.0f), 0.16f, 0.10f);
        uint32_t r, c;

        anim_move_to(s, h3, ANIM_V3(0.0f, 2.95f, 0.0f));
        anim_set_fill(s, h3, ANIM_WHITE, false);
        anim_move_to(s, pgrid, ANIM_V3(-3.0f, -0.15f, 0.0f));

        for (r = 0u; r < 7u; ++r)
        {
            for (c = 0u; c < 9u; ++c)
            {
                float cx = -3.0f + (-0.5f * 9.0f + (float)c + 0.5f) * 0.44f;
                float cy = -0.15f + (0.5f * 7.0f - (float)r - 0.5f) * 0.44f;
                float e0 = edge2d(tri[0], tri[1], cx, cy);
                float e1 = edge2d(tri[1], tri[2], cx, cy);
                float e2 = edge2d(tri[2], tri[0], cx, cy);
                if (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f)
                {
                    anim_set_fill(s, cells[r * 9u + c], anim_rgba(0.18f, 0.78f, 0.85f, 0.78f), false);
                    anim_set_stroke(s, cells[r * 9u + c], ANIM_TEAL, 2.0f, false);
                }
            }
        }

        anim_set_fill(s, tri_m, anim_rgba(0.95f, 0.75f, 0.25f, 0.20f), false);
        anim_set_stroke(s, tri_m, ANIM_YELLOW, 3.2f, false);

        anim_move_to(s, r_bg, ANIM_V3(3.15f, -1.05f, -0.08f));
        anim_set_fill(s, r_bg, anim_rgba(0.08f, 0.11f, 0.18f, 0.88f), false);
        anim_set_stroke(s, r_bg, ANIM_GOLD, 2.0f, false);
        anim_move_to(s, r_hdr, ANIM_V3(3.15f, 0.12f, 0.0f));
        anim_set_fill(s, r_hdr, ANIM_GOLD, false);
        anim_move_to(s, r_eq, ANIM_V3(3.15f, -0.44f, 0.0f));
        anim_set_fill(s, r_eq, ANIM_WHITE, true);
        anim_move_to(s, b_lbl, ANIM_V3(3.15f, -0.96f, 0.0f));
        anim_set_fill(s, b_lbl, ANIM_GREY_B, false);
        anim_move_to(s, b_arc, ANIM_V3(3.15f, -2.05f, 0.0f));
        anim_set_stroke(s, b_arc, ANIM_PINK, 4.0f, false);
        anim_set_glow(s, b_arc, 0.7f, false);
        anim_set_stroke(s, b_l0, ANIM_TEAL, 1.6f, false);
        anim_set_stroke(s, b_l1, ANIM_TEAL, 1.6f, false);
        anim_set_stroke(s, b_l2, ANIM_TEAL, 1.6f, false);

        anim_add(s, h3);
        anim_add(s, pgrid);
        anim_add(s, tri_m);
        anim_add(s, r_bg);
        anim_add(s, r_eq);
        {
            AnimSpec s_in[5] = {
                anim_anim_fade_in(h3, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(pgrid, ANIM_ORIGIN, 0.96f),
                anim_anim_create(tri_m),
                anim_anim_fade_in(r_bg, ANIM_ORIGIN, 0.98f),
                anim_anim_write(r_eq)
            };
            anim_play(tl, s, s_in, 5u, 1.15f, ANIM_RATE_SMOOTH);
        }

        anim_add(s, r_hdr);
        anim_add(s, b_lbl);
        anim_add(s, b_arc);
        anim_add(s, b_l0);
        anim_add(s, b_l1);
        anim_add(s, b_l2);
        {
            AnimSpec s_b[6] = {
                anim_anim_fade_in(r_hdr, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(b_lbl, ANIM_ORIGIN, 1.0f),
                anim_anim_create(b_arc),
                anim_anim_create(b_l0),
                anim_anim_create(b_l1),
                anim_anim_create(b_l2)
            };
            anim_play(tl, s, s_b, 6u, 0.9f, ANIM_RATE_SMOOTH);
        }
        {
            AnimCamera zc = anim_camera_default();
            const float pip_rect[4] = { 0.60f, 0.17f, 0.94f, 0.44f };
            zc.pos    = ANIM_V3(-2.65f, 0.25f, 4.0f);
            zc.target = ANIM_V3(-2.65f, 0.25f, 0.0f);
            zc.zoom   = 1.55f;
            {
                AnimSpec s_pip[2] = {
                    anim_anim_zoom_inset_to(true, zc, pip_rect),
                    anim_anim_flash(ANIM_V3(-2.85f, 1.25f, 0.0f), ANIM_YELLOW, 0.32f, 10u, 0.12f)
                };
                anim_play(tl, s, s_pip, 2u, 1.45f, ANIM_RATE_SMOOTH);
            }
        }
        anim_wait(tl, s, 0.85f);
        {
            AnimCamera z_off = anim_camera_default();
            AnimSpec s_out[12] = {
                anim_anim_zoom_inset_to(false, z_off, NULL),
                anim_anim_fade_out(h3, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(pgrid, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(tri_m, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(r_bg, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(r_hdr, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(r_eq, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(b_lbl, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(b_arc, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(b_l0, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(b_l1, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(b_l2, ANIM_ORIGIN, 1.0f)
            };
            anim_play(tl, s, s_out, 12u, 0.55f, ANIM_RATE_SMOOTH);
        }
        anim_remove(s, h3); anim_remove(s, pgrid); anim_remove(s, tri_m);
        anim_remove(s, r_bg); anim_remove(s, r_hdr); anim_remove(s, r_eq);
        anim_remove(s, b_lbl); anim_remove(s, b_arc);
        anim_remove(s, b_l0); anim_remove(s, b_l1); anim_remove(s, b_l2);
        log_info("Act 3 ends at %.2fs", (double)anim_timeline_duration(tl));
    }

    /* ================================================================ */
    /* ACT 4 (~15.0s - 19.8s): Cook-Torrance GGX PBR Shading            */
    /* ================================================================ */
    {
        static const char *pbr_bullets[3] = {
            "D: GGX microfacet distribution",
            "G: Smith self-shadowing mask",
            "F: Schlick Fresnel grazing glint"
        };
        AnimId h4     = anim_text(s, NULL, "Stage 4: Cook-Torrance GGX PBR Shading", 0.40f);
        AnimId sph    = anim_sphere(s, 0.95f, 40u, 22u);
        AnimId dod    = anim_polyhedron(s, ANIM_POLY_DODECAHEDRON, 0.72f);
        AnimId orb    = anim_circle(s, 1.85f);
        AnimId ldot   = anim_dot(s, ANIM_V3(-0.95f, 0.15f, 0.0f), 0.14f);
        AnimId trail  = anim_traced_path(s, ldot, ANIM_YELLOW, 2.5f, 160u);
        AnimId p_bg   = anim_rounded_rect(s, 5.2f, 4.1f, 0.18f);
        AnimId p_eq   = anim_tex(s, ANIM_TEX_LATEX, "f_{\\text{spec}} = \\frac{D\\,G\\,F}{4\\,(n\\cdot l)(n\\cdot v)}", 0.62f);
        AnimId p_list = anim_bulleted_list(s, NULL, pbr_bullets, 3u, 0.23f, 0.42f);

        anim_move_to(s, h4, ANIM_V3(0.0f, 2.95f, 0.0f));
        anim_set_fill(s, h4, ANIM_WHITE, false);

        anim_move_to(s, sph, ANIM_V3(-2.85f, 0.15f, 0.0f));
        anim_set_fill(s, sph, ANIM_TEAL, false);
        anim_surface_set_roughness(s, sph, 0.14f, true);
        anim_surface_set_wireframe(s, sph, 16.0f, 0.25f, true);

        anim_move_to(s, dod, ANIM_V3(-1.35f, -1.05f, 0.45f));
        anim_set_fill(s, dod, ANIM_GOLD, false);
        anim_surface_set_roughness(s, dod, 0.22f, true);
        anim_surface_set_wireframe(s, dod, 8.0f, 0.28f, true);

        anim_move_to(s, orb, ANIM_V3(-2.85f, 0.15f, 0.0f));
        anim_set_fill(s, ldot, ANIM_YELLOW, false);
        anim_set_glow(s, ldot, 1.0f, false);

        anim_move_to(s, p_bg, ANIM_V3(3.15f, -0.25f, -0.15f));
        anim_set_fill(s, p_bg, anim_rgba(0.08f, 0.11f, 0.18f, 0.88f), false);
        anim_set_stroke(s, p_bg, ANIM_GOLD, 2.0f, false);
        anim_move_to(s, p_eq, ANIM_V3(3.15f, 1.02f, 0.06f));
        anim_set_fill(s, p_eq, ANIM_WHITE, true);
        anim_move_to(s, p_list, ANIM_V3(3.05f, -0.65f, 0.06f));
        anim_set_fill(s, p_list, ANIM_WHITE, true);

        anim_add(s, h4);
        anim_add(s, sph);
        anim_add(s, dod);
        anim_add(s, p_bg);
        anim_add(s, p_eq);
        {
            AnimCamera cam_pbr = anim_camera_default();
            cam_pbr.pos    = ANIM_V3(0.4f, 1.25f, 9.4f);
            cam_pbr.target = ANIM_V3(0.0f, 0.0f, 0.0f);
            {
                AnimSpec s_in[6] = {
                    anim_anim_fade_in(h4, ANIM_ORIGIN, 1.0f),
                    anim_anim_fade_in(sph, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_in(dod, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_in(p_bg, ANIM_ORIGIN, 0.98f),
                    anim_anim_write(p_eq),
                    anim_anim_camera_to(cam_pbr)
                };
                anim_play(tl, s, s_in, 6u, 1.15f, ANIM_RATE_SMOOTH);
            }
        }

        anim_add(s, ldot);
        anim_add(s, trail);
        anim_add(s, p_list);
        {
            AnimCamera cam_pbr2 = anim_camera_default();
            cam_pbr2.pos    = ANIM_V3(-1.1f, 1.65f, 8.9f);
            cam_pbr2.target = ANIM_V3(-0.2f, -0.1f, 0.0f);
            {
                AnimSpec s_run[5] = {
                    anim_anim_type_with_cursor(p_list, ANIM_YELLOW),
                    anim_anim_follow_path(ldot, orb, false),
                    anim_anim_rotate(sph, 1.8f, ANIM_V3(0.15f, 1.0f, 0.0f)),
                    anim_anim_rotate(dod, 2.1f, ANIM_V3(0.3f, 1.0f, 0.2f)),
                    anim_anim_camera_to(cam_pbr2)
                };
                anim_play(tl, s, s_run, 5u, 2.40f, ANIM_RATE_SMOOTH);
            }
        }
        anim_wait(tl, s, 0.85f);
        {
            AnimCamera cam_rst = anim_camera_default();
            AnimSpec s_out[9] = {
                anim_anim_fade_out(h4, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(sph, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(dod, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(ldot, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(trail, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(p_bg, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(p_eq, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(p_list, ANIM_ORIGIN, 1.0f),
                anim_anim_camera_to(cam_rst)
            };
            anim_play(tl, s, s_out, 9u, 0.55f, ANIM_RATE_SMOOTH);
        }
        anim_remove(s, h4); anim_remove(s, sph); anim_remove(s, dod);
        anim_remove(s, ldot); anim_remove(s, trail);
        anim_remove(s, p_bg); anim_remove(s, p_eq); anim_remove(s, p_list);
        log_info("Act 4 ends at %.2fs", (double)anim_timeline_duration(tl));
    }

    /* ================================================================ */
    /* ACT 5 (~19.8s - 24.5s): HDR Bloom, Particles & AgX Tone Mapping  */
    /* ================================================================ */
    {
        AnimId h5     = anim_text(s, NULL, "Stage 5: HDR Bloom, Chromatic FX & AgX", 0.40f);
        AnimAxesDesc axd = {
            .x_min = 0.0f, .x_max = 5.0f, .x_step = 1.0f,
            .y_min = 0.0f, .y_max = 1.2f, .y_step = 0.4f,
            .x_length = 4.4f, .y_length = 3.0f,
            .include_ticks = true, .include_tips = true,
            .include_grid = true, .include_numbers = true
        };
        AnimId axes   = anim_axes(s, &axd);
        AnimId c_lin, c_agx;
        AnimId lbl_l  = anim_text(s, NULL, "Linear Clamp (clips)", 0.21f);
        AnimId lbl_a  = anim_text(s, NULL, "AgX Filmic S-Curve", 0.22f);
        float  lvs[4] = { 0.45f, 0.85f, 1.25f, 1.65f };
        AnimId cplot  = anim_contour_plot(s, ANIM_ID_NIL, bloom_ring_fn, -1.8f, 1.8f, -1.8f, 1.8f, 28u, 28u, lvs, 4u, ANIM_PINK, ANIM_YELLOW, NULL);
        AnimId psys   = anim_particle_system(s, 36u, ANIM_V3(3.05f, -0.35f, 0.0f),
                                             ANIM_V3(-1.15f, 0.45f, 0.0f), ANIM_V3(1.15f, 1.85f, 0.0f),
                                             ANIM_V3(0.0f, -1.35f, 0.0f), 1.35f, ANIM_YELLOW, ANIM_PINK, 0.07f);
        AnimId badge  = anim_text(s, NULL, "HDR -> Bloom -> AgX -> sRGB", 0.28f);

        anim_move_to(s, axes, ANIM_V3(-2.75f, -0.35f, 0.0f));
        /* Build plots AFTER positioning axes so c2p uses positioned axes! */
        c_lin = anim_plot(s, axes, linear_clamp_fn, 0.0f, 5.0f, 96u, NULL);
        c_agx = anim_plot(s, axes, agx_curve_fn, 0.0f, 5.0f, 96u, NULL);

        anim_move_to(s, h5, ANIM_V3(0.0f, 2.95f, 0.0f));
        anim_set_fill(s, h5, ANIM_WHITE, false);
        anim_set_stroke(s, c_lin, ANIM_RED, 2.6f, false);
        anim_set_stroke(s, c_agx, ANIM_GOLD, 3.6f, false);
        anim_set_glow(s, c_agx, 0.65f, false);

        anim_move_to(s, lbl_l, ANIM_V3(-2.15f, 1.05f, 0.0f));
        anim_set_fill(s, lbl_l, ANIM_RED, false);
        anim_move_to(s, lbl_a, ANIM_V3(-2.10f, 0.28f, 0.0f));
        anim_set_fill(s, lbl_a, ANIM_GOLD, false);

        anim_move_to(s, cplot, ANIM_V3(3.05f, -0.35f, 0.0f));
        anim_move_to(s, badge, ANIM_V3(3.05f, 1.85f, 0.0f));
        anim_set_fill(s, badge, ANIM_YELLOW, false);
        anim_set_glow(s, badge, 0.95f, false);

        anim_add(s, h5);
        anim_add(s, axes);
        anim_add(s, c_lin);
        anim_add(s, c_agx);
        anim_add(s, lbl_l);
        anim_add(s, lbl_a);
        anim_add(s, cplot);
        anim_add(s, psys);
        anim_add(s, badge);

        {
            AnimSpec s_in[9] = {
                anim_anim_fade_in(h5, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(axes, ANIM_ORIGIN, 0.98f),
                anim_anim_create(c_lin),
                anim_anim_create(c_agx),
                anim_anim_fade_in(lbl_l, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(lbl_a, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_in(cplot, ANIM_ORIGIN, 0.95f),
                anim_anim_fade_in(psys, ANIM_ORIGIN, 1.0f),
                anim_anim_write(badge)
            };
            anim_play(tl, s, s_in, 9u, 1.45f, ANIM_RATE_SMOOTH);
        }
        anim_wait(tl, s, 2.40f);
        {
            AnimSpec s_end[9] = {
                anim_anim_fade_out(h5, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(axes, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(c_lin, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(c_agx, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(lbl_l, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(lbl_a, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(cplot, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(psys, ANIM_ORIGIN, 1.0f),
                anim_anim_fade_out(badge, ANIM_ORIGIN, 1.0f)
            };
            anim_play(tl, s, s_end, 9u, 0.85f, ANIM_RATE_SMOOTH);
        }
        log_info("Act 5 ends at %.2fs", (double)anim_timeline_duration(tl));
    }
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

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)        shot_path  = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) video_path = argv[++i];
        else if ((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "--t") == 0) && i + 1 < argc)
            shot_time = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)   fps        = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)  blur       = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate = atoi(argv[++i]) != 0;
    }

    scene = anim_scene_create();
    anim_scene_set_bg(scene, anim_hex(0x0D111A), anim_hex(0x161F33));
    tl = anim_timeline_create();
    build_pipeline_scene(scene, tl);
    log_info("graphics_pipeline_explainer duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim — Real-Time Graphics Pipeline",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "graphics_pipeline_demo",
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

    ren = anim_renderer_create(gpu, w, h);
    opts = anim_render_opts_default(w, h);
    opts.fps = fps;
    opts.motion_blur_samples = MU_MAX(blur, 1u);
    opts.tonemap       = ANIM_TONEMAP_AGX;
    opts.aberration_px = 0.75f;
    opts.grain_amount  = 0.0018f;

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path))
            rc = 1;
    }
    else if (video_path)
    {
        if (!anim_export_video(ren, tl, scene, &opts, video_path, NULL))
            rc = 1;
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
