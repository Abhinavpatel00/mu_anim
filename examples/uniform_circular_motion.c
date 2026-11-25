/*
 * examples/uniform_circular_motion.c
 *
 * Derivation of Uniform Circular Motion from position r(t) to tangent
 * velocity v(t) and inward centripetal acceleration a(t), finishing with
 * highlights of v = omega * R and a_c = v^2 / R.
 *
 * Usage:
 *   ./uniform_circular_motion_demo                         Interactive preview
 *   ./uniform_circular_motion_demo --shot out.png --time 8 Headless PNG shot
 *   ./uniform_circular_motion_demo --video ucm.mp4         Headless MP4 export
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

static void set_arrow_2d(AnimScene *s, AnimId arrow_id, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width)
{
    AnimId   stem = anim_child_at(s, arrow_id, 0u);
    AnimId   tip  = anim_child_at(s, arrow_id, 1u);
    AnimVec3 d    = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len  = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    AnimVec3 dir  = (len > 1e-5f) ? ANIM_V3(d.x / len, d.y / len, d.z / len) : ANIM_RIGHT;
    float    tl   = (tip_len > 0.0f) ? tip_len : MU_MIN(0.22f, len * 0.35f);
    float    tw   = (tip_width > 0.0f) ? tip_width : tl * 0.85f;
    AnimVec3 end  = ANIM_V3(b.x - dir.x * tl * 0.7f, b.y - dir.y * tl * 0.7f, b.z - dir.z * tl * 0.7f);
    AnimVec3 perp = ANIM_V3(-dir.y, dir.x, 0.0f);
    AnimVec3 base = ANIM_V3(b.x - dir.x * tl, b.y - dir.y * tl, b.z - dir.z * tl);
    AnimVec3 p_l  = ANIM_V3(base.x + perp.x * (0.5f * tw), base.y + perp.y * (0.5f * tw), base.z);
    AnimVec3 p_r  = ANIM_V3(base.x - perp.x * (0.5f * tw), base.y - perp.y * (0.5f * tw), base.z);

    if (ANIM_ID_VALID(stem))
    {
        anim_vmob_clear_paths(s, stem);
        anim_vmob_begin_subpath(s, stem, a);
        anim_vmob_line_to(s, stem, (len > tl) ? end : a);
    }
    if (ANIM_ID_VALID(tip))
    {
        anim_vmob_clear_paths(s, tip);
        anim_vmob_begin_subpath(s, tip, b);
        anim_vmob_line_to(s, tip, p_l);
        anim_vmob_line_to(s, tip, p_r);
        anim_vmob_close_subpath(s, tip);
    }
}

typedef struct UcmCtx {
    AnimTrackerId theta_tr;
    AnimVec3      center;
    float         radius;
    float         v_len;
    float         a_len;
    AnimId        particle;
    AnimId        r_arrow;
    AnimId        v_arrow;
    AnimId        a_arrow;
    AnimId        theta_arc;
    AnimId        right_angle;
    AnimId        lbl_r;
    AnimId        lbl_v;
    AnimId        lbl_a;
    AnimId        lbl_theta;
} UcmCtx;

static void update_ucm_geometry(AnimScene *s, AnimId target, float dt, void *user)
{
    UcmCtx  *ctx = (UcmCtx *)user;
    float    th  = anim_tracker_get(s, ctx->theta_tr);
    float    ct  = cosf(th);
    float    st  = sinf(th);
    AnimVec3 rad = ANIM_V3(ct, st, 0.0f);
    AnimVec3 tan = ANIM_V3(-st, ct, 0.0f);
    AnimVec3 p   = ANIM_V3(ctx->center.x + ctx->radius * ct, ctx->center.y + ctx->radius * st, 0.0f);
    AnimVec3 vt  = ANIM_V3(p.x + ctx->v_len * tan.x, p.y + ctx->v_len * tan.y, 0.0f);
    AnimVec3 at  = ANIM_V3(p.x - ctx->a_len * rad.x, p.y - ctx->a_len * rad.y, 0.02f);
    (void)target;
    (void)dt;

    /* Particle */
    anim_move_to(s, ctx->particle, ANIM_V3(p.x, p.y, 0.03f));

    /* Position vector r(t): center -> p */
    set_arrow_2d(s, ctx->r_arrow, ctx->center, p, 0.22f, 0.16f);

    /* Tangent velocity vector v(t): p -> p + v_len * tangent */
    set_arrow_2d(s, ctx->v_arrow, p, vt, 0.22f, 0.16f);

    /* Inward centripetal acceleration vector a(t): p -> p - a_len * r_hat */
    set_arrow_2d(s, ctx->a_arrow, ANIM_V3(p.x, p.y, 0.02f), at, 0.22f, 0.16f);

    /* Angle arc theta(t) from 0 to mod(theta, 2pi) */
    if (ANIM_ID_VALID(ctx->theta_arc))
    {
        float    ang  = fmodf(th, ANIM_TAU);
        float    ar   = 0.46f;
        uint32_t segs = 28u, k;
        if (ang < 0.08f) ang = 0.08f;
        anim_vmob_clear_paths(s, ctx->theta_arc);
        anim_vmob_begin_subpath(s, ctx->theta_arc, ANIM_V3(ctx->center.x + ar, ctx->center.y, 0.0f));
        for (k = 1u; k <= segs; ++k)
        {
            float a = ang * ((float)k / (float)segs);
            anim_vmob_line_to(s, ctx->theta_arc, ANIM_V3(ctx->center.x + ar * cosf(a), ctx->center.y + ar * sinf(a), 0.0f));
        }
    }

    /* Right-angle square between -r_hat and tangent at particle p */
    if (ANIM_ID_VALID(ctx->right_angle))
    {
        float    sz = 0.22f;
        AnimVec3 q1 = ANIM_V3(p.x - sz * rad.x, p.y - sz * rad.y, 0.01f);
        AnimVec3 q2 = ANIM_V3(q1.x + sz * tan.x, q1.y + sz * tan.y, 0.01f);
        AnimVec3 q3 = ANIM_V3(p.x + sz * tan.x, p.y + sz * tan.y, 0.01f);
        anim_vmob_clear_paths(s, ctx->right_angle);
        anim_vmob_begin_subpath(s, ctx->right_angle, q1);
        anim_vmob_line_to(s, ctx->right_angle, q2);
        anim_vmob_line_to(s, ctx->right_angle, q3);
    }

    /* Floating vector labels on opposite sides so r(t) & a(t) never collide */
    if (ANIM_ID_VALID(ctx->lbl_r))
        anim_move_to(s, ctx->lbl_r, ANIM_V3(ctx->center.x + 0.42f * ctx->radius * ct - 0.30f * tan.x,
                                            ctx->center.y + 0.42f * ctx->radius * st - 0.30f * tan.y, 0.05f));
    if (ANIM_ID_VALID(ctx->lbl_v))
        anim_move_to(s, ctx->lbl_v, ANIM_V3(vt.x + 0.28f * tan.x + 0.24f * rad.x,
                                            vt.y + 0.28f * tan.y + 0.24f * rad.y, 0.05f));
    if (ANIM_ID_VALID(ctx->lbl_a))
        anim_move_to(s, ctx->lbl_a, ANIM_V3(p.x - 0.56f * ctx->a_len * rad.x + 0.32f * tan.x,
                                            p.y - 0.56f * ctx->a_len * rad.y + 0.32f * tan.y, 0.05f));
    if (ANIM_ID_VALID(ctx->lbl_theta))
    {
        float mid = 0.5f * fmodf(th, ANIM_TAU);
        anim_move_to(s, ctx->lbl_theta, ANIM_V3(ctx->center.x + 0.72f * cosf(mid),
                                                ctx->center.y + 0.72f * sinf(mid), 0.05f));
    }
}

