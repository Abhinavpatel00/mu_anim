/*
 * examples/quaternions_viz.c
 *
 * Visualizes 3D Quaternion math from mu_lib (mu/mu_math.h):
 *   Act 1: Axis-angle parameterization q(u, theta) & double-cover half-angle
 *   Act 2: Sandwich product rotation v' = q * v * q* (mu_quat_rotate_vec3)
 *   Act 3: Quaternion composition q_net = q2 * q1 (mu_quat_mul)
 *   Act 4: Geodesic SLERP vs NLERP angular-speed comparison (mu_quat_slerp / nlerp)
 *
 * Usage:
 *   ./quaternions_demo                            Interactive preview
 *   ./quaternions_demo --shot q.png --time 15     Headless PNG shot
 *   ./quaternions_demo --video quaternions.mp4    Headless MP4 export
 */
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_math.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "../third_party/mugfx/desktop/platform.h"
#include "../third_party/mugfx/desktop/vk.h"
#include "../anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPHERE_CX (-2.75f)
#define SPHERE_CY (-0.22f)
#define SPHERE_CZ (0.0f)
#define SPHERE_R  (1.92f)

#define rgb(r, g, b)       anim_rgba((r), (g), (b), 1.0f)
#define fade_in(id)        anim_anim_fade_in((id), ANIM_ORIGIN, 1.0f)
#define fade_out(id)       anim_anim_fade_out((id), ANIM_ORIGIN, 1.0f)

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

static void make_view_quat(mu_quat q_view)
{
    mu_quat q_yaw, q_pitch;
    float   up[3]    = { 0.0f, 1.0f, 0.0f };
    float   right[3] = { 1.0f, 0.0f, 0.0f };
    mu_quat_axis_angle(q_yaw, up, -0.46f);
    mu_quat_axis_angle(q_pitch, right, 0.34f);
    mu_quat_mul(q_view, q_pitch, q_yaw);
    mu_quat_normalize(q_view, q_view);
}

static AnimVec3 project_sphere_pt(const mu_quat q_view, const float v[3], float scale)
{
    float r[3];
    mu_quat_rotate_vec3(r, q_view, v);
    return ANIM_V3(SPHERE_CX + r[0] * scale, SPHERE_CY + r[1] * scale, SPHERE_CZ + r[2] * scale);
}

