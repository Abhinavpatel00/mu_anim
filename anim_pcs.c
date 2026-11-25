/*
 * anim_pcs.c — Physics & Computer Science high-level animation toolkit for mu_anim.
 * Built cleanly on top of anim.h + anim_internal.h (strict C11, zero warnings).
 */
#include "anim_internal.h"

static void append_polyline_subpath(AnimScene *s, AnimId id, const AnimVec3 *pts, uint32_t n)
{
    uint32_t i;
    if (n == 0u)
        return;
    anim_vmob_begin_subpath(s, id, pts[0]);
    for (i = 1u; i < n; ++i)
        anim_vmob_line_to(s, id, pts[i]);
}

/* ============================================================================
 *  PHYSICS TOOLKIT
 * ==========================================================================*/

void anim_phys_step_rk4(float *y, uint32_t dim, float t, float dt, AnimOdeFn fn, void *user)
{
    float  stack_buf[64 * 5];
    float *buf;
    float *k1, *k2, *k3, *k4, *yt;
    uint32_t i;

    if (!y || !fn || dim == 0u)
        return;
    buf = (dim <= 64u) ? stack_buf : (float *)malloc((size_t)dim * 5u * sizeof *buf);
    MU_ASSERT(buf != NULL);
    k1 = buf;
    k2 = buf + dim;
    k3 = buf + dim * 2u;
    k4 = buf + dim * 3u;
    yt = buf + dim * 4u;

    fn(t, y, k1, dim, user);
    for (i = 0u; i < dim; ++i)
        yt[i] = y[i] + 0.5f * dt * k1[i];
    fn(t + 0.5f * dt, yt, k2, dim, user);
    for (i = 0u; i < dim; ++i)
        yt[i] = y[i] + 0.5f * dt * k2[i];
    fn(t + 0.5f * dt, yt, k3, dim, user);
    for (i = 0u; i < dim; ++i)
        yt[i] = y[i] + dt * k3[i];
    fn(t + dt, yt, k4, dim, user);

    for (i = 0u; i < dim; ++i)
        y[i] += (dt / 6.0f) * (k1[i] + 2.0f * k2[i] + 2.0f * k3[i] + k4[i]);

    if (buf != stack_buf)
        free(buf);
}

AnimId anim_phys_trajectory(AnimScene *s, const float *y0, uint32_t dim, float t0, float t1, uint32_t steps,
                            AnimOdeFn fn, AnimOdeProjFn proj, void *user)
{
    AnimId    id  = anim_vmob_create(s);
    uint32_t  n   = MU_MAX(steps, 4u);
    float     dt  = (t1 - t0) / (float)n;
    float    *y   = (float *)malloc((size_t)dim * sizeof *y);
    AnimVec3 *pts = (AnimVec3 *)malloc((size_t)(n + 1u) * sizeof *pts);
    uint32_t  i;
    float     t = t0;

    MU_ASSERT(y != NULL && pts != NULL);
    memcpy(y, y0, (size_t)dim * sizeof *y);
    for (i = 0u; i <= n; ++i)
    {
        pts[i] = proj ? proj(y, dim, user) : ANIM_V3(dim >= 1u ? y[0] : 0.0f, dim >= 2u ? y[1] : 0.0f, dim >= 3u ? y[2] : 0.0f);
        if (i < n)
        {
            anim_phys_step_rk4(y, dim, t, dt, fn, user);
            t += dt;
        }
    }

    anim_vmob_begin_subpath(s, id, pts[0]);
    for (i = 0u; i < n; ++i)
    {
        AnimVec3 pm = (i > 0u) ? pts[i - 1u] : pts[i];
        AnimVec3 p0 = pts[i];
        AnimVec3 p1 = pts[i + 1u];
        AnimVec3 pp = (i + 1u < n) ? pts[i + 2u] : p1;
        AnimVec3 c1 = ANIM_V3(p0.x + (p1.x - pm.x) / 6.0f, p0.y + (p1.y - pm.y) / 6.0f, p0.z + (p1.z - pm.z) / 6.0f);
        AnimVec3 c2 = ANIM_V3(p1.x - (pp.x - p0.x) / 6.0f, p1.y - (pp.y - p0.y) / 6.0f, p1.z - (pp.z - p0.z) / 6.0f);
        anim_vmob_cubic_to(s, id, c1, c2, p1);
    }
    anim_set_stroke(s, id, ANIM_TEAL, 3.2f, false);
    free(y);
    free(pts);
    return id;
}

void anim_phys_spring_set(AnimScene *s, AnimId spring_id, AnimVec3 p0, AnimVec3 p1, uint32_t coils, float width)
{
    AnimVec3 d   = ANIM_V3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
    float    len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    uint32_t nc  = MU_CLAMP(coils, 2u, 32u);
    uint32_t pts_n = nc * 2u + 4u;
    AnimVec3 pts[72];
    AnimVec3 dir, perp;
    float    lead;
    uint32_t k;

    anim_vmob_clear_paths(s, spring_id);
    if (len < 1e-4f)
        return;
    dir  = ANIM_V3(d.x / len, d.y / len, d.z / len);
    perp = ANIM_V3(-dir.y, dir.x, 0.0f);
    if (perp.x * perp.x + perp.y * perp.y < 1e-6f)
        perp = ANIM_V3(0.0f, 1.0f, 0.0f);
    lead = MU_MIN(len * 0.12f, 0.25f);

    pts[0] = p0;
    pts[1] = ANIM_V3(p0.x + dir.x * lead, p0.y + dir.y * lead, p0.z + dir.z * lead);
    for (k = 0u; k < nc * 2u; ++k)
    {
        float    u   = ((float)k + 0.5f) / (float)(nc * 2u);
        float    s_a = lead + u * (len - 2.0f * lead);
        float    sgn = (k & 1u) ? -1.0f : 1.0f;
        float    hw  = 0.5f * width * sgn;
        pts[2u + k] = ANIM_V3(p0.x + dir.x * s_a + perp.x * hw,
                              p0.y + dir.y * s_a + perp.y * hw,
                              p0.z + dir.z * s_a + perp.z * hw);
    }
    pts[pts_n - 2u] = ANIM_V3(p1.x - dir.x * lead, p1.y - dir.y * lead, p1.z - dir.z * lead);
    pts[pts_n - 1u] = p1;

    append_polyline_subpath(s, spring_id, pts, pts_n);
}

AnimId anim_phys_spring(AnimScene *s, AnimVec3 p0, AnimVec3 p1, uint32_t coils, float width)
{
    AnimId id = anim_vmob_create(s);
    anim_phys_spring_set(s, id, p0, p1, coils, width);
    anim_set_stroke(s, id, ANIM_BLUE_A, 3.2f, false);
    return id;
}

AnimId anim_phys_pendulum(AnimScene *s, AnimVec3 pivot, float length, float angle_rad, float bob_radius)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    AnimVec3 bob_pos = ANIM_V3(pivot.x + length * sinf(angle_rad),
                               pivot.y - length * cosf(angle_rad),
                               pivot.z);
    AnimId   rod = anim_line(s, pivot, bob_pos);
    AnimId   piv = anim_dot(s, pivot, MU_MAX(bob_radius * 0.28f, 0.05f));
    AnimId   bob = anim_circle(s, bob_radius);

    anim_set_stroke(s, rod, ANIM_GREY_B, 3.2f, false);
    anim_set_tag(s, rod, "rod");
    anim_set_fill(s, piv, ANIM_WHITE, false);
    anim_set_tag(s, piv, "pivot");
    anim_move_to(s, bob, bob_pos);
    anim_set_fill_gradient_radial(s, bob, ANIM_YELLOW, ANIM_GOLD, bob_pos, bob_radius, false);
    anim_set_stroke(s, bob, ANIM_WHITE, 2.0f, false);
    anim_set_glow(s, bob, 0.45f, false);
    anim_set_tag(s, bob, "bob");

    anim_add_child(s, grp, rod);
    anim_add_child(s, grp, piv);
    anim_add_child(s, grp, bob);
    return grp;
}