static void build_ucm_scene(AnimScene *s, AnimTimeline *tl, UcmCtx *ctx)
{
    const AnimColor col_r = anim_hex(0x38BDF8); /* Electric cyan-blue for position r(t) */
    const AnimColor col_v = anim_hex(0x4ADE80); /* Emerald green for velocity v(t) */
    const AnimColor col_a = anim_hex(0xFB7185); /* Coral red for acceleration a(t) */

    AnimVec3 C = ANIM_V3(-3.35f, -0.22f, 0.0f);
    float    R = 1.92f;

    /* Title & Coordinate Circle Setup */
    AnimId title = anim_text(s, NULL, "Uniform Circular Motion: Calculus Derivation", 0.44f);
    AnimAxesDesc axd = {
        .x_min = -2.5f, .x_max = 2.5f, .x_step = 1.0f,
        .y_min = -2.5f, .y_max = 2.5f, .y_step = 1.0f,
        .x_length = 4.7f, .y_length = 4.7f,
        .include_ticks = false, .include_tips = true,
        .include_grid = false, .include_numbers = false
    };
    AnimId axes     = anim_axes(s, &axd);
    AnimId ref_circ = anim_circle(s, R);
    AnimId dot      = anim_dot(s, ANIM_V3(C.x + R * cosf(0.55f), C.y + R * sinf(0.55f), 0.03f), 0.11f);
    AnimId trail    = anim_traced_path(s, dot, ANIM_YELLOW, 3.0f, 512u);

    AnimId r_arrow  = anim_arrow(s, C, ANIM_V3(C.x + R, C.y, 0.0f), 0.22f, 0.16f);
    AnimId v_arrow  = anim_arrow(s, C, ANIM_V3(C.x, C.y + 1.45f, 0.0f), 0.22f, 0.16f);
    AnimId a_arrow  = anim_arrow(s, C, ANIM_V3(C.x - 1.12f, C.y, 0.02f), 0.22f, 0.16f);
    AnimId th_arc   = anim_vmob_create(s);
    AnimId r_angle  = anim_vmob_create(s);

    AnimId lbl_r    = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{r}(t)", 0.34f);
    AnimId lbl_v    = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{v}(t)", 0.34f);
    AnimId lbl_a    = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{a}(t)", 0.34f);
    AnimId lbl_th   = anim_tex(s, ANIM_TEX_LATEX, "\\theta=\\omega t", 0.26f);

    /* Right-hand Derivation Card & LaTeX Equations */
    AnimId card     = anim_rounded_rect(s, 5.90f, 5.30f, 0.20f);
    AnimId eq_pos   = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{r}(t) = R\\begin{bmatrix}\\cos(\\omega t)\\\\ \\sin(\\omega t)\\end{bmatrix}", 0.78f);
    AnimId eq_vel   = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{v}(t) = \\frac{d\\mathbf{r}}{dt} = R\\omega\\begin{bmatrix}-\\sin(\\omega t)\\\\ \\cos(\\omega t)\\end{bmatrix}", 0.78f);
    AnimId eq_acc   = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{a}(t) = \\frac{d\\mathbf{v}}{dt} = -R\\omega^2\\begin{bmatrix}\\cos(\\omega t)\\\\ \\sin(\\omega t)\\end{bmatrix} = -\\omega^2\\mathbf{r}(t)", 0.78f);

    /* Final Highlight Equations + Permanent Framed Highlight Boxes */
    AnimId hl_hdr   = anim_text(s, NULL, "Key Uniform Circular Motion Identities", 0.27f);
    AnimId hl_v     = anim_tex(s, ANIM_TEX_LATEX, "v = \\|\\mathbf{v}(t)\\| = \\omega R", 0.54f);
    AnimId hl_ac    = anim_tex(s, ANIM_TEX_LATEX, "a_c = \\|\\mathbf{a}(t)\\| = \\omega^2 R = \\frac{v^2}{R}", 0.66f);
    AnimId hl_perp  = anim_tex(s, ANIM_TEX_LATEX, "\\mathbf{a}(t) \\perp \\mathbf{v}(t) \\quad\\text{(inward centripetal)}", 0.35f);
    AnimId box_v, box_ac;

    /* Color coding */
    anim_move_to(s, title, ANIM_V3(0.0f, 3.05f, 0.0f));
    anim_set_fill(s, title, ANIM_WHITE, false);
    anim_set_glow(s, title, 0.22f, false);

    anim_move_to(s, axes, C);
    anim_set_stroke(s, axes, anim_rgba(0.45f, 0.55f, 0.72f, 0.55f), 1.6f, true);
    anim_move_to(s, ref_circ, C);
    anim_set_fill(s, ref_circ, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_stroke(s, ref_circ, anim_rgba(0.55f, 0.65f, 0.80f, 0.40f), 1.8f, false);
    anim_set_dash(s, ref_circ, 0.14f, 0.10f, 0.0f, false);

    anim_set_fill(s, dot, ANIM_YELLOW, false);
    anim_set_glow(s, dot, 0.95f, false);

    anim_set_color(s, r_arrow, col_r, true);
    anim_set_stroke(s, r_arrow, col_r, 3.4f, true);
    anim_set_color(s, lbl_r, col_r, true);

    anim_set_color(s, v_arrow, col_v, true);
    anim_set_stroke(s, v_arrow, col_v, 3.4f, true);
    anim_set_color(s, lbl_v, col_v, true);

    anim_set_color(s, a_arrow, col_a, true);
    anim_set_stroke(s, a_arrow, col_a, 4.2f, true);
    anim_set_color(s, lbl_a, col_a, true);

    anim_set_stroke(s, th_arc, ANIM_GOLD, 2.2f, false);
    anim_set_fill(s, th_arc, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_color(s, lbl_th, ANIM_GOLD, true);

    anim_set_stroke(s, r_angle, ANIM_GOLD, 2.0f, false);
    anim_set_fill(s, r_angle, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);

    /* Sleek deep navy card so blue/green/coral equations pop cleanly */
    anim_move_to(s, card, ANIM_V3(3.05f, -0.22f, -0.10f));
    anim_set_fill(s, card, anim_rgba(0.015f, 0.025f, 0.055f, 0.90f), false);
    anim_set_stroke(s, card, anim_rgba(0.32f, 0.50f, 0.78f, 0.80f), 2.0f, false);

    anim_move_to(s, eq_pos, ANIM_V3(3.05f, 1.55f, 0.04f));
    anim_set_color(s, eq_pos, col_r, true);
    anim_move_to(s, eq_vel, ANIM_V3(3.05f, -0.10f, 0.04f));
    anim_set_color(s, eq_vel, col_v, true);
    anim_move_to(s, eq_acc, ANIM_V3(3.05f, -1.75f, 0.04f));
    anim_set_color(s, eq_acc, col_a, true);

    anim_move_to(s, hl_hdr, ANIM_V3(3.05f, 1.98f, 0.04f));
    anim_set_fill(s, hl_hdr, ANIM_TEAL, false);
    anim_move_to(s, hl_v, ANIM_V3(3.05f, 0.90f, 0.04f));
    anim_set_color(s, hl_v, col_v, true);
    anim_move_to(s, hl_ac, ANIM_V3(3.05f, -0.52f, 0.04f));
    anim_set_color(s, hl_ac, col_a, true);
    anim_move_to(s, hl_perp, ANIM_V3(3.05f, -1.98f, 0.04f));
    anim_set_color(s, hl_perp, ANIM_YELLOW, true);

    box_v  = anim_surrounding_rect(s, hl_v, 0.22f, 0.14f);
    box_ac = anim_surrounding_rect(s, hl_ac, 0.24f, 0.14f);
    anim_shift(s, box_v, ANIM_V3(0.0f, 0.0f, -0.05f));
    anim_shift(s, box_ac, ANIM_V3(0.0f, 0.0f, -0.05f));
    anim_set_fill(s, box_v, anim_rgba(col_v.r * 0.12f, col_v.g * 0.12f, col_v.b * 0.12f, 0.22f), false);
    anim_set_stroke(s, box_v, col_v, 2.8f, false);
    anim_set_glow(s, box_v, 0.65f, false);
    anim_set_fill(s, box_ac, anim_rgba(col_a.r * 0.12f, col_a.g * 0.12f, col_a.b * 0.12f, 0.22f), false);
    anim_set_stroke(s, box_ac, ANIM_YELLOW, 3.2f, false);
    anim_set_glow(s, box_ac, 0.85f, false);

    /* Tracker & updater */
    ctx->theta_tr    = anim_tracker_create(s, 0.55f);
    ctx->center      = C;
    ctx->radius      = R;
    ctx->v_len       = 1.46f;
    ctx->a_len       = 1.12f;
    ctx->particle    = dot;
    ctx->r_arrow     = r_arrow;
    ctx->v_arrow     = v_arrow;
    ctx->a_arrow     = a_arrow;
    ctx->theta_arc   = th_arc;
    ctx->right_angle = r_angle;
    ctx->lbl_r       = lbl_r;
    ctx->lbl_v       = lbl_v;
    ctx->lbl_a       = lbl_a;
    ctx->lbl_theta   = lbl_th;

    anim_add_updater(s, dot, update_ucm_geometry, ctx, NULL, 0u, &ctx->theta_tr, 1u);
    update_ucm_geometry(s, dot, 0.0f, ctx);

    /* ================================================================ */
    /* ACT 1: Circle Setup, Position Vector r(t) & Traced Path          */
    /* ================================================================ */
    anim_add(s, title);
    anim_add(s, axes);
    anim_add(s, ref_circ);
    anim_add(s, card);
    {
        AnimSpec s1[4] = {
            anim_anim_write(title),
            anim_anim_fade_in(axes, ANIM_ORIGIN, 0.98f),
            anim_anim_create(ref_circ),
            anim_anim_fade_in(card, ANIM_ORIGIN, 0.98f)
        };
        anim_play(tl, s, s1, 4u, 1.0f, ANIM_RATE_SMOOTH);
    }

    anim_add(s, trail);
    anim_add(s, dot);
    anim_add(s, r_arrow);
    anim_add(s, th_arc);
    anim_add(s, lbl_r);
    anim_add(s, lbl_th);
    anim_add(s, eq_pos);
    {
        AnimSpec s2[7] = {
            anim_anim_grow_from_center(dot),
            anim_anim_grow_arrow(r_arrow),
            anim_anim_create(th_arc),
            anim_anim_fade_in(lbl_r, ANIM_ORIGIN, 1.0f),
            anim_anim_fade_in(lbl_th, ANIM_ORIGIN, 1.0f),
            anim_anim_write(eq_pos),
            anim_anim_tracker_to(ctx->theta_tr, 1.45f)
        };
        anim_play(tl, s, s2, 7u, 2.60f, ANIM_RATE_SMOOTH);
    }
    log_info("Act 1 (r(t)) ends at %.2fs", (double)anim_timeline_duration(tl));

    /* ================================================================ */
    /* ACT 2: First Derivative -> Tangent Velocity Vector v(t)          */
    /* ================================================================ */
    anim_add(s, v_arrow);
    anim_add(s, r_angle);
    anim_add(s, lbl_v);
    anim_add(s, eq_vel);
    {
        AnimCamera cam_zoom = anim_camera_default();
        cam_zoom.pos    = ANIM_V3(-0.15f, 0.05f, 8.75f);
        cam_zoom.target = ANIM_V3(-0.15f, 0.00f, 0.0f);
        {
            AnimSpec s3[6] = {
                anim_anim_grow_arrow(v_arrow),
                anim_anim_create(r_angle),
                anim_anim_fade_in(lbl_v, ANIM_ORIGIN, 1.0f),
                anim_anim_write(eq_vel),
                anim_anim_camera_to(cam_zoom),
                anim_anim_tracker_to(ctx->theta_tr, 2.65f)
            };
            anim_play(tl, s, s3, 6u, 3.20f, ANIM_RATE_SMOOTH);
        }
    }
    {
        AnimSpec s3b[2] = {
            anim_anim_tracker_to(ctx->theta_tr, 3.45f),
            anim_anim_circumscribe(eq_vel, col_v, 0.10f)
        };
        anim_play(tl, s, s3b, 2u, 1.50f, ANIM_RATE_LINEAR);
    }
    log_info("Act 2 (v(t)) ends at %.2fs", (double)anim_timeline_duration(tl));

    /* ================================================================ */
    /* ACT 3: Second Derivative -> Inward Centripetal Acceleration a(t) */
    /* ================================================================ */
    anim_add(s, a_arrow);
    anim_add(s, lbl_a);
    anim_add(s, eq_acc);
    {
        AnimCamera cam_wide = anim_camera_default();
        AnimSpec s4[5] = {
            anim_anim_camera_to(cam_wide),
            anim_anim_grow_arrow(a_arrow),
            anim_anim_fade_in(lbl_a, ANIM_ORIGIN, 1.0f),
            anim_anim_write(eq_acc),
            anim_anim_tracker_to(ctx->theta_tr, 5.20f)
        };
        anim_play(tl, s, s4, 5u, 3.30f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec s4b[2] = {
            anim_anim_tracker_to(ctx->theta_tr, 7.10f),
            anim_anim_circumscribe(eq_acc, col_a, 0.10f)
        };
        anim_play(tl, s, s4b, 2u, 2.30f, ANIM_RATE_LINEAR);
    }
    log_info("Act 3 (a(t)) ends at %.2fs", (double)anim_timeline_duration(tl));

    /* ================================================================ */
    /* ACT 4: Highlight v = omega*R and a_c = v^2 / R                   */
    /* ================================================================ */
    {
        AnimSpec s_swap_out[3] = {
            anim_anim_fade_out(eq_pos, ANIM_V3(0.0f, 0.25f, 0.0f), 0.96f),
            anim_anim_fade_out(eq_vel, ANIM_ORIGIN, 0.96f),
            anim_anim_fade_out(eq_acc, ANIM_V3(0.0f, -0.25f, 0.0f), 0.96f)
        };
        anim_play(tl, s, s_swap_out, 3u, 0.60f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, eq_pos);
    anim_remove(s, eq_vel);
    anim_remove(s, eq_acc);

    anim_add(s, hl_hdr);
    anim_add(s, hl_v);
    anim_add(s, hl_ac);
    anim_add(s, hl_perp);
    {
        AnimCamera cam_fin = anim_camera_default();
        cam_fin.pos    = ANIM_V3(0.20f, -0.05f, 9.35f);
        cam_fin.target = ANIM_V3(0.20f, -0.05f, 0.0f);
        {
            AnimSpec s5[6] = {
                anim_anim_fade_in(hl_hdr, ANIM_ORIGIN, 1.0f),
                anim_anim_write(hl_v),
                anim_anim_write(hl_ac),
                anim_anim_fade_in(hl_perp, ANIM_ORIGIN, 1.0f),
                anim_anim_camera_to(cam_fin),
                anim_anim_tracker_to(ctx->theta_tr, 8.40f)
            };
            anim_play(tl, s, s5, 6u, 2.20f, ANIM_RATE_SMOOTH);
        }
    }
    anim_add(s, box_v);
    anim_add(s, box_ac);
    {
        AnimSpec s_hl[3] = {
            anim_anim_create(box_v),
            anim_anim_create(box_ac),
            anim_anim_tracker_to(ctx->theta_tr, 9.65f)
        };
        anim_play(tl, s, s_hl, 3u, 1.80f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec s_hold = anim_anim_tracker_to(ctx->theta_tr, 10.45f);
        anim_play(tl, s, &s_hold, 1u, 1.45f, ANIM_RATE_LINEAR);
    }
    log_info("Act 4 (highlights) ends at %.2fs", (double)anim_timeline_duration(tl));
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
    UcmCtx         ctx = { 0 };

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
    anim_scene_set_bg(scene, anim_hex(0x0B0F18), anim_hex(0x141D30));
    tl = anim_timeline_create();
    build_ucm_scene(scene, tl, &ctx);
    log_info("uniform_circular_motion duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim — Uniform Circular Motion Derivation",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "uniform_circular_motion_demo",
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
    opts.aberration_px = 0.45f;
    opts.grain_amount  = 0.0014f;

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