static void set_3d_arrow(AnimScene *s, AnimId arrow_id, AnimVec3 a, AnimVec3 b, float tip_len, float tip_w)
{
    AnimId   stem = anim_child_at(s, arrow_id, 0u);
    AnimId   tip  = anim_child_at(s, arrow_id, 1u);
    AnimVec3 d    = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len  = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    AnimVec3 dir  = (len > 1e-5f) ? ANIM_V3(d.x / len, d.y / len, d.z / len) : ANIM_RIGHT;
    float    tl   = (tip_len > 0.0f) ? tip_len : MU_MIN(0.22f, len * 0.32f);
    float    tw   = (tip_w > 0.0f) ? tip_w : tl * 0.78f;
    AnimVec3 end  = ANIM_V3(b.x - dir.x * tl * 0.65f, b.y - dir.y * tl * 0.65f, b.z - dir.z * tl * 0.65f);
    float    plen = sqrtf(dir.x * dir.x + dir.y * dir.y);
    AnimVec3 perp = (plen > 1e-4f) ? ANIM_V3(-dir.y / plen, dir.x / plen, 0.0f) : ANIM_UP;
    AnimVec3 base = ANIM_V3(b.x - dir.x * tl, b.y - dir.y * tl, b.z - dir.z * tl);
    AnimVec3 p_l  = ANIM_V3(base.x + perp.x * 0.5f * tw, base.y + perp.y * 0.5f * tw, base.z);
    AnimVec3 p_r  = ANIM_V3(base.x - perp.x * 0.5f * tw, base.y - perp.y * 0.5f * tw, base.z);

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

static AnimId make_great_circle(AnimScene *s, const mu_quat q_view, const float u_ax[3], const float v_ax[3], float radius)
{
    AnimId   c    = anim_vmob_create(s);
    uint32_t segs = 64u, k;
    for (k = 0u; k <= segs; ++k)
    {
        float    a  = ANIM_TAU * ((float)k / (float)segs);
        float    ca = cosf(a), sa = sinf(a);
        float    p[3] = {
            u_ax[0] * ca + v_ax[0] * sa,
            u_ax[1] * ca + v_ax[1] * sa,
            u_ax[2] * ca + v_ax[2] * sa
        };
        AnimVec3 wp = project_sphere_pt(q_view, p, radius);
        if (k == 0u) anim_vmob_begin_subpath(s, c, wp);
        else         anim_vmob_line_to(s, c, wp);
    }
    anim_set_fill(s, c, anim_rgba(0, 0, 0, 0), false);
    anim_set_stroke(s, c, anim_rgba(0.45f, 0.60f, 0.82f, 0.30f), 1.4f, false);
    return c;
}

typedef struct QuatCtx {
    mu_quat       q_view;
    float         u_axis[3];
    float         v_base[3];
    mu_quat       q_slerp_a;
    mu_quat       q_slerp_b;
    AnimTrackerId tr_theta;    /* Act 1-2: axis-angle theta */
    AnimTrackerId tr_comp;     /* Act 3: 0..1 composition q2 * q1 */
    AnimTrackerId tr_slerp;    /* Act 4: 0..1 SLERP vs NLERP */
    AnimTrackerId tr_mode;     /* 0: axis-angle, 1: composition, 2: slerp vs nlerp */
    AnimId        v_arrow;
    AnimId        v_dot;
    AnimId        triad_x;
    AnimId        triad_y;
    AnimId        triad_z;
    AnimId        craft_wire;
    AnimId        sweep_arc;
    AnimId        slerp_dot;
    AnimId        nlerp_dot;
    AnimId        slerp_vec;
    AnimId        nlerp_vec;
    AnimId        omega_axes;
    AnimId        omega_slerp_dot;
    AnimId        omega_nlerp_dot;
} QuatCtx;

static float slerp_omega_fn(float t, void *user)
{
    (void)t;
    (void)user;
    return 1.0f;
}

static float nlerp_omega_fn(float t, void *user)
{
    const float omega = *(const float *)user;
    float half_w = 0.5f * omega;
    float c  = cosf(half_w);
    float s  = sinf(half_w);
    float a  = (1.0f - t);
    float b  = t;
    float n2 = (a * a + b * b + 2.0f * a * b * c);
    return (s / (n2 > 1e-5f ? n2 : 1e-5f)) / (half_w > 1e-5f ? half_w : 1.0f);
}

static void update_quat_scene(AnimScene *s, AnimId target, float dt, void *user)
{
    QuatCtx *ctx   = (QuatCtx *)user;
    float    mode  = anim_tracker_get(s, ctx->tr_mode);
    float    theta = anim_tracker_get(s, ctx->tr_theta);
    float    comp  = anim_tracker_get(s, ctx->tr_comp);
    float    st    = anim_tracker_get(s, ctx->tr_slerp);
    mu_quat  q_cur;
    (void)target;
    (void)dt;

    if (mode < 0.5f)
    {
        mu_quat_axis_angle(q_cur, ctx->u_axis, theta);
    }
    else if (mode < 1.5f)
    {
        float   ax1[3] = { 1.0f, 0.0f, 0.0f };
        float   ax2[3] = { 0.0f, 1.0f, 0.0f };
        float   u1     = MU_CLAMP(comp * 2.0f, 0.0f, 1.0f);
        float   u2     = MU_CLAMP((comp - 0.5f) * 2.0f, 0.0f, 1.0f);
        mu_quat q0, q1, q2, qt;
        mu_quat_axis_angle(q0, ctx->u_axis, theta);
        mu_quat_axis_angle(q1, ax1, u1 * 1.15f);
        mu_quat_axis_angle(q2, ax2, u2 * 1.35f);
        mu_quat_mul(qt, q1, q0);
        mu_quat_mul(q_cur, q2, qt);
        mu_quat_normalize(q_cur, q_cur);
    }
    else
    {
        mu_quat q_sl, q_nl;
        float   v_sl[3], v_nl[3];
        AnimVec3 p_sl, p_nl, org;
        mu_quat_slerp(q_sl, ctx->q_slerp_a, ctx->q_slerp_b, st);
        mu_quat_nlerp(q_nl, ctx->q_slerp_a, ctx->q_slerp_b, st);
        memcpy(q_cur, q_sl, sizeof(mu_quat));

        mu_quat_rotate_vec3(v_sl, q_sl, ctx->v_base);
        mu_quat_rotate_vec3(v_nl, q_nl, ctx->v_base);

        p_sl = project_sphere_pt(ctx->q_view, v_sl, SPHERE_R);
        p_nl = project_sphere_pt(ctx->q_view, v_nl, SPHERE_R * 1.09f);
        org  = ANIM_V3(SPHERE_CX, SPHERE_CY, SPHERE_CZ);
        if (ANIM_ID_VALID(ctx->slerp_dot)) anim_move_to(s, ctx->slerp_dot, p_sl);
        if (ANIM_ID_VALID(ctx->nlerp_dot)) anim_move_to(s, ctx->nlerp_dot, p_nl);
        if (ANIM_ID_VALID(ctx->slerp_vec)) set_3d_arrow(s, ctx->slerp_vec, org, p_sl, 0.20f, 0.14f);
        if (ANIM_ID_VALID(ctx->nlerp_vec)) set_3d_arrow(s, ctx->nlerp_vec, org, p_nl, 0.20f, 0.14f);

        if (ANIM_ID_VALID(ctx->omega_axes))
        {
            float om   = 2.0f * acosf(MU_CLAMP(fabsf(mu_quat_dot(ctx->q_slerp_a, ctx->q_slerp_b)), 0.0f, 1.0f));
            float w_sl = slerp_omega_fn(st, &om);
            float w_nl = nlerp_omega_fn(st, &om);
            if (ANIM_ID_VALID(ctx->omega_slerp_dot))
                anim_move_to(s, ctx->omega_slerp_dot, anim_axes_c2p(s, ctx->omega_axes, st, w_sl));
            if (ANIM_ID_VALID(ctx->omega_nlerp_dot))
                anim_move_to(s, ctx->omega_nlerp_dot, anim_axes_c2p(s, ctx->omega_axes, st, w_nl));
        }
    }

    /* Rotate primary vector v' = q * v * q* */
    {
        float    vp[3];
        AnimVec3 org = ANIM_V3(SPHERE_CX, SPHERE_CY, SPHERE_CZ);
        AnimVec3 p_v;
        mu_quat_rotate_vec3(vp, q_cur, ctx->v_base);
        p_v = project_sphere_pt(ctx->q_view, vp, SPHERE_R);
        set_3d_arrow(s, ctx->v_arrow, org, p_v, 0.22f, 0.15f);
        anim_move_to(s, ctx->v_dot, p_v);
    }

    /* Rotate local basis triad */
    {
        float ex0[3] = { 1, 0, 0 }, ey0[3] = { 0, 1, 0 }, ez0[3] = { 0, 0, 1 };
        float ex[3], ey[3], ez[3];
        AnimVec3 org = ANIM_V3(SPHERE_CX, SPHERE_CY, SPHERE_CZ);
        mu_quat_rotate_vec3(ex, q_cur, ex0);
        mu_quat_rotate_vec3(ey, q_cur, ey0);
        mu_quat_rotate_vec3(ez, q_cur, ez0);
        set_3d_arrow(s, ctx->triad_x, org, project_sphere_pt(ctx->q_view, ex, 0.92f), 0.14f, 0.10f);
        set_3d_arrow(s, ctx->triad_y, org, project_sphere_pt(ctx->q_view, ey, 0.92f), 0.14f, 0.10f);
        set_3d_arrow(s, ctx->triad_z, org, project_sphere_pt(ctx->q_view, ez, 0.92f), 0.14f, 0.10f);
    }

    /* Rotate 3D rigid aerospace dart wireframe */
    if (ANIM_ID_VALID(ctx->craft_wire))
    {
        static const float raw_pts[4][3] = {
            {  0.72f,  0.00f,  0.00f },
            { -0.45f,  0.00f,  0.52f },
            { -0.45f,  0.00f, -0.52f },
            { -0.35f,  0.34f,  0.00f }
        };
        static const uint32_t edges[6][2] = {
            {0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}
        };
        AnimVec3 wp[4];
        uint32_t i;
        for (i = 0u; i < 4u; ++i)
        {
            float rp[3];
            mu_quat_rotate_vec3(rp, q_cur, raw_pts[i]);
            wp[i] = project_sphere_pt(ctx->q_view, rp, 1.0f);
        }
        anim_vmob_clear_paths(s, ctx->craft_wire);
        for (i = 0u; i < 6u; ++i)
        {
            anim_vmob_begin_subpath(s, ctx->craft_wire, wp[edges[i][0]]);
            anim_vmob_line_to(s, ctx->craft_wire, wp[edges[i][1]]);
        }
    }

    /* Sweep arc around u_axis in mode 0 */
    if (ANIM_ID_VALID(ctx->sweep_arc) && mode < 0.5f)
    {
        float    ang     = fmodf(fabsf(theta), ANIM_TAU);
        uint32_t segs    = 32u, k;
        float    u_dot_v = mu_vec3_dot(ctx->u_axis, ctx->v_base);
        float    proj_c[3], ortho_a[3], ortho_b[3];
        if (ang < 0.06f) ang = 0.06f;
        for (k = 0u; k < 3u; ++k)
        {
            proj_c[k]  = ctx->u_axis[k] * u_dot_v;
            ortho_a[k] = ctx->v_base[k] - proj_c[k];
        }
        mu_vec3_cross(ortho_b, ctx->u_axis, ortho_a);
        anim_vmob_clear_paths(s, ctx->sweep_arc);
        for (k = 0u; k <= segs; ++k)
        {
            float    a  = ((theta >= 0.0f) ? 1.0f : -1.0f) * ang * ((float)k / (float)segs);
            float    ca = cosf(a), sa = sinf(a);
            float    pt[3] = {
                proj_c[0] + ortho_a[0] * ca + ortho_b[0] * sa,
                proj_c[1] + ortho_a[1] * ca + ortho_b[1] * sa,
                proj_c[2] + ortho_a[2] * ca + ortho_b[2] * sa
            };
            AnimVec3 wp = project_sphere_pt(ctx->q_view, pt, SPHERE_R);
            if (k == 0u) anim_vmob_begin_subpath(s, ctx->sweep_arc, wp);
            else         anim_vmob_line_to(s, ctx->sweep_arc, wp);
        }
    }
}

static void build_scene(AnimScene *s, AnimTimeline *tl, QuatCtx *ctx, const char *font_path)
{
    anim_scene_set_bg(s, anim_hex(0x000000), anim_hex(0x050812));

    make_view_quat(ctx->q_view);
    {
        float u_raw[3] = { 0.38f, 0.86f, 0.34f };
        float v_raw[3] = { 0.85f, -0.25f, 0.46f };
        mu_vec3_normalize(ctx->u_axis, u_raw);
        mu_vec3_normalize(ctx->v_base, v_raw);
    }
    {
        float ax_a[3] = { 0.0f, 1.0f, 0.0f };
        float ax_b[3] = { 0.28f, -0.58f, 0.76f };
        mu_vec3_normalize(ax_b, ax_b);
        mu_quat_axis_angle(ctx->q_slerp_a, ax_a, -0.85f);
        mu_quat_axis_angle(ctx->q_slerp_b, ax_b,  2.15f);
    }

    ctx->tr_theta = anim_tracker_create(s, 0.0f);
    ctx->tr_comp  = anim_tracker_create(s, 0.0f);
    ctx->tr_slerp = anim_tracker_create(s, 0.0f);
    ctx->tr_mode  = anim_tracker_create(s, 0.0f);

    AnimColor col_sky    = anim_hex(0x38BDF8);
    AnimColor col_green  = anim_hex(0x4ADE80);
    AnimColor col_rose   = anim_hex(0xFB7185);
    AnimColor col_gold   = anim_hex(0xF9E26B);
    AnimColor col_violet = anim_hex(0xA78BFA);
    AnimColor col_amber  = anim_hex(0xF59E0B);

    AnimId title = anim_text(s, font_path, "3D Quaternions: Axis-Angle, Rotation & SLERP", 0.42f);
    anim_move_to(s, title, ANIM_V3(0.0f, 3.12f, 0.0f));
    anim_set_fill(s, title, rgb(0.94f, 0.97f, 1.0f), false);
    anim_set_glow(s, title, 0.20f, false);

    AnimId sphere = anim_sphere(s, SPHERE_R, 40u, 22u);
    anim_move_to(s, sphere, ANIM_V3(SPHERE_CX, SPHERE_CY, SPHERE_CZ));
    anim_rotate(s, sphere, -0.46f, ANIM_UP);
    anim_rotate(s, sphere,  0.34f, ANIM_RIGHT);
    anim_set_fill(s, sphere, anim_rgba(0.018f, 0.042f, 0.095f, 0.24f), false);
    anim_surface_set_wireframe(s, sphere, 12.0f, 0.20f, false);
    anim_surface_set_roughness(s, sphere, 0.58f, false);

    float ex[3] = {1,0,0}, ey[3] = {0,1,0}, ez[3] = {0,0,1};
    AnimId ring_xy = make_great_circle(s, ctx->q_view, ex, ey, SPHERE_R);
    AnimId ring_xz = make_great_circle(s, ctx->q_view, ex, ez, SPHERE_R);
    AnimId ring_yz = make_great_circle(s, ctx->q_view, ey, ez, SPHERE_R);

    AnimId world_axes = anim_vmob_create(s);
    {
        float x0[3] = {-1.1f,0,0}, x1[3] = {1.1f,0,0};
        float y0[3] = {0,-1.1f,0}, y1[3] = {0,1.1f,0};
        float z0[3] = {0,0,-1.1f}, z1[3] = {0,0,1.1f};
        anim_vmob_begin_subpath(s, world_axes, project_sphere_pt(ctx->q_view, x0, SPHERE_R));
        anim_vmob_line_to(s, world_axes, project_sphere_pt(ctx->q_view, x1, SPHERE_R));
        anim_vmob_begin_subpath(s, world_axes, project_sphere_pt(ctx->q_view, y0, SPHERE_R));
        anim_vmob_line_to(s, world_axes, project_sphere_pt(ctx->q_view, y1, SPHERE_R));
        anim_vmob_begin_subpath(s, world_axes, project_sphere_pt(ctx->q_view, z0, SPHERE_R));
        anim_vmob_line_to(s, world_axes, project_sphere_pt(ctx->q_view, z1, SPHERE_R));
    }
    anim_set_fill(s, world_axes, anim_rgba(0,0,0,0), false);
    anim_set_stroke(s, world_axes, anim_rgba(0.55f, 0.65f, 0.80f, 0.28f), 1.4f, false);

    float u_neg[3] = { -ctx->u_axis[0], -ctx->u_axis[1], -ctx->u_axis[2] };
    AnimVec3 u_a = project_sphere_pt(ctx->q_view, u_neg, SPHERE_R * 1.15f);
    AnimVec3 u_b = project_sphere_pt(ctx->q_view, ctx->u_axis, SPHERE_R * 1.24f);
    AnimId u_axis_arrow = anim_arrow(s, u_a, u_b, 0.22f, 0.16f);
    anim_set_color(s, u_axis_arrow, col_gold, true);
    anim_set_stroke(s, u_axis_arrow, col_gold, 2.8f, true);
    anim_set_glow(s, u_axis_arrow, 0.35f, true);

    AnimId lbl_u = anim_tex(s, ANIM_TEX_LATEX, "\\hat{u}", 0.36f);
    anim_move_to(s, lbl_u, ANIM_V3(u_b.x + 0.22f, u_b.y + 0.18f, u_b.z));
    anim_set_fill(s, lbl_u, col_gold, true);

    ctx->sweep_arc = anim_vmob_create(s);
    anim_set_fill(s, ctx->sweep_arc, anim_rgba(0,0,0,0), false);
    anim_set_stroke(s, ctx->sweep_arc, col_amber, 2.6f, false);

    ctx->craft_wire = anim_vmob_create(s);
    anim_set_fill(s, ctx->craft_wire, anim_rgba(0,0,0,0), false);
    anim_set_stroke(s, ctx->craft_wire, anim_rgba(0.88f, 0.94f, 1.0f, 0.85f), 2.2f, false);

    ctx->triad_x = anim_arrow(s, ANIM_ORIGIN, ANIM_RIGHT, 0.14f, 0.10f);
    ctx->triad_y = anim_arrow(s, ANIM_ORIGIN, ANIM_UP,    0.14f, 0.10f);
    ctx->triad_z = anim_arrow(s, ANIM_ORIGIN, ANIM_OUT,   0.14f, 0.10f);
    anim_set_color(s, ctx->triad_x, col_rose,  true);
    anim_set_color(s, ctx->triad_y, col_green, true);
    anim_set_color(s, ctx->triad_z, col_sky,   true);
    anim_set_stroke(s, ctx->triad_x, col_rose,  2.2f, true);
    anim_set_stroke(s, ctx->triad_y, col_green, 2.2f, true);
    anim_set_stroke(s, ctx->triad_z, col_sky,   2.2f, true);

    ctx->v_arrow = anim_arrow(s, ANIM_ORIGIN, ANIM_RIGHT, 0.22f, 0.15f);
    anim_set_color(s, ctx->v_arrow, col_sky, true);
    anim_set_stroke(s, ctx->v_arrow, col_sky, 3.6f, true);
    anim_set_glow(s, ctx->v_arrow, 0.35f, true);

    ctx->v_dot = anim_dot(s, ANIM_V3(SPHERE_CX, SPHERE_CY, SPHERE_CZ), 0.095f);
    anim_set_fill(s, ctx->v_dot, col_sky, false);
    anim_set_glow(s, ctx->v_dot, 0.85f, false);

    AnimId v_trail = anim_traced_path(s, ctx->v_dot, anim_rgba(0.22f, 0.74f, 0.97f, 0.80f), 2.4f, 160u);

    float va_raw[3], vb_raw[3];
    mu_quat_rotate_vec3(va_raw, ctx->q_slerp_a, ctx->v_base);
    mu_quat_rotate_vec3(vb_raw, ctx->q_slerp_b, ctx->v_base);
    AnimId dot_a = anim_dot(s, project_sphere_pt(ctx->q_view, va_raw, SPHERE_R), 0.08f);
    AnimId dot_b = anim_dot(s, project_sphere_pt(ctx->q_view, vb_raw, SPHERE_R), 0.08f);
    anim_set_fill(s, dot_a, col_sky, false);
    anim_set_fill(s, dot_b, col_violet, false);

    ctx->slerp_vec = anim_arrow(s, ANIM_ORIGIN, ANIM_RIGHT, 0.20f, 0.14f);
    ctx->nlerp_vec = anim_arrow(s, ANIM_ORIGIN, ANIM_RIGHT, 0.20f, 0.14f);
    anim_set_color(s, ctx->slerp_vec, col_green, true);
    anim_set_color(s, ctx->nlerp_vec, col_rose,  true);
    anim_set_stroke(s, ctx->slerp_vec, col_green, 3.4f, true);
    anim_set_stroke(s, ctx->nlerp_vec, col_rose,  2.8f, true);

    ctx->slerp_dot = anim_dot(s, project_sphere_pt(ctx->q_view, va_raw, SPHERE_R), 0.09f);
    ctx->nlerp_dot = anim_dot(s, project_sphere_pt(ctx->q_view, va_raw, SPHERE_R * 1.09f), 0.085f);
    anim_set_fill(s, ctx->slerp_dot, col_green, false);
    anim_set_fill(s, ctx->nlerp_dot, col_rose,  false);
    anim_set_glow(s, ctx->slerp_dot, 0.8f, false);
    anim_set_glow(s, ctx->nlerp_dot, 0.6f, false);

    AnimId slerp_trail = anim_traced_path(s, ctx->slerp_dot, col_green, 3.0f, 120u);
    AnimId nlerp_trail = anim_traced_path(s, ctx->nlerp_dot, col_rose,  2.4f, 120u);

    AnimId card = anim_rounded_rect(s, 5.75f, 5.45f, 0.22f);
    anim_move_to(s, card, ANIM_V3(3.15f, -0.22f, -0.15f));
    anim_set_fill(s, card, anim_rgba(0.005f, 0.009f, 0.022f, 0.92f), false);
    anim_set_stroke(s, card, anim_rgba(0.28f, 0.48f, 0.78f, 0.85f), 2.0f, false);

    AnimId eq1 = anim_tex(s, ANIM_TEX_LATEX,
        "q(\\hat{u},\\theta) = \\cos\\frac{\\theta}{2} + \\hat{u}\\sin\\frac{\\theta}{2}", 0.58f);
    anim_move_to(s, eq1, ANIM_V3(3.15f, 1.72f, 0.0f));
    anim_set_fill(s, eq1, col_gold, true);

    AnimId note1 = anim_text(s, font_path, "Double cover: q(2pi) = -q(0)  |  ||q|| = 1 on S^3", 0.22f);
    anim_move_to(s, note1, ANIM_V3(3.15f, 1.00f, 0.0f));
    anim_set_fill(s, note1, rgb(0.75f, 0.84f, 0.96f), false);

    AnimId eq2 = anim_tex(s, ANIM_TEX_LATEX,
        "\\mathbf{v}' = q\\,\\mathbf{v}\\,q^* \\quad (q^* = w - \\vec{u})", 0.42f);
    anim_move_to(s, eq2, ANIM_V3(3.15f, 0.02f, 0.0f));
    anim_set_fill(s, eq2, col_sky, true);

    AnimId eq3 = anim_tex(s, ANIM_TEX_LATEX,
        "q_{\\text{net}} = q_2 \\cdot q_1 \\neq q_1 \\cdot q_2", 0.46f);
    anim_move_to(s, eq3, ANIM_V3(3.15f, -1.35f, 0.0f));
    anim_set_fill(s, eq3, col_green, true);

    AnimId note3 = anim_text(s, font_path, "mu_quat_mul: gimbal-lock-free 3D composition", 0.22f);
    anim_move_to(s, note3, ANIM_V3(3.15f, -2.08f, 0.0f));
    anim_set_fill(s, note3, rgb(0.75f, 0.84f, 0.96f), false);

    AnimId eq4 = anim_tex(s, ANIM_TEX_LATEX,
        "\\mathrm{slerp}(q_a, q_b, t) = \\frac{\\sin((1-t)\\Omega)}{\\sin\\Omega}q_a + \\frac{\\sin(t\\Omega)}{\\sin\\Omega}q_b",
        0.50f);
    anim_move_to(s, eq4, ANIM_V3(3.15f, 1.72f, 0.0f));
    anim_set_fill(s, eq4, col_green, true);

    AnimAxesDesc ax_desc = {
        .x_min = 0.0f, .x_max = 1.0f, .x_step = 0.25f,
        .y_min = 0.68f, .y_max = 1.42f, .y_step = 0.20f,
        .x_length = 4.35f, .y_length = 2.05f,
        .include_ticks = true, .include_tips = true,
        .include_grid = true, .include_numbers = false,
        .font_path = font_path
    };
    ctx->omega_axes = anim_axes(s, &ax_desc);
    anim_move_to(s, ctx->omega_axes, ANIM_V3(3.25f, -0.35f, 0.0f));
    anim_set_stroke(s, ctx->omega_axes, anim_rgba(0.60f, 0.72f, 0.90f, 0.75f), 1.6f, true);

    float omega_val = 2.0f * acosf(MU_CLAMP(fabsf(mu_quat_dot(ctx->q_slerp_a, ctx->q_slerp_b)), 0.0f, 1.0f));
    AnimId curve_slerp = anim_plot(s, ctx->omega_axes, slerp_omega_fn, 0.0f, 1.0f, 48u, &omega_val);
    AnimId curve_nlerp = anim_plot(s, ctx->omega_axes, nlerp_omega_fn, 0.0f, 1.0f, 64u, &omega_val);
    anim_set_fill(s, curve_slerp, anim_rgba(0,0,0,0), false);
    anim_set_fill(s, curve_nlerp, anim_rgba(0,0,0,0), false);
    anim_set_stroke(s, curve_slerp, col_green, 3.2f, false);
    anim_set_stroke(s, curve_nlerp, col_rose,  3.0f, false);

    ctx->omega_slerp_dot = anim_dot(s, anim_axes_c2p(s, ctx->omega_axes, 0.0f, 1.0f), 0.08f);
    ctx->omega_nlerp_dot = anim_dot(s, anim_axes_c2p(s, ctx->omega_axes, 0.0f, nlerp_omega_fn(0.0f, &omega_val)), 0.08f);
    anim_set_fill(s, ctx->omega_slerp_dot, col_green, false);
    anim_set_fill(s, ctx->omega_nlerp_dot, col_rose,  false);

    AnimId plot_lbl = anim_text(s, font_path, "Angular speed w(t):  SLERP (const) vs NLERP (curved)", 0.22f);
    anim_move_to(s, plot_lbl, ANIM_V3(3.15f, -1.95f, 0.0f));
    anim_set_fill(s, plot_lbl, rgb(0.85f, 0.92f, 1.0f), false);

    AnimId leg_sl = anim_text(s, font_path, "SLERP: geodesic S^3", 0.21f);
    AnimId leg_nl = anim_text(s, font_path, "NLERP: chord + renormalize", 0.21f);
    anim_move_to(s, leg_sl, ANIM_V3(1.85f, -2.42f, 0.0f));
    anim_move_to(s, leg_nl, ANIM_V3(4.35f, -2.42f, 0.0f));
    anim_set_fill(s, leg_sl, col_green, false);
    anim_set_fill(s, leg_nl, col_rose,  false);

    {
        AnimTrackerId deps[4] = { ctx->tr_theta, ctx->tr_comp, ctx->tr_slerp, ctx->tr_mode };
        anim_add_updater(s, ctx->v_arrow, update_quat_scene, ctx, NULL, 0u, deps, 4u);
    }
    update_quat_scene(s, ctx->v_arrow, 0.0f, ctx);

    /* ---------------- Timeline Choreography ---------------- */
    anim_add(s, title);
    anim_add(s, card);
    anim_add(s, sphere);
    anim_add(s, ring_xy); anim_add(s, ring_xz); anim_add(s, ring_yz);
    anim_add(s, world_axes);
    anim_add(s, u_axis_arrow); anim_add(s, lbl_u);
    anim_add(s, ctx->craft_wire);
    anim_add(s, ctx->triad_x); anim_add(s, ctx->triad_y); anim_add(s, ctx->triad_z);
    anim_add(s, ctx->v_arrow); anim_add(s, ctx->v_dot); anim_add(s, v_trail);
    anim_add(s, ctx->sweep_arc);
    anim_add(s, eq1); anim_add(s, note1);
    {
        AnimSpec sp[] = {
            anim_anim_write(title),
            fade_in(card),
            fade_in(sphere),
            anim_anim_create(ring_xy), anim_anim_create(ring_xz), anim_anim_create(ring_yz),
            fade_in(world_axes),
            anim_anim_grow_arrow(u_axis_arrow),
            fade_in(lbl_u),
            anim_anim_write(eq1),
            fade_in(note1),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.35f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec sp[] = {
            anim_anim_grow_arrow(ctx->v_arrow),
            anim_anim_grow_from_center(ctx->v_dot),
            fade_in(ctx->craft_wire),
            fade_in(ctx->triad_x), fade_in(ctx->triad_y), fade_in(ctx->triad_z),
            fade_in(ctx->sweep_arc),
            anim_anim_tracker_to(ctx->tr_theta, 3.14159f),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 3.25f, ANIM_RATE_SMOOTH);
    }

    anim_add(s, eq2);
    {
        AnimSpec sp[] = {
            anim_anim_write(eq2),
            anim_anim_indicate(eq2, 1.10f, col_sky),
            anim_anim_tracker_to(ctx->tr_theta, ANIM_TAU),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 4.20f, ANIM_RATE_SMOOTH);
    }

    anim_add(s, eq3); anim_add(s, note3);
    {
        AnimSpec sp[] = {
            anim_anim_tracker_to(ctx->tr_mode, 1.0f),
            anim_anim_write(eq3),
            fade_in(note3),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 0.70f, ANIM_RATE_LINEAR);
    }
    {
        AnimSpec sp[] = {
            anim_anim_circumscribe(eq3, col_green, 0.14f),
            anim_anim_tracker_to(ctx->tr_comp, 1.0f),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 3.50f, ANIM_RATE_SMOOTH);
    }

    {
        AnimSpec sp[] = {
            fade_out(eq1), fade_out(note1),
            fade_out(eq2),
            fade_out(eq3), fade_out(note3),
            fade_out(u_axis_arrow), fade_out(lbl_u),
            fade_out(ctx->sweep_arc),
            fade_out(v_trail),
            fade_out(ctx->v_arrow),
            fade_out(ctx->v_dot),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 0.65f, ANIM_RATE_SMOOTH);
    }
    anim_remove(s, eq1); anim_remove(s, note1); anim_remove(s, eq2);
    anim_remove(s, eq3); anim_remove(s, note3);
    anim_remove(s, u_axis_arrow); anim_remove(s, lbl_u);
    anim_remove(s, ctx->sweep_arc); anim_remove(s, v_trail);
    anim_remove(s, ctx->v_arrow); anim_remove(s, ctx->v_dot);

    anim_add(s, dot_a); anim_add(s, dot_b);
    anim_add(s, ctx->slerp_vec); anim_add(s, ctx->nlerp_vec);
    anim_add(s, ctx->slerp_dot); anim_add(s, ctx->nlerp_dot);
    anim_add(s, slerp_trail); anim_add(s, nlerp_trail);
    anim_add(s, eq4); anim_add(s, ctx->omega_axes);
    anim_add(s, curve_slerp); anim_add(s, curve_nlerp);
    anim_add(s, ctx->omega_slerp_dot); anim_add(s, ctx->omega_nlerp_dot);
    anim_add(s, plot_lbl); anim_add(s, leg_sl); anim_add(s, leg_nl);
    {
        AnimSpec sp[] = {
            anim_anim_tracker_to(ctx->tr_mode, 2.0f),
            anim_anim_write(eq4),
            fade_in(ctx->omega_axes),
            anim_anim_create(curve_slerp),
            anim_anim_create(curve_nlerp),
            fade_in(plot_lbl),
            fade_in(leg_sl),
            fade_in(leg_nl),
            anim_anim_grow_from_center(dot_a),
            anim_anim_grow_from_center(dot_b),
            fade_in(ctx->slerp_vec), fade_in(ctx->nlerp_vec),
            fade_in(ctx->slerp_dot), fade_in(ctx->nlerp_dot),
            fade_in(ctx->omega_slerp_dot), fade_in(ctx->omega_nlerp_dot),
        };
        anim_play(tl, s, sp, MU_ARRAY_COUNT(sp), 1.25f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec sp[] = {
            anim_anim_tracker_to(ctx->tr_slerp, 1.0f),
        };
        anim_play(tl, s, sp, 1u, 3.10f, ANIM_RATE_LINEAR);
    }
    anim_wait(tl, s, 0.85f);
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
    QuatCtx        ctx = { 0 };

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
    build_scene(scene, tl, &ctx, find_font_path());
    log_info("quaternions_viz duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim - 3D Quaternions",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "quaternions_demo",
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