void anim_phys_pendulum_set(AnimScene *s, AnimId pend_id, AnimVec3 pivot, float length, float angle_rad)
{
    AnimId   rod = anim_find_by_tag(s, pend_id, "rod");
    AnimId   piv = anim_find_by_tag(s, pend_id, "pivot");
    AnimId   bob = anim_find_by_tag(s, pend_id, "bob");
    AnimVec3 bob_pos = ANIM_V3(pivot.x + length * sinf(angle_rad),
                               pivot.y - length * cosf(angle_rad),
                               pivot.z);
    if (ANIM_ID_VALID(rod))
    {
        anim_vmob_clear_paths(s, rod);
        anim_vmob_begin_subpath(s, rod, pivot);
        anim_vmob_line_to(s, rod, bob_pos);
    }
    if (ANIM_ID_VALID(piv))
        anim_move_to(s, piv, pivot);
    if (ANIM_ID_VALID(bob))
        anim_move_to(s, bob, bob_pos);
}

AnimId anim_phys_free_body(AnimScene *s, AnimVec3 center, float box_w, float box_h,
                           const AnimVec3 *forces, const char *const *labels, const AnimColor *colors,
                           uint32_t force_count, const char *font_path)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    AnimId   box = anim_rounded_rect(s, box_w, box_h, 0.10f);
    AnimId   com = anim_dot(s, center, 0.06f);
    uint32_t i;

    anim_move_to(s, box, center);
    anim_set_fill_gradient_linear(s, box, anim_hex_a(0x23334F, 0.92f), anim_hex_a(0x182336, 0.92f),
                                  ANIM_V3(center.x - 0.5f * box_w, center.y + 0.5f * box_h, center.z),
                                  ANIM_V3(center.x + 0.5f * box_w, center.y - 0.5f * box_h, center.z), false);
    anim_set_stroke(s, box, ANIM_BLUE_B, 2.5f, false);
    anim_set_shadow(s, box, 10.0f, false);
    anim_set_fill(s, com, ANIM_WHITE, false);
    anim_add_child(s, grp, box);
    anim_add_child(s, grp, com);

    for (i = 0u; i < force_count; ++i)
    {
        AnimVec3  f   = forces ? forces[i] : ANIM_UP;
        float     fl  = sqrtf(f.x * f.x + f.y * f.y + f.z * f.z);
        AnimColor col = colors ? colors[i] : ANIM_GOLD;
        AnimVec3  end = ANIM_V3(center.x + f.x, center.y + f.y, center.z + f.z);
        AnimId    arr = anim_arrow(s, center, end, 0.22f, 0.16f);

        anim_set_color(s, arr, col, true);
        anim_set_glow(s, arr, 0.4f, true);
        anim_add_child(s, grp, arr);

        if (labels && labels[i] && labels[i][0] && fl > 1e-4f)
        {
            AnimId lbl = anim_text(s, font_path, labels[i], 0.30f);
            AnimVec3 lpos = ANIM_V3(end.x + (f.x / fl) * 0.28f, end.y + (f.y / fl) * 0.28f, end.z);
            anim_set_color(s, lbl, col, true);
            anim_move_to(s, lbl, lpos);
            anim_add_child(s, grp, lbl);
        }
    }
    return grp;
}

void anim_phys_wave_set(AnimScene *s, AnimId wave_id, AnimVec3 p0, AnimVec3 p1, float amplitude, float cycles, float phase_rad, uint32_t samples)
{
    AnimVec3 d   = ANIM_V3(p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
    float    len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    uint32_t n   = MU_CLAMP(samples, 8u, 256u);
    AnimVec3 pts[260];
    AnimVec3 perp;
    uint32_t i;

    anim_vmob_clear_paths(s, wave_id);
    if (len < 1e-4f)
        return;
    perp = ANIM_V3(-d.y / len, d.x / len, 0.0f);
    if (perp.x * perp.x + perp.y * perp.y < 1e-6f)
        perp = ANIM_V3(0.0f, 1.0f, 0.0f);

    for (i = 0u; i <= n; ++i)
    {
        float u   = (float)i / (float)n;
        float off = amplitude * sinf(ANIM_TAU * cycles * u + phase_rad);
        pts[i]    = ANIM_V3(p0.x + d.x * u + perp.x * off,
                            p0.y + d.y * u + perp.y * off,
                            p0.z + d.z * u + perp.z * off);
    }
    append_polyline_subpath(s, wave_id, pts, n + 1u);
}

AnimId anim_phys_wave(AnimScene *s, AnimVec3 p0, AnimVec3 p1, float amplitude, float cycles, float phase_rad, uint32_t samples)
{
    AnimId id = anim_vmob_create(s);
    anim_phys_wave_set(s, id, p0, p1, amplitude, cycles, phase_rad, samples);
    anim_set_stroke(s, id, ANIM_TEAL, 3.6f, false);
    anim_set_glow(s, id, 0.55f, false);
    return id;
}

AnimId anim_phys_charge(AnimScene *s, AnimVec3 pos, float radius, int charge_sign)
{
    AnimId    grp  = anim_group(s, NULL, 0u);
    AnimId    disc = anim_circle(s, radius);
    AnimColor c_hi = (charge_sign > 0) ? ANIM_RED : (charge_sign < 0 ? ANIM_BLUE_C : ANIM_GREY_B);
    AnimColor c_lo = (charge_sign > 0) ? ANIM_MAROON : (charge_sign < 0 ? ANIM_BLUE_E : ANIM_GREY_D);
    float     bar  = radius * 0.48f;

    anim_move_to(s, disc, pos);
    anim_set_fill_gradient_radial(s, disc, c_hi, c_lo, pos, radius, false);
    anim_set_stroke(s, disc, ANIM_WHITE, 2.2f, false);
    anim_set_glow(s, disc, 0.7f, false);
    anim_add_child(s, grp, disc);

    if (charge_sign != 0)
    {
        AnimId hbar = anim_line(s, ANIM_V3(pos.x - bar, pos.y, pos.z), ANIM_V3(pos.x + bar, pos.y, pos.z));
        anim_set_stroke(s, hbar, ANIM_WHITE, 3.0f, false);
        anim_add_child(s, grp, hbar);
        if (charge_sign > 0)
        {
            AnimId vbar = anim_line(s, ANIM_V3(pos.x, pos.y - bar, pos.z), ANIM_V3(pos.x, pos.y + bar, pos.z));
            anim_set_stroke(s, vbar, ANIM_WHITE, 3.0f, false);
            anim_add_child(s, grp, vbar);
        }
    }
    return grp;
}

static AnimVec3 eval_efield(const AnimVec3 *cpos, const float *q, uint32_t n, AnimVec3 p)
{
    AnimVec3 e = ANIM_ORIGIN;
    uint32_t i;
    for (i = 0u; i < n; ++i)
    {
        float dx = p.x - cpos[i].x;
        float dy = p.y - cpos[i].y;
        float r2 = dx * dx + dy * dy + 0.02f;
        float r3 = r2 * sqrtf(r2);
        e.x += q[i] * dx / r3;
        e.y += q[i] * dy / r3;
    }
    return e;
}

AnimId anim_phys_field_lines(AnimScene *s, const AnimVec3 *charge_pos, const float *charges, uint32_t charge_count,
                             AnimVec3 bounds_min, AnimVec3 bounds_max, uint32_t lines_per_charge)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    uint32_t ci, li;
    uint32_t nl = MU_CLAMP(lines_per_charge, 4u, 24u);

    if (!charge_pos || !charges || charge_count == 0u)
        return grp;

    for (ci = 0u; ci < charge_count; ++ci)
    {
        float sign = (charges[ci] >= 0.0f) ? 1.0f : -1.0f;
        /* Avoid duplicate streamlines if both + and - charges exist: trace from + only unless all are -. */
        bool  has_pos = false;
        uint32_t k;
        for (k = 0u; k < charge_count; ++k)
            if (charges[k] > 0.0f)
                has_pos = true;
        if (has_pos && charges[ci] < 0.0f)
            continue;

        for (li = 0u; li < nl; ++li)
        {
            float    ang = ANIM_TAU * ((float)li / (float)nl);
            AnimVec3 p   = ANIM_V3(charge_pos[ci].x + 0.30f * cosf(ang),
                                   charge_pos[ci].y + 0.30f * sinf(ang),
                                   charge_pos[ci].z);
            AnimVec3 pts[96];
            uint32_t pn = 0u;
            uint32_t step;
            float    ds = 0.14f * sign;

            pts[pn++] = p;
            for (step = 0u; step < 90u; ++step)
            {
                AnimVec3 e1 = eval_efield(charge_pos, charges, charge_count, p);
                float    m1 = sqrtf(e1.x * e1.x + e1.y * e1.y);
                AnimVec3 mid, e2, nxt;
                float    m2;
                bool     hit = false;
                if (m1 < 1e-5f)
                    break;
                mid = ANIM_V3(p.x + 0.5f * ds * (e1.x / m1), p.y + 0.5f * ds * (e1.y / m1), p.z);
                e2  = eval_efield(charge_pos, charges, charge_count, mid);
                m2  = sqrtf(e2.x * e2.x + e2.y * e2.y);
                if (m2 < 1e-5f)
                    break;
                nxt = ANIM_V3(p.x + ds * (e2.x / m2), p.y + ds * (e2.y / m2), p.z);
                pts[pn++] = nxt;
                p = nxt;
                if (p.x < bounds_min.x || p.x > bounds_max.x || p.y < bounds_min.y || p.y > bounds_max.y)
                    break;
                for (k = 0u; k < charge_count; ++k)
                {
                    if (k == ci)
                        continue;
                    float dx = p.x - charge_pos[k].x;
                    float dy = p.y - charge_pos[k].y;
                    if (dx * dx + dy * dy < 0.08f)
                    {
                        hit = true;
                        break;
                    }
                }
                if (hit || pn >= 95u)
                    break;
            }
            if (pn >= 3u)
            {
                AnimId line = anim_vmob_create(s);
                append_polyline_subpath(s, line, pts, pn);
                anim_set_stroke(s, line, anim_hex_a(0x9CDCEB, 0.72f), 2.0f, false);
                anim_add_child(s, grp, line);

                /* Small directional chevron near midpoint */
                {
                    uint32_t mi = pn / 2u;
                    AnimVec3 a  = pts[mi - 1u];
                    AnimVec3 b  = pts[mi];
                    AnimVec3 d  = ANIM_V3((b.x - a.x) * sign, (b.y - a.y) * sign, 0.0f);
                    float    dl = sqrtf(d.x * d.x + d.y * d.y);
                    if (dl > 1e-4f)
                    {
                        AnimVec3 u  = ANIM_V3(d.x / dl, d.y / dl, 0.0f);
                        AnimVec3 pr = ANIM_V3(-u.y, u.x, 0.0f);
                        AnimId   ch = anim_vmob_create(s);
                        anim_vmob_begin_subpath(s, ch, ANIM_V3(b.x - 0.12f * u.x + 0.07f * pr.x, b.y - 0.12f * u.y + 0.07f * pr.y, b.z));
                        anim_vmob_line_to(s, ch, b);
                        anim_vmob_line_to(s, ch, ANIM_V3(b.x - 0.12f * u.x - 0.07f * pr.x, b.y - 0.12f * u.y - 0.07f * pr.y, b.z));
                        anim_set_stroke(s, ch, ANIM_BLUE_A, 2.2f, false);
                        anim_add_child(s, grp, ch);
                    }
                }
            }
        }
    }
    return grp;
}

AnimId anim_phys_lens(AnimScene *s, AnimVec3 center, float height, float thickness, bool diverging)
{
    AnimId id  = anim_vmob_create(s);
    float  hh  = 0.5f * height;
    float  bul = diverging ? -0.35f * thickness : thickness;
    float  rim = diverging ? 0.5f * thickness : 0.08f * thickness;

    anim_vmob_begin_subpath(s, id, ANIM_V3(center.x - rim, center.y + hh, center.z));
    anim_vmob_cubic_to(s, id,
                       ANIM_V3(center.x - rim + bul, center.y + 0.5f * hh, center.z),
                       ANIM_V3(center.x - rim + bul, center.y - 0.5f * hh, center.z),
                       ANIM_V3(center.x - rim, center.y - hh, center.z));
    anim_vmob_line_to(s, id, ANIM_V3(center.x + rim, center.y - hh, center.z));
    anim_vmob_cubic_to(s, id,
                       ANIM_V3(center.x + rim - bul, center.y - 0.5f * hh, center.z),
                       ANIM_V3(center.x + rim - bul, center.y + 0.5f * hh, center.z),
                       ANIM_V3(center.x + rim, center.y + hh, center.z));
    anim_vmob_close_subpath(s, id);
    anim_set_fill_gradient_linear(s, id, anim_hex_a(0x58C4DD, 0.35f), anim_hex_a(0xC7E9F1, 0.50f),
                                  ANIM_V3(center.x, center.y - hh, center.z),
                                  ANIM_V3(center.x, center.y + hh, center.z), false);
    anim_set_stroke(s, id, ANIM_BLUE_A, 2.6f, false);
    anim_set_cap_join(s, id, ANIM_CAP_ROUND, ANIM_JOIN_ROUND, false);
    anim_set_glow(s, id, 0.45f, false);
    return id;
}

AnimId anim_phys_ray_bundle(AnimScene *s, AnimVec3 lens_center, float focal_length, float lens_height,
                            const float *y_offsets, uint32_t ray_count, float x_start, float x_end)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    float    f   = (fabsf(focal_length) < 0.05f) ? 2.0f : focal_length;
    uint32_t i;

    /* Optical axis */
    {
        AnimId axis = anim_dashed_line(s, ANIM_V3(x_start, lens_center.y, lens_center.z),
                                       ANIM_V3(x_end, lens_center.y, lens_center.z), 0.16f, 0.10f);
        anim_set_stroke(s, axis, ANIM_GREY_C, 1.8f, false);
        anim_add_child(s, grp, axis);
    }

    for (i = 0u; i < ray_count; ++i)
    {
        float    dy   = y_offsets ? y_offsets[i] : (-0.38f * lens_height + 0.76f * lens_height * ((float)i / (float)MU_MAX(ray_count - 1u, 1u)));
        float    y_in = lens_center.y + dy;
        float    slope = -dy / f;
        float    y_end = y_in + slope * (x_end - lens_center.x);
        AnimId   ray   = anim_vmob_create(s);

        anim_vmob_begin_subpath(s, ray, ANIM_V3(x_start, y_in, lens_center.z));
        anim_vmob_line_to(s, ray, ANIM_V3(lens_center.x, y_in, lens_center.z));
        anim_vmob_line_to(s, ray, ANIM_V3(x_end, y_end, lens_center.z));
        anim_set_stroke(s, ray, ANIM_YELLOW, 2.4f, false);
        anim_set_glow(s, ray, 0.45f, false);
        anim_add_child(s, grp, ray);
    }
    return grp;
}

/* ============================================================================
 *  COMPUTER SCIENCE TOOLKIT
 * ==========================================================================*/

void anim_cs_swap_mobs(AnimTimeline *tl, AnimScene *scene, AnimId a, AnimId b, float arc_height, float duration)
{
    AnimVec3 ca = anim_center(scene, a);
    AnimVec3 cb = anim_center(scene, b);
    AnimSpec sp[2] = {
        anim_anim_move_arc(a, cb,  arc_height),
        anim_anim_move_arc(b, ca, -arc_height),
    };
    anim_play(tl, scene, sp, 2u, duration, ANIM_RATE_SMOOTH);
}

void anim_cs_bar_swap(AnimTimeline *tl, AnimScene *scene, AnimId *bars, uint32_t i, uint32_t j, float arc_height, float duration)
{
    AnimId   a = bars[i];
    AnimId   b = bars[j];
    AnimVec3 ca = anim_center(scene, a);
    AnimVec3 cb = anim_center(scene, b);
    AnimVec3 dst_a = ANIM_V3(cb.x, ca.y, ca.z);
    AnimVec3 dst_b = ANIM_V3(ca.x, cb.y, cb.z);
    AnimSpec sp[2] = {
        anim_anim_move_arc(a, dst_a,  arc_height),
        anim_anim_move_arc(b, dst_b, -arc_height),
    };
    anim_play(tl, scene, sp, 2u, duration, ANIM_RATE_SMOOTH);
    bars[i] = b;
    bars[j] = a;
}

AnimId anim_cs_array(AnimScene *s, const char *const *labels, uint32_t count, float cell_w, float cell_h,
                     bool show_indices, const char *font_path, AnimId *out_cells)
{
    AnimId   grp   = anim_group(s, NULL, 0u);
    float    tot_w = (float)count * cell_w;
    float    x0    = -0.5f * tot_w + 0.5f * cell_w;
    uint32_t i;

    for (i = 0u; i < count; ++i)
    {
        float    cx   = x0 + (float)i * cell_w;
        AnimId   cell = anim_group(s, NULL, 0u);
        AnimId   box  = anim_rounded_rect(s, cell_w - 0.04f, cell_h, 0.08f);
        const char *txt = (labels && labels[i]) ? labels[i] : "";
        AnimId   lbl  = anim_text(s, font_path, txt, cell_h * 0.42f);
        char     tag[32];

        anim_move_to(s, box, ANIM_V3(cx, 0.0f, 0.0f));
        anim_set_fill_gradient_linear(s, box, anim_hex_a(0x1B2B44, 0.92f), anim_hex_a(0x162235, 0.92f),
                                      ANIM_V3(cx, 0.5f * cell_h, 0.0f), ANIM_V3(cx, -0.5f * cell_h, 0.0f), false);
        anim_set_stroke(s, box, ANIM_BLUE_C, 2.2f, false);
        anim_set_z_index(s, box, -0.1f, false);
        anim_move_to(s, lbl, ANIM_V3(cx, 0.0f, 0.0f));
        anim_set_color(s, lbl, ANIM_WHITE, true);

        anim_add_child(s, cell, box);
        anim_add_child(s, cell, lbl);
        snprintf(tag, sizeof tag, "cell_%u", i);
        anim_set_tag(s, cell, tag);
        anim_add_child(s, grp, cell);
        if (out_cells)
            out_cells[i] = cell;

        if (show_indices)
        {
            char   ib[16];
            AnimId idx_lbl;
            snprintf(ib, sizeof ib, "%u", i);
            idx_lbl = anim_text(s, font_path, ib, cell_h * 0.25f);
            anim_set_color(s, idx_lbl, ANIM_GREY_C, true);
            anim_move_to(s, idx_lbl, ANIM_V3(cx, -0.5f * cell_h - 0.20f, 0.0f));
            anim_add_child(s, grp, idx_lbl);
        }
    }
    return grp;
}

AnimId anim_cs_bar_chart(AnimScene *s, const float *values, const char *const *labels, uint32_t count,
                         float total_w, float max_h, AnimColor c_low, AnimColor c_high, const char *font_path, AnimId *out_bars)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    float    vmax = 1e-4f;
    float    slot_w, bar_w;
    float    base_y = -0.5f * max_h;
    uint32_t i;

    if (count == 0u)
        return grp;
    for (i = 0u; i < count; ++i)
        if (values && values[i] > vmax)
            vmax = values[i];

    slot_w = total_w / (float)count;
    bar_w  = slot_w * 0.78f;

    /* Baseline */
    {
        AnimId base = anim_line(s, ANIM_V3(-0.5f * total_w - 0.1f, base_y, 0.0f),
                                ANIM_V3(0.5f * total_w + 0.1f, base_y, 0.0f));
        anim_set_stroke(s, base, ANIM_GREY_B, 2.2f, false);
        anim_add_child(s, grp, base);
    }

    for (i = 0u; i < count; ++i)
    {
        float    v   = values ? MU_MAX(values[i], 0.0f) : 0.5f;
        float    t   = MU_CLAMP(v / vmax, 0.0f, 1.0f);
        float    bh  = MU_MAX(t * max_h, 0.08f);
        float    cx  = -0.5f * total_w + ((float)i + 0.5f) * slot_w;
        float    cy  = base_y + 0.5f * bh;
        AnimId   bar_grp = anim_group(s, NULL, 0u);
        AnimId   rect    = anim_rounded_rect(s, bar_w, bh, MU_MIN(bar_w * 0.12f, 0.08f));
        AnimColor col    = anim_color_lerp_oklab(c_low, c_high, t);
        char     tag[32];

        anim_move_to(s, rect, ANIM_V3(cx, cy, 0.0f));
        anim_set_fill(s, rect, col, false);
        anim_set_stroke(s, rect, ANIM_WHITE, 1.6f, false);
        anim_add_child(s, bar_grp, rect);

        if (labels && labels[i] && labels[i][0])
        {
            AnimId lbl = anim_text(s, font_path, labels[i], MU_MIN(bar_w * 0.55f, 0.28f));
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_move_to(s, lbl, ANIM_V3(cx, cy, 0.0f));
            anim_add_child(s, bar_grp, lbl);
        }

        snprintf(tag, sizeof tag, "bar_%u", i);
        anim_set_tag(s, bar_grp, tag);
        anim_add_child(s, grp, bar_grp);
        if (out_bars)
            out_bars[i] = bar_grp;
    }
    return grp;
}

AnimId anim_cs_linked_list(AnimScene *s, const char *const *labels, uint32_t count, float node_w, float node_h,
                           float spacing, const char *font_path, AnimId *out_nodes)
{
    AnimId   grp   = anim_group(s, NULL, 0u);
    float    step  = node_w + spacing;
    float    tot_w = (count > 0u) ? ((float)(count - 1u) * step + node_w) : 0.0f;
    float    x0    = -0.5f * tot_w + 0.5f * node_w;
    uint32_t i;

    for (i = 0u; i < count; ++i)
    {
        float       cx   = x0 + (float)i * step;
        AnimId      node = anim_group(s, NULL, 0u);
        AnimId      box  = anim_rounded_rect(s, node_w, node_h, 0.08f);
        float       divx = cx + 0.18f * node_w;
        AnimId      div  = anim_line(s, ANIM_V3(divx, -0.5f * node_h, 0.0f), ANIM_V3(divx, 0.5f * node_h, 0.0f));
        AnimId      pt   = anim_dot(s, ANIM_V3(cx + 0.34f * node_w, 0.0f, 0.0f), 0.06f);
        const char *txt  = (labels && labels[i]) ? labels[i] : "";
        AnimId      lbl  = anim_text(s, font_path, txt, node_h * 0.42f);

        anim_move_to(s, box, ANIM_V3(cx, 0.0f, 0.0f));
        anim_set_fill_gradient_linear(s, box, anim_hex_a(0x1C2B45, 0.92f), anim_hex_a(0x172438, 0.92f),
                                      ANIM_V3(cx, 0.5f * node_h, 0.0f), ANIM_V3(cx, -0.5f * node_h, 0.0f), false);
        anim_set_stroke(s, box, ANIM_TEAL, 2.2f, false);
        anim_set_z_index(s, box, -0.1f, false);
        anim_set_stroke(s, div, ANIM_TEAL, 1.8f, false);
        anim_set_fill(s, pt, ANIM_GOLD, false);
        anim_move_to(s, lbl, ANIM_V3(cx - 0.16f * node_w, 0.0f, 0.0f));
        anim_set_color(s, lbl, ANIM_WHITE, true);

        anim_add_child(s, node, box);
        anim_add_child(s, node, div);
        anim_add_child(s, node, pt);
        anim_add_child(s, node, lbl);
        anim_add_child(s, grp, node);
        if (out_nodes)
            out_nodes[i] = node;

        {
            AnimVec3 p0 = ANIM_V3(cx + 0.34f * node_w, 0.0f, 0.0f);
            AnimVec3 p1 = (i + 1u < count)
                              ? ANIM_V3(cx + step - 0.5f * node_w - 0.02f, 0.0f, 0.0f)
                              : ANIM_V3(cx + 0.5f * node_w + 0.65f * spacing, 0.0f, 0.0f);
            AnimId arr = anim_arrow(s, p0, p1, 0.16f, 0.13f);
            anim_set_color(s, arr, ANIM_GOLD, true);
            anim_add_child(s, grp, arr);

            if (i + 1u == count)
            {
                AnimId nl = anim_text(s, font_path, "NULL", node_h * 0.32f);
                anim_set_color(s, nl, ANIM_GREY_C, true);
                anim_move_to(s, nl, ANIM_V3(p1.x + 0.36f, 0.0f, 0.0f));
                anim_add_child(s, grp, nl);
            }
        }
    }
    return grp;
}

AnimId anim_cs_stack_queue(AnimScene *s, const char *const *labels, uint32_t count, bool is_queue,
                           float item_w, float item_h, const char *font_path)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    uint32_t i;

    if (!is_queue)
    {
        float  cap_h = (float)MU_MAX(count, 3u) * (item_h + 0.08f) + 0.20f;
        float  hw    = 0.5f * item_w + 0.10f;
        float  yb    = -0.5f * cap_h;
        AnimId wall  = anim_vmob_create(s);
        anim_vmob_begin_subpath(s, wall, ANIM_V3(-hw, yb + cap_h, 0.0f));
        anim_vmob_line_to(s, wall, ANIM_V3(-hw, yb, 0.0f));
        anim_vmob_line_to(s, wall, ANIM_V3( hw, yb, 0.0f));
        anim_vmob_line_to(s, wall, ANIM_V3( hw, yb + cap_h, 0.0f));
        anim_set_stroke(s, wall, ANIM_GREY_B, 2.6f, false);
        anim_add_child(s, grp, wall);

        for (i = 0u; i < count; ++i)
        {
            float    cy  = yb + 0.10f + 0.5f * item_h + (float)i * (item_h + 0.08f);
            AnimId   bx  = anim_rounded_rect(s, item_w, item_h, 0.08f);
            AnimId   lbl = anim_text(s, font_path, (labels && labels[i]) ? labels[i] : "", item_h * 0.45f);
            anim_move_to(s, bx, ANIM_V3(0.0f, cy, 0.0f));
            anim_set_fill(s, bx, anim_hex_a(0x236B8E, 0.88f), false);
            anim_set_stroke(s, bx, ANIM_BLUE_B, 2.0f, false);
            anim_set_z_index(s, bx, -0.1f, false);
            anim_move_to(s, lbl, ANIM_V3(0.0f, cy, 0.0f));
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_add_child(s, grp, bx);
            anim_add_child(s, grp, lbl);
        }
        if (count > 0u)
        {
            float  top_y = yb + 0.10f + 0.5f * item_h + (float)(count - 1u) * (item_h + 0.08f);
            AnimId arr   = anim_arrow(s, ANIM_V3(hw + 0.65f, top_y, 0.0f), ANIM_V3(hw + 0.08f, top_y, 0.0f), 0.14f, 0.12f);
            AnimId tlbl  = anim_text(s, font_path, "TOP", item_h * 0.34f);
            anim_set_color(s, arr, ANIM_GOLD, true);
            anim_set_color(s, tlbl, ANIM_GOLD, true);
            anim_move_to(s, tlbl, ANIM_V3(hw + 1.05f, top_y, 0.0f));
            anim_add_child(s, grp, arr);
            anim_add_child(s, grp, tlbl);
        }
    }
    else
    {
        float  cap_w = (float)MU_MAX(count, 3u) * (item_w + 0.08f) + 0.25f;
        float  hh    = 0.5f * item_h + 0.10f;
        float  xl    = -0.5f * cap_w;
        AnimId top_l = anim_line(s, ANIM_V3(xl,  hh, 0.0f), ANIM_V3(xl + cap_w,  hh, 0.0f));
        AnimId bot_l = anim_line(s, ANIM_V3(xl, -hh, 0.0f), ANIM_V3(xl + cap_w, -hh, 0.0f));
        anim_set_stroke(s, top_l, ANIM_GREY_B, 2.6f, false);
        anim_set_stroke(s, bot_l, ANIM_GREY_B, 2.6f, false);
        anim_add_child(s, grp, top_l);
        anim_add_child(s, grp, bot_l);

        for (i = 0u; i < count; ++i)
        {
            float  cx  = xl + 0.14f + 0.5f * item_w + (float)i * (item_w + 0.08f);
            AnimId bx  = anim_rounded_rect(s, item_w, item_h, 0.08f);
            AnimId lbl = anim_text(s, font_path, (labels && labels[i]) ? labels[i] : "", item_h * 0.45f);
            anim_move_to(s, bx, ANIM_V3(cx, 0.0f, 0.0f));
            anim_set_fill(s, bx, anim_hex_a(0x2D5F4E, 0.88f), false);
            anim_set_stroke(s, bx, ANIM_TEAL, 2.0f, false);
            anim_set_z_index(s, bx, -0.1f, false);
            anim_move_to(s, lbl, ANIM_V3(cx, 0.0f, 0.0f));
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_add_child(s, grp, bx);
            anim_add_child(s, grp, lbl);
        }
    }
    return grp;
}

AnimId anim_cs_binary_tree(AnimScene *s, const char *const *heap_labels, uint32_t heap_size, float level_dy,
                           float root_spread, float node_r, const char *font_path, AnimId *out_nodes)
{
    AnimId    grp = anim_group(s, NULL, 0u);
    AnimVec3 *pos;
    uint32_t  i;

    if (heap_size == 0u)
        return grp;
    pos = (AnimVec3 *)calloc(heap_size, sizeof *pos);
    MU_ASSERT(pos != NULL);

    /* Compute heap-indexed positions */
    pos[0] = ANIM_V3(0.0f, 1.2f, 0.0f);
    for (i = 1u; i < heap_size; ++i)
    {
        uint32_t p     = (i - 1u) / 2u;
        uint32_t depth = 0u;
        uint32_t tmp   = i + 1u;
        float    dx;
        while (tmp > 1u)
        {
            tmp >>= 1u;
            depth++;
        }
        dx     = root_spread / powf(2.0f, (float)(depth > 0u ? depth - 1u : 0u));
        pos[i] = ANIM_V3(pos[p].x + ((i & 1u) ? -dx : dx), pos[p].y - level_dy, 0.0f);
    }

    /* Edges first (z_index -0.1) */
    for (i = 1u; i < heap_size; ++i)
    {
        uint32_t p = (i - 1u) / 2u;
        AnimVec3 d;
        float    len;
        if (!heap_labels || !heap_labels[i] || !heap_labels[p])
            continue;
        d   = ANIM_V3(pos[i].x - pos[p].x, pos[i].y - pos[p].y, 0.0f);
        len = sqrtf(d.x * d.x + d.y * d.y);
        if (len > 2.0f * node_r)
        {
            AnimVec3 u  = ANIM_V3(d.x / len, d.y / len, 0.0f);
            AnimVec3 a  = ANIM_V3(pos[p].x + u.x * node_r, pos[p].y + u.y * node_r, 0.0f);
            AnimVec3 b  = ANIM_V3(pos[i].x - u.x * node_r, pos[i].y - u.y * node_r, 0.0f);
            AnimId   ar = anim_arrow(s, a, b, 0.14f, 0.11f);
            anim_set_color(s, ar, ANIM_GREY_B, true);
            anim_set_z_index(s, ar, -0.1f, true);
            anim_add_child(s, grp, ar);
        }
    }

    /* Nodes */
    for (i = 0u; i < heap_size; ++i)
    {
        if (!heap_labels || !heap_labels[i])
        {
            if (out_nodes)
                out_nodes[i] = ANIM_ID_NIL;
            continue;
        }
        {
            AnimId node = anim_group(s, NULL, 0u);
            AnimId cir  = anim_circle(s, node_r);
            AnimId lbl  = anim_text(s, font_path, heap_labels[i], node_r * 0.90f);
            anim_move_to(s, cir, pos[i]);
            anim_set_fill_gradient_radial(s, cir, ANIM_BLUE_D, ANIM_BLUE_E, pos[i], node_r, false);
            anim_set_stroke(s, cir, ANIM_BLUE_A, 2.2f, false);
            anim_set_z_index(s, cir, -0.05f, false);
            anim_move_to(s, lbl, pos[i]);
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_add_child(s, node, cir);
            anim_add_child(s, node, lbl);
            anim_add_child(s, grp, node);
            if (out_nodes)
                out_nodes[i] = node;
        }
    }

    free(pos);
    return grp;
}

AnimId anim_cs_fsm(AnimScene *s, uint32_t state_count, const AnimVec3 *positions, const char *const *labels,
                   const bool *is_accept, uint32_t start_state, uint32_t trans_count, const uint32_t *from,
                   const uint32_t *to, const char *const *edge_labels, float node_r, const char *font_path)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    uint32_t i;

    if (state_count == 0u || !positions)
        return grp;

    /* Start arrow */
    if (start_state < state_count)
    {
        AnimVec3 p = positions[start_state];
        AnimId   st = anim_arrow(s, ANIM_V3(p.x - node_r - 0.75f, p.y, p.z),
                                 ANIM_V3(p.x - node_r - 0.03f, p.y, p.z), 0.16f, 0.13f);
        anim_set_color(s, st, ANIM_GOLD, true);
        anim_add_child(s, grp, st);
    }

    /* Transitions */
    for (i = 0u; i < trans_count; ++i)
    {
        uint32_t u = from ? from[i] : 0u;
        uint32_t v = to ? to[i] : 0u;
        if (u >= state_count || v >= state_count)
            continue;

        if (u == v)
        {
            /* Self-loop arc above the node */
            AnimVec3 p  = positions[u];
            AnimVec3 a  = ANIM_V3(p.x - 0.45f * node_r, p.y + 0.89f * node_r, p.z);
            AnimVec3 b  = ANIM_V3(p.x + 0.45f * node_r, p.y + 0.89f * node_r, p.z);
            AnimVec3 c1 = ANIM_V3(p.x - 0.85f * node_r, p.y + 2.35f * node_r, p.z);
            AnimVec3 c2 = ANIM_V3(p.x + 0.85f * node_r, p.y + 2.35f * node_r, p.z);
            AnimId   lp = anim_vmob_create(s);
            anim_vmob_begin_subpath(s, lp, a);
            anim_vmob_cubic_to(s, lp, c1, c2, b);
            anim_set_stroke(s, lp, ANIM_GREY_A, 2.2f, false);
            anim_add_child(s, grp, lp);

            /* Small arrow head at b */
            {
                AnimId head = anim_arrow(s, ANIM_V3(b.x - 0.08f, b.y + 0.14f, b.z), b, 0.14f, 0.11f);
                anim_set_color(s, head, ANIM_GREY_A, true);
                anim_add_child(s, grp, head);
            }
            if (edge_labels && edge_labels[i] && edge_labels[i][0])
            {
                AnimId el = anim_text(s, font_path, edge_labels[i], node_r * 0.65f);
                anim_set_color(s, el, ANIM_YELLOW, true);
                anim_move_to(s, el, ANIM_V3(p.x, p.y + 2.25f * node_r, p.z));
                anim_add_child(s, grp, el);
            }
        }
        else
        {
            AnimVec3 pa  = positions[u];
            AnimVec3 pb  = positions[v];
            AnimVec3 d   = ANIM_V3(pb.x - pa.x, pb.y - pa.y, 0.0f);
            float    len = sqrtf(d.x * d.x + d.y * d.y);
            if (len > 2.0f * node_r)
            {
                AnimVec3 dir  = ANIM_V3(d.x / len, d.y / len, 0.0f);
                AnimVec3 perp = ANIM_V3(-dir.y, dir.x, 0.0f);
                /* Check if reverse transition exists to offset curves cleanly */
                bool     has_rev = false;
                uint32_t k;
                for (k = 0u; k < trans_count; ++k)
                    if (from[k] == v && to[k] == u)
                        has_rev = true;
                {
                    float    bend = has_rev ? (0.36f * len * 0.45f) : 0.0f;
                    AnimVec3 p_start = ANIM_V3(pa.x + dir.x * node_r + perp.x * (bend * 0.25f),
                                               pa.y + dir.y * node_r + perp.y * (bend * 0.25f), 0.0f);
                    AnimVec3 p_end   = ANIM_V3(pb.x - dir.x * node_r + perp.x * (bend * 0.25f),
                                               pb.y - dir.y * node_r + perp.y * (bend * 0.25f), 0.0f);
                    AnimVec3 mid     = ANIM_V3(0.5f * (pa.x + pb.x) + perp.x * bend,
                                               0.5f * (pa.y + pb.y) + perp.y * bend, 0.0f);
                    if (has_rev)
                    {
                        AnimId arc = anim_vmob_create(s);
                        AnimId hd;
                        anim_vmob_begin_subpath(s, arc, p_start);
                        anim_vmob_quad_to(s, arc, mid, p_end);
                        anim_set_stroke(s, arc, ANIM_GREY_A, 2.2f, false);
                        anim_add_child(s, grp, arc);

                        {
                            AnimVec3 td = ANIM_V3(p_end.x - mid.x, p_end.y - mid.y, 0.0f);
                            float    tl = sqrtf(td.x * td.x + td.y * td.y);
                            if (tl > 1e-4f)
                            {
                                hd = anim_arrow(s, ANIM_V3(p_end.x - 0.16f * (td.x / tl), p_end.y - 0.16f * (td.y / tl), 0.0f),
                                                p_end, 0.14f, 0.11f);
                                anim_set_color(s, hd, ANIM_GREY_A, true);
                                anim_add_child(s, grp, hd);
                            }
                        }
                    }
                    else
                    {
                        AnimId ar = anim_arrow(s, p_start, p_end, 0.15f, 0.12f);
                        anim_set_color(s, ar, ANIM_GREY_A, true);
                        anim_add_child(s, grp, ar);
                    }

                    if (edge_labels && edge_labels[i] && edge_labels[i][0])
                    {
                        AnimId el = anim_text(s, font_path, edge_labels[i], node_r * 0.65f);
                        float  lo = has_rev ? (bend * 0.72f + 0.18f) : 0.24f;
                        anim_set_color(s, el, ANIM_YELLOW, true);
                        anim_move_to(s, el, ANIM_V3(0.5f * (pa.x + pb.x) + perp.x * lo,
                                                    0.5f * (pa.y + pb.y) + perp.y * lo, 0.0f));
                        anim_add_child(s, grp, el);
                    }
                }
            }
        }
    }

    /* State circles */
    for (i = 0u; i < state_count; ++i)
    {
        AnimId cir = anim_circle(s, node_r);
        anim_move_to(s, cir, positions[i]);
        anim_set_fill_gradient_radial(s, cir, ANIM_BLUE_E, anim_hex(0x132033), positions[i], node_r, false);
        anim_set_stroke(s, cir, ANIM_BLUE_C, 2.5f, false);
        anim_set_z_index(s, cir, -0.05f, false);
        anim_add_child(s, grp, cir);

        if (is_accept && is_accept[i])
        {
            AnimId inner = anim_circle(s, node_r * 0.80f);
            anim_move_to(s, inner, positions[i]);
            anim_set_fill(s, inner, anim_rgba(0, 0, 0, 0), false);
            anim_set_stroke(s, inner, ANIM_TEAL, 2.0f, false);
            anim_add_child(s, grp, inner);
        }
        if (labels && labels[i] && labels[i][0])
        {
            AnimId lbl = anim_text(s, font_path, labels[i], node_r * 0.78f);
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_move_to(s, lbl, positions[i]);
            anim_add_child(s, grp, lbl);
        }
    }
    return grp;
}

AnimId anim_cs_logic_gate(AnimScene *s, AnimLogicGateKind kind, AnimVec3 pos, float scale, const char *label, const char *font_path)
{
    AnimId grp  = anim_group(s, NULL, 0u);
    AnimId body = anim_vmob_create(s);
    float  sc   = (scale > 1e-3f) ? scale : 1.0f;
    bool   inv  = (kind == ANIM_GATE_NOT || kind == ANIM_GATE_NAND || kind == ANIM_GATE_NOR);

    if (kind == ANIM_GATE_NOT)
    {
        anim_vmob_begin_subpath(s, body, ANIM_V3(-0.45f * sc,  0.42f * sc, 0.0f));
        anim_vmob_line_to(s, body, ANIM_V3(-0.45f * sc, -0.42f * sc, 0.0f));
        anim_vmob_line_to(s, body, ANIM_V3( 0.35f * sc,  0.00f, 0.0f));
        anim_vmob_close_subpath(s, body);
    }
    else if (kind == ANIM_GATE_AND || kind == ANIM_GATE_NAND)
    {
        anim_vmob_begin_subpath(s, body, ANIM_V3(-0.50f * sc,  0.45f * sc, 0.0f));
        anim_vmob_line_to(s, body, ANIM_V3( 0.05f * sc,  0.45f * sc, 0.0f));
        anim_vmob_cubic_to(s, body,
                           ANIM_V3( 0.62f * sc,  0.45f * sc, 0.0f),
                           ANIM_V3( 0.62f * sc, -0.45f * sc, 0.0f),
                           ANIM_V3( 0.05f * sc, -0.45f * sc, 0.0f));
        anim_vmob_line_to(s, body, ANIM_V3(-0.50f * sc, -0.45f * sc, 0.0f));
        anim_vmob_close_subpath(s, body);
    }
    else /* OR / NOR / XOR */
    {
        anim_vmob_begin_subpath(s, body, ANIM_V3(-0.52f * sc,  0.45f * sc, 0.0f));
        anim_vmob_cubic_to(s, body,
                           ANIM_V3( 0.00f,       0.45f * sc, 0.0f),
                           ANIM_V3( 0.35f * sc,  0.28f * sc, 0.0f),
                           ANIM_V3( 0.52f * sc,  0.00f,      0.0f));
        anim_vmob_cubic_to(s, body,
                           ANIM_V3( 0.35f * sc, -0.28f * sc, 0.0f),
                           ANIM_V3( 0.00f,      -0.45f * sc, 0.0f),
                           ANIM_V3(-0.52f * sc, -0.45f * sc, 0.0f));
        anim_vmob_quad_to(s, body, ANIM_V3(-0.26f * sc, 0.0f, 0.0f), ANIM_V3(-0.52f * sc, 0.45f * sc, 0.0f));
        anim_vmob_close_subpath(s, body);

        if (kind == ANIM_GATE_XOR)
        {
            AnimId xor_arc = anim_vmob_create(s);
            anim_vmob_begin_subpath(s, xor_arc, ANIM_V3(-0.64f * sc,  0.45f * sc, 0.0f));
            anim_vmob_quad_to(s, xor_arc, ANIM_V3(-0.38f * sc, 0.0f, 0.0f), ANIM_V3(-0.64f * sc, -0.45f * sc, 0.0f));
            anim_set_stroke(s, xor_arc, ANIM_BLUE_A, 2.4f, false);
            anim_add_child(s, grp, xor_arc);
        }
    }

    anim_set_fill_gradient_linear(s, body, anim_hex_a(0x1D304D, 0.92f), anim_hex_a(0x16243A, 0.92f),
                                  ANIM_V3(-0.5f * sc, 0.45f * sc, 0.0f), ANIM_V3(0.5f * sc, -0.45f * sc, 0.0f), false);
    anim_set_stroke(s, body, ANIM_BLUE_B, 2.4f, false);
    anim_add_child(s, grp, body);

    /* Input/Output pins */
    if (kind == ANIM_GATE_NOT)
    {
        AnimId in_l = anim_line(s, ANIM_V3(-0.85f * sc, 0.0f, 0.0f), ANIM_V3(-0.45f * sc, 0.0f, 0.0f));
        anim_set_stroke(s, in_l, ANIM_GREY_A, 2.2f, false);
        anim_add_child(s, grp, in_l);
    }
    else
    {
        float  x_in = (kind == ANIM_GATE_AND || kind == ANIM_GATE_NAND) ? -0.50f * sc : -0.42f * sc;
        AnimId in1  = anim_line(s, ANIM_V3(-0.85f * sc,  0.22f * sc, 0.0f), ANIM_V3(x_in,  0.22f * sc, 0.0f));
        AnimId in2  = anim_line(s, ANIM_V3(-0.85f * sc, -0.22f * sc, 0.0f), ANIM_V3(x_in, -0.22f * sc, 0.0f));
        anim_set_stroke(s, in1, ANIM_GREY_A, 2.2f, false);
        anim_set_stroke(s, in2, ANIM_GREY_A, 2.2f, false);
        anim_add_child(s, grp, in1);
        anim_add_child(s, grp, in2);
    }

    {
        float  x_tip = (kind == ANIM_GATE_NOT) ? 0.35f * sc : 0.50f * sc;
        float  x_out = x_tip + (inv ? 0.16f * sc : 0.0f);
        AnimId out_l = anim_line(s, ANIM_V3(x_out, 0.0f, 0.0f), ANIM_V3(0.88f * sc, 0.0f, 0.0f));
        anim_set_stroke(s, out_l, ANIM_TEAL, 2.2f, false);
        anim_add_child(s, grp, out_l);
        if (inv)
        {
            AnimId bub = anim_circle(s, 0.08f * sc);
            anim_move_to(s, bub, ANIM_V3(x_tip + 0.08f * sc, 0.0f, 0.0f));
            anim_set_fill(s, bub, anim_hex(0x0F111A), false);
            anim_set_stroke(s, bub, ANIM_BLUE_A, 2.0f, false);
            anim_add_child(s, grp, bub);
        }
    }

    if (label && label[0])
    {
        AnimId lbl = anim_text(s, font_path, label, 0.24f * sc);
        anim_set_color(s, lbl, ANIM_WHITE, true);
        anim_move_to(s, lbl, ANIM_V3(-0.08f * sc, 0.0f, 0.0f));
        anim_add_child(s, grp, lbl);
    }

    anim_move_to(s, grp, pos);
    return grp;
}

AnimId anim_cs_bit_register(AnimScene *s, uint64_t value, uint32_t bit_count, float bit_w, bool show_indices,
                            const char *font_path, AnimId *out_bits)
{
    uint32_t n = MU_CLAMP(bit_count, 1u, 32u);
    const char *lbls[32];
    char        buf[32][4];
    AnimId      cells[32];
    AnimId      grp;
    uint32_t    i;

    for (i = 0u; i < n; ++i)
    {
        uint32_t bit_idx = n - 1u - i;
        uint32_t bit     = (uint32_t)((value >> bit_idx) & 1ull);
        snprintf(buf[i], sizeof buf[i], "%u", bit);
        lbls[i] = buf[i];
    }
    grp = anim_cs_array(s, lbls, n, bit_w, bit_w * 0.95f, false, font_path, cells);
    for (i = 0u; i < n; ++i)
    {
        uint32_t bit_idx = n - 1u - i;
        uint32_t bit     = (uint32_t)((value >> bit_idx) & 1ull);
        AnimId   box     = anim_child_at(s, cells[i], 0u);
        if (bit)
        {
            anim_set_fill_gradient_linear(s, box, anim_hex_a(0x2D6D5E, 0.95f), anim_hex_a(0x1E4B42, 0.95f),
                                          ANIM_ORIGIN, ANIM_DOWN, false);
            anim_set_stroke(s, box, ANIM_TEAL, 2.2f, false);
        }
        else
        {
            anim_set_fill(s, box, anim_hex_a(0x171D2B, 0.85f), false);
            anim_set_stroke(s, box, ANIM_GREY_D, 1.8f, false);
        }
        if (out_bits)
            out_bits[i] = cells[i];

        if (show_indices)
        {
            char     ib[16];
            AnimId   il;
            AnimVec3 c = anim_center(s, cells[i]);
            snprintf(ib, sizeof ib, "b%u", bit_idx);
            il = anim_text(s, font_path, ib, bit_w * 0.24f);
            anim_set_color(s, il, ANIM_GREY_C, true);
            anim_move_to(s, il, ANIM_V3(c.x, c.y - 0.5f * bit_w - 0.22f, 0.0f));
            anim_add_child(s, grp, il);
        }
    }
    return grp;
}

AnimId anim_cs_neural_net(AnimScene *s, const uint32_t *layer_sizes, uint32_t layer_count, float width, float height,
                          float node_r, AnimColor c_in, AnimColor c_out)
{
    AnimId    grp = anim_group(s, NULL, 0u);
    uint32_t  L   = MU_CLAMP(layer_count, 1u, 16u);
    AnimVec3 *pos[16];
    uint32_t  li, ni, nj;

    if (!layer_sizes || layer_count == 0u)
        return grp;

    for (li = 0u; li < L; ++li)
    {
        uint32_t cnt = MU_MAX(layer_sizes[li], 1u);
        float    x   = (L == 1u) ? 0.0f : (-0.5f * width + ((float)li / (float)(L - 1u)) * width);
        float    dy  = (cnt == 1u) ? 0.0f : (height / (float)(cnt - 1u));
        pos[li] = (AnimVec3 *)malloc((size_t)cnt * sizeof(AnimVec3));
        MU_ASSERT(pos[li] != NULL);
        for (ni = 0u; ni < cnt; ++ni)
        {
            float y = (cnt == 1u) ? 0.0f : (0.5f * height - (float)ni * dy);
            pos[li][ni] = ANIM_V3(x, y, 0.0f);
        }
    }

    /* Edges */
    for (li = 0u; li + 1u < L; ++li)
    {
        uint32_t c0 = MU_MAX(layer_sizes[li], 1u);
        uint32_t c1 = MU_MAX(layer_sizes[li + 1u], 1u);
        for (ni = 0u; ni < c0; ++ni)
        {
            for (nj = 0u; nj < c1; ++nj)
            {
                AnimId ln = anim_line(s, pos[li][ni], pos[li + 1u][nj]);
                anim_set_stroke(s, ln, anim_hex_a(0x88A4C4, 0.38f), 1.6f, false);
                anim_set_z_index(s, ln, -0.2f, false);
                anim_add_child(s, grp, ln);
            }
        }
    }

    /* Neurons */
    for (li = 0u; li < L; ++li)
    {
        uint32_t  cnt = MU_MAX(layer_sizes[li], 1u);
        float     t   = (L == 1u) ? 0.0f : ((float)li / (float)(L - 1u));
        AnimColor col = anim_color_lerp_oklab(c_in, c_out, t);
        for (ni = 0u; ni < cnt; ++ni)
        {
            AnimId nd = anim_circle(s, node_r);
            anim_move_to(s, nd, pos[li][ni]);
            anim_set_fill(s, nd, col, false);
            anim_set_stroke(s, nd, ANIM_WHITE, 1.8f, false);
            anim_set_glow(s, nd, 0.55f, false);
            anim_add_child(s, grp, nd);
        }
        free(pos[li]);
    }

    return grp;
}
