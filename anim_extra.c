#include "anim_internal.h"

#define ANIM_CLEAR anim_rgba(0.0f, 0.0f, 0.0f, 0.0f)
#define ANIM_RGBA  anim_rgba
#define ANIM_CYAN  ANIM_TEAL
#define ANIM_ORANGE ANIM_GOLD
#define ANIM_CORAL  ANIM_RED

/* --------------------------------------------------------- Geometry Helpers */

static AnimVec3 v3_add(AnimVec3 a, AnimVec3 b) { return ANIM_V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static AnimVec3 v3_sub(AnimVec3 a, AnimVec3 b) { return ANIM_V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static AnimVec3 v3_mul(AnimVec3 a, float s)    { return ANIM_V3(a.x * s, a.y * s, a.z * s); }
static float    v3_dot(AnimVec3 a, AnimVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float    v3_len(AnimVec3 a)             { return sqrtf(v3_dot(a, a)); }

static AnimVec3 v3_cross(AnimVec3 a, AnimVec3 b)
{
    return ANIM_V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static AnimVec3 v3_norm(AnimVec3 a)
{
    float l = v3_len(a);
    return (l > 1e-6f) ? v3_mul(a, 1.0f / l) : ANIM_V3(0.0f, 1.0f, 0.0f);
}

/* ----------------------------------------------------- Shape Matchers & Arc */

AnimId anim_cubic_bezier(AnimScene *s, AnimVec3 p0, AnimVec3 p1, AnimVec3 p2, AnimVec3 p3)
{
    AnimId id = anim_vmob_create(s);
    if (!ANIM_ID_VALID(id))
        return ANIM_ID_NIL;
    anim_vmob_begin_subpath(s, id, p0);
    anim_vmob_cubic_to(s, id, p1, p2, p3);
    anim_set_stroke(s, id, ANIM_WHITE, 3.0f, false);
    anim_set_fill(s, id, ANIM_CLEAR, false);
    return id;
}

static void append_arc_cubics(AnimScene *s, AnimId id, AnimVec3 center, float radius,
                              float start_angle, float sweep_angle, bool new_subpath)
{
    uint32_t segs;
    float    dth;
    float    k;
    uint32_t i;

    if (fabsf(sweep_angle) < 1e-6f)
        return;
    segs = (uint32_t)MU_MAX(ceilf(fabsf(sweep_angle) / (ANIM_PI * 0.25f)), 1.0f);
    dth  = sweep_angle / (float)segs;
    k    = (4.0f / 3.0f) * tanf(0.25f * dth);

    if (new_subpath)
    {
        AnimVec3 p0 = ANIM_V3(center.x + radius * cosf(start_angle),
                              center.y + radius * sinf(start_angle), center.z);
        anim_vmob_begin_subpath(s, id, p0);
    }
    for (i = 0u; i < segs; ++i)
    {
        float    a0 = start_angle + (float)i * dth;
        float    a1 = a0 + dth;
        float    c0 = cosf(a0), s0 = sinf(a0);
        float    c1 = cosf(a1), s1 = sinf(a1);
        AnimVec3 cp1 = ANIM_V3(center.x + radius * (c0 - k * s0), center.y + radius * (s0 + k * c0), center.z);
        AnimVec3 cp2 = ANIM_V3(center.x + radius * (c1 + k * s1), center.y + radius * (s1 - k * c1), center.z);
        AnimVec3 end = ANIM_V3(center.x + radius * c1, center.y + radius * s1, center.z);
        anim_vmob_cubic_to(s, id, cp1, cp2, end);
    }
}

AnimId anim_arc_between_points(AnimScene *s, AnimVec3 a, AnimVec3 b, float angle_rad)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float d  = sqrtf(dx * dx + dy * dy);
    if (d < 1e-5f || fabsf(angle_rad) < 1e-4f)
        return anim_line(s, a, b);

    {
        float    half   = 0.5f * angle_rad;
        float    sin_h  = sinf(half);
        float    cos_h  = cosf(half);
        float    radius = fabsf(0.5f * d / sin_h);
        float    mx     = 0.5f * (a.x + b.x);
        float    my     = 0.5f * (a.y + b.y);
        float    nx     = -dy / d;
        float    ny     = dx / d;
        float    h_off  = (0.5f * d) * (cos_h / sin_h);
        AnimVec3 center = ANIM_V3(mx + nx * h_off, my + ny * h_off, 0.5f * (a.z + b.z));
        float    a0     = atan2f(a.y - center.y, a.x - center.x);
        AnimId   id     = anim_vmob_create(s);
        if (!ANIM_ID_VALID(id))
            return ANIM_ID_NIL;
        append_arc_cubics(s, id, center, radius, a0, angle_rad, true);
        anim_set_stroke(s, id, ANIM_WHITE, 3.0f, false);
        anim_set_fill(s, id, ANIM_CLEAR, false);
        return id;
    }
}

AnimId anim_sector(AnimScene *s, float r_inner, float r_outer, float start_angle, float angle)
{
    AnimId id = anim_vmob_create(s);
    float  ri = MU_MAX(r_inner, 0.0f);
    float  ro = MU_MAX(r_outer, ri + 1e-3f);
    if (!ANIM_ID_VALID(id))
        return ANIM_ID_NIL;

    if (ri < 1e-4f)
    {
        AnimVec3 p0 = ANIM_V3(ro * cosf(start_angle), ro * sinf(start_angle), 0.0f);
        anim_vmob_begin_subpath(s, id, ANIM_ORIGIN);
        anim_vmob_line_to(s, id, p0);
        append_arc_cubics(s, id, ANIM_ORIGIN, ro, start_angle, angle, false);
        anim_vmob_close_subpath(s, id);
    }
    else
    {
        float    a1 = start_angle + angle;
        AnimVec3 i1 = ANIM_V3(ri * cosf(a1), ri * sinf(a1), 0.0f);
        append_arc_cubics(s, id, ANIM_ORIGIN, ro, start_angle, angle, true);
        anim_vmob_line_to(s, id, i1);
        append_arc_cubics(s, id, ANIM_ORIGIN, ri, a1, -angle, false);
        anim_vmob_close_subpath(s, id);
    }
    anim_set_fill(s, id, ANIM_BLUE_C, false);
    anim_set_stroke(s, id, ANIM_WHITE, 2.0f, false);
    return id;
}

AnimId anim_angle(AnimScene *s, AnimVec3 a_start, AnimVec3 vertex, AnimVec3 b_end, float radius)
{
    float  a0   = atan2f(a_start.y - vertex.y, a_start.x - vertex.x);
    float  a1   = atan2f(b_end.y - vertex.y, b_end.x - vertex.x);
    float  diff = a1 - a0;
    AnimId id;

    while (diff > ANIM_PI)  diff -= ANIM_TAU;
    while (diff < -ANIM_PI) diff += ANIM_TAU;
    id = anim_vmob_create(s);
    if (!ANIM_ID_VALID(id))
        return ANIM_ID_NIL;
    append_arc_cubics(s, id, vertex, MU_MAX(radius, 0.05f), a0, diff, true);
    anim_set_stroke(s, id, ANIM_YELLOW, 2.5f, false);
    anim_set_fill(s, id, ANIM_CLEAR, false);
    return id;
}

AnimId anim_right_angle(AnimScene *s, AnimVec3 a_start, AnimVec3 vertex, AnimVec3 b_end, float length)
{
    AnimVec3 u  = v3_norm(v3_sub(a_start, vertex));
    AnimVec3 v  = v3_norm(v3_sub(b_end, vertex));
    float    l  = MU_MAX(length, 0.05f);
    AnimVec3 p1 = v3_add(vertex, v3_mul(u, l));
    AnimVec3 p2 = v3_add(p1, v3_mul(v, l));
    AnimVec3 p3 = v3_add(vertex, v3_mul(v, l));
    AnimId   id = anim_vmob_create(s);
    if (!ANIM_ID_VALID(id))
        return ANIM_ID_NIL;
    anim_vmob_begin_subpath(s, id, p1);
    anim_vmob_line_to(s, id, p2);
    anim_vmob_line_to(s, id, p3);
    anim_set_stroke(s, id, ANIM_YELLOW, 2.5f, false);
    anim_set_fill(s, id, ANIM_CLEAR, false);
    return id;
}

AnimId anim_surrounding_rect(AnimScene *s, AnimId target, float buff, float corner_radius)
{
    AnimVec3 mn, mx;
    float    w, h;
    AnimId   r;

    anim_bbox_world(s, target, &mn, &mx);
    w = MU_MAX((mx.x - mn.x) + 2.0f * buff, 0.05f);
    h = MU_MAX((mx.y - mn.y) + 2.0f * buff, 0.05f);
    r = (corner_radius > 1e-4f) ? anim_rounded_rect(s, w, h, corner_radius) : anim_rect(s, w, h);
    anim_move_to(s, r, ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), mx.z));
    anim_set_fill(s, r, ANIM_CLEAR, false);
    anim_set_stroke(s, r, ANIM_YELLOW, 3.0f, false);
    return r;
}

AnimId anim_background_rect(AnimScene *s, AnimId target, float buff, AnimColor color)
{
    AnimId         r = anim_surrounding_rect(s, target, buff, 0.08f);
    const VMobHot *th = anim__hot_const(s, target);
    float          tz = th ? th->z_index : 0.0f;
    anim_set_fill(s, r, color.a > 0.0f ? color : ANIM_RGBA(0.06f, 0.07f, 0.11f, 0.85f), false);
    anim_set_stroke(s, r, ANIM_CLEAR, 0.0f, false);
    anim_set_z_index(s, r, tz - 0.1f, false);
    return r;
}

AnimId anim_underline(AnimScene *s, AnimId target, float buff)
{
    AnimVec3 mn, mx;
    float    y;
    AnimId   ln;

    anim_bbox_world(s, target, &mn, &mx);
    y  = mn.y - buff;
    ln = anim_line(s, ANIM_V3(mn.x, y, mx.z), ANIM_V3(mx.x, y, mx.z));
    anim_set_stroke(s, ln, ANIM_YELLOW, 3.0f, false);
    return ln;
}

AnimId anim_cross(AnimScene *s, AnimId target, float stroke_width)
{
    AnimVec3 mn, mx;
    AnimId   l1, l2, grp;
    AnimId   kids[2];
    float    sw = stroke_width > 0.0f ? stroke_width : 5.0f;

    anim_bbox_world(s, target, &mn, &mx);
    l1 = anim_line(s, ANIM_V3(mn.x, mn.y, mx.z), ANIM_V3(mx.x, mx.y, mx.z));
    l2 = anim_line(s, ANIM_V3(mn.x, mx.y, mx.z), ANIM_V3(mx.x, mn.y, mx.z));
    anim_set_stroke(s, l1, ANIM_RED, sw, false);
    anim_set_stroke(s, l2, ANIM_RED, sw, false);
    kids[0] = l1;
    kids[1] = l2;
    grp = anim_group(s, kids, 2u);
    anim_set_z_index(s, grp, 10.0f, true);
    return grp;
}

/* -------------------------------------------------------------- TracedPath */

static void traced_path_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    VMobHot  *ph = anim__hot(s, target);
    VMobCold *pc = anim__cold(s, target);
    AnimId    src_id;
    AnimVec3  pt;
    (void)dt;
    (void)user;

    if (!ph || !pc || !pc->is_traced_path)
        return;
    src_id = (AnimId){ pc->traced_target_idx, pc->traced_target_gen };
    if (!anim_vmob_alive(s, src_id))
        return;
    pt = anim_center(s, src_id);

    if (ph->subpath_count == 0u)
    {
        /* First sample: store anchor in fill_grad_p0 and open subpath */
        pc->fill_grad_p0 = pt;
        anim_vmob_begin_subpath(s, target, pt);
        return;
    }

    {
        SubpathMeta *sm = &s->subpaths[ph->subpath_offset];
        AnimVec3     prev = (sm->cubic_count == 0u)
                                ? pc->fill_grad_p0
                                : s->cubics[sm->cubic_offset + sm->cubic_count - 1u].p3;
        float dx = pt.x - prev.x;
        float dy = pt.y - prev.y;
        float dz = pt.z - prev.z;
        if (dx * dx + dy * dy + dz * dz < 1e-6f)
            return;

        if (sm->cubic_count < pc->traced_max_pts)
        {
            AnimCubic c;
            c.p0 = prev;
            c.p1 = v3_add(prev, v3_mul(v3_sub(pt, prev), 1.0f / 3.0f));
            c.p2 = v3_add(prev, v3_mul(v3_sub(pt, prev), 2.0f / 3.0f));
            c.p3 = pt;
            anim_vmob_add_cubic(s, target, c, false);
        }
        else if (sm->cubic_count > 1u)
        {
            uint32_t  off = sm->cubic_offset;
            uint32_t  cnt = sm->cubic_count;
            AnimCubic c;
            memmove(&s->cubics[off], &s->cubics[off + 1u], (size_t)(cnt - 1u) * sizeof(AnimCubic));
            c.p0 = prev;
            c.p1 = v3_add(prev, v3_mul(v3_sub(pt, prev), 1.0f / 3.0f));
            c.p2 = v3_add(prev, v3_mul(v3_sub(pt, prev), 2.0f / 3.0f));
            c.p3 = pt;
            s->cubics[off + cnt - 1u] = c;
        }
    }
}

AnimId anim_traced_path(AnimScene *s, AnimId target, AnimColor color, float stroke_width, uint32_t max_points)
{
    AnimId    path_id = anim_vmob_create(s);
    VMobCold *pc      = anim__cold(s, path_id);
    if (!ANIM_ID_VALID(path_id) || !pc || !anim_vmob_alive(s, target))
        return ANIM_ID_NIL;

    pc->is_traced_path    = true;
    pc->traced_target_idx = target.index;
    pc->traced_target_gen = target.gen;
    pc->traced_max_pts    = MU_CLAMP(max_points > 0u ? max_points : 256u, 8u, 4096u);
    s->has_traced_paths   = true;

    anim_set_stroke(s, path_id, color.a > 0.0f ? color : ANIM_YELLOW, stroke_width > 0.0f ? stroke_width : 2.5f, false);
    anim_set_fill(s, path_id, ANIM_CLEAR, false);
    anim_add_updater(s, path_id, traced_path_updater, NULL, &target, 1u, NULL, 0u);
    return path_id;
}

/* ----------------------------------------------------- 2D Polygon CSG Ops */

typedef struct Poly2Pt {
    float x, y;
} Poly2Pt;

typedef struct Poly2Loop {
    Poly2Pt *pts;
    uint32_t count;
    uint32_t cap;
} Poly2Loop;

typedef struct Poly2List {
    Poly2Loop *loops;
    uint32_t   count;
    uint32_t   cap;
} Poly2List;

typedef struct Seg2 {
    float x0, y0, x1, y1;
    bool  used;
} Seg2;

static void poly2_loop_push(Poly2Loop *lp, float x, float y)
{
    if (lp->count > 0u)
    {
        Poly2Pt last = lp->pts[lp->count - 1u];
        if ((x - last.x) * (x - last.x) + (y - last.y) * (y - last.y) < 1e-10f)
            return;
    }
    if (lp->count >= lp->cap)
    {
        lp->cap = lp->cap ? lp->cap * 2u : 32u;
        lp->pts = (Poly2Pt *)realloc(lp->pts, (size_t)lp->cap * sizeof *lp->pts);
    }
    lp->pts[lp->count++] = (Poly2Pt){ x, y };
}

static void poly2_list_free(Poly2List *pl)
{
    uint32_t i;
    for (i = 0u; i < pl->count; ++i)
        free(pl->loops[i].pts);
    free(pl->loops);
    pl->loops = NULL;
    pl->count = pl->cap = 0u;
}

static void collect_world_polys(AnimScene *s, AnimId root, Poly2List *out)
{
    uint32_t need = anim__count_subtree(s, root);
    AnimId  *mobs = (AnimId *)malloc((size_t)MU_MAX(need, 1u) * sizeof *mobs);
    uint32_t cnt = 0u, mi;

    anim__sync_world_mats(s);
    anim__collect_subtree(s, root, mobs, &cnt, need);
    for (mi = 0u; mi < cnt; ++mi)
    {
        const VMobHot *hm = anim__hot_const(s, mobs[mi]);
        uint32_t si;
        if (!hm || hm->subpath_count == 0u)
            continue;
        for (si = 0u; si < hm->subpath_count; ++si)
        {
            const SubpathMeta *sm = &s->subpaths[hm->subpath_offset + si];
            Poly2Loop lp = { 0 };
            uint32_t ci;
            if (sm->cubic_count == 0u)
                continue;
            for (ci = 0u; ci < sm->cubic_count; ++ci)
            {
                AnimCubic c = s->cubics[sm->cubic_offset + ci];
                uint32_t  k, steps = 8u;
                for (k = 0u; k < steps; ++k)
                {
                    float    t = (float)k / (float)steps;
                    AnimVec3 p;
                    float    pin[3], pout[3];
                    anim__cubic_eval(c, t, &p);
                    pin[0] = p.x; pin[1] = p.y; pin[2] = p.z;
                    mu_mat4_transform_point(pout, hm->world_mat, pin);
                    poly2_loop_push(&lp, pout[0], pout[1]);
                }
            }
            if (lp.count > 1u)
            {
                Poly2Pt f = lp.pts[0], l = lp.pts[lp.count - 1u];
                if ((f.x - l.x) * (f.x - l.x) + (f.y - l.y) * (f.y - l.y) < 1e-10f)
                    lp.count--;
            }
            if (lp.count >= 3u)
            {
                /* Ensure CCW positive signed area so boundary normals are consistent */
                double area = 0.0;
                uint32_t k;
                for (k = 0u; k < lp.count; ++k)
                {
                    Poly2Pt a = lp.pts[k];
                    Poly2Pt b = lp.pts[(k + 1u) % lp.count];
                    area += (double)a.x * (double)b.y - (double)b.x * (double)a.y;
                }
                if (area < 0.0)
                {
                    for (k = 0u; k < lp.count / 2u; ++k)
                    {
                        Poly2Pt tmp = lp.pts[k];
                        lp.pts[k] = lp.pts[lp.count - 1u - k];
                        lp.pts[lp.count - 1u - k] = tmp;
                    }
                }
                if (out->count >= out->cap)
                {
                    out->cap = out->cap ? out->cap * 2u : 8u;
                    out->loops = (Poly2Loop *)realloc(out->loops, (size_t)out->cap * sizeof *out->loops);
                }
                out->loops[out->count++] = lp;
            }
            else
            {
                free(lp.pts);
            }
        }
    }
    free(mobs);
}

static bool pt_in_poly_list(float px, float py, const Poly2List *pl)
{
    bool     inside = false;
    uint32_t li, i;
    for (li = 0u; li < pl->count; ++li)
    {
        const Poly2Loop *lp = &pl->loops[li];
        for (i = 0u; i < lp->count; ++i)
        {
            Poly2Pt a = lp->pts[i];
            Poly2Pt b = lp->pts[(i + 1u) % lp->count];
            if (((a.y > py) != (b.y > py)) &&
                (px < (b.x - a.x) * (py - a.y) / (b.y - a.y + 1e-20f) + a.x))
            {
                inside = !inside;
            }
        }
    }
    return inside;
}

static bool seg_intersect_t(float ax, float ay, float bx, float by,
                            float cx, float cy, float dx, float dy, float *out_ta)
{
    float r_x = bx - ax, r_y = by - ay;
    float s_x = dx - cx, s_y = dy - cy;
    float den = r_x * s_y - r_y * s_x;
    float qpx, qpy, t, u;

    if (fabsf(den) < 1e-8f)
        return false;
    qpx = cx - ax;
    qpy = cy - ay;
    t   = (qpx * s_y - qpy * s_x) / den;
    u   = (qpx * r_y - qpy * r_x) / den;
    if (t > 1e-5f && t < 1.0f - 1e-5f && u >= -1e-5f && u <= 1.0f + 1e-5f)
    {
        *out_ta = t;
        return true;
    }
    return false;
}

static int cmp_float_asc(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void emit_split_segments(const Poly2List *src, const Poly2List *other,
                                bool keep_inside, bool keep_outside, bool rev_inside,
                                Seg2 **io_segs, uint32_t *io_cnt, uint32_t *io_cap)
{
    float    ts_stack[128];
    uint32_t li, i, oli, oi;

    for (li = 0u; li < src->count; ++li)
    {
        const Poly2Loop *lp = &src->loops[li];
        for (i = 0u; i < lp->count; ++i)
        {
            Poly2Pt  p0 = lp->pts[i];
            Poly2Pt  p1 = lp->pts[(i + 1u) % lp->count];
            uint32_t tn = 0u;
            uint32_t k;

            ts_stack[tn++] = 0.0f;
            ts_stack[tn++] = 1.0f;
            for (oli = 0u; oli < other->count && tn < 126u; ++oli)
            {
                const Poly2Loop *ol = &other->loops[oli];
                for (oi = 0u; oi < ol->count && tn < 126u; ++oi)
                {
                    Poly2Pt q0 = ol->pts[oi];
                    Poly2Pt q1 = ol->pts[(oi + 1u) % ol->count];
                    float   t  = 0.0f;
                    if (seg_intersect_t(p0.x, p0.y, p1.x, p1.y, q0.x, q0.y, q1.x, q1.y, &t))
                        ts_stack[tn++] = t;
                }
            }
            qsort(ts_stack, tn, sizeof(float), cmp_float_asc);

            for (k = 0u; k + 1u < tn; ++k)
            {
                float t0 = ts_stack[k];
                float t1 = ts_stack[k + 1u];
                float sx0, sy0, sx1, sy1, mx, my;
                bool  ins, keep, rev;
                if (t1 - t0 < 1e-5f)
                    continue;
                sx0 = p0.x + (p1.x - p0.x) * t0;
                sy0 = p0.y + (p1.y - p0.y) * t0;
                sx1 = p0.x + (p1.x - p0.x) * t1;
                sy1 = p0.y + (p1.y - p0.y) * t1;
                mx  = 0.5f * (sx0 + sx1);
                my  = 0.5f * (sy0 + sy1);
                ins = pt_in_poly_list(mx, my, other);
                keep = ins ? keep_inside : keep_outside;
                if (!keep)
                    continue;
                rev = ins && rev_inside;
                if (*io_cnt >= *io_cap)
                {
                    *io_cap = *io_cap ? (*io_cap * 2u) : 128u;
                    *io_segs = (Seg2 *)realloc(*io_segs, (size_t)(*io_cap) * sizeof **io_segs);
                }
                (*io_segs)[(*io_cnt)++] = rev
                    ? (Seg2){ sx1, sy1, sx0, sy0, false }
                    : (Seg2){ sx0, sy0, sx1, sy1, false };
            }
        }
    }
}

AnimId anim_bool_op(AnimScene *s, AnimId a, AnimId b, AnimBoolOp op)
{
    Poly2List pa = { 0 }, pb = { 0 };
    Seg2     *segs = NULL;
    uint32_t  seg_cnt = 0u, seg_cap = 0u;
    AnimId    res;
    uint32_t  i;

    collect_world_polys(s, a, &pa);
    collect_world_polys(s, b, &pb);

    switch (op)
    {
    case ANIM_BOOL_UNION:
        emit_split_segments(&pa, &pb, false, true, false, &segs, &seg_cnt, &seg_cap);
        emit_split_segments(&pb, &pa, false, true, false, &segs, &seg_cnt, &seg_cap);
        break;
    case ANIM_BOOL_INTERSECT:
        emit_split_segments(&pa, &pb, true, false, false, &segs, &seg_cnt, &seg_cap);
        emit_split_segments(&pb, &pa, true, false, false, &segs, &seg_cnt, &seg_cap);
        break;
    case ANIM_BOOL_DIFFERENCE:
        emit_split_segments(&pa, &pb, false, true, false, &segs, &seg_cnt, &seg_cap);
        emit_split_segments(&pb, &pa, true, false, true,  &segs, &seg_cnt, &seg_cap);
        break;
    case ANIM_BOOL_XOR:
        emit_split_segments(&pa, &pb, true, true, true, &segs, &seg_cnt, &seg_cap);
        emit_split_segments(&pb, &pa, true, true, true, &segs, &seg_cnt, &seg_cap);
        break;
    }

    res = anim_vmob_create(s);
    if (ANIM_ID_VALID(res))
    {
        for (i = 0u; i < seg_cnt; ++i)
        {
            float sx, sy, cx, cy;
            uint32_t steps = 0u;
            if (segs[i].used)
                continue;
            segs[i].used = true;
            sx = segs[i].x0; sy = segs[i].y0;
            cx = segs[i].x1; cy = segs[i].y1;
            anim_vmob_begin_subpath(s, res, ANIM_V3(sx, sy, 0.0f));
            anim_vmob_line_to(s, res, ANIM_V3(cx, cy, 0.0f));

            while (steps++ < seg_cnt)
            {
                float    best_d2 = 1e-4f;
                uint32_t best_j  = UINT32_MAX;
                uint32_t j;
                if ((cx - sx) * (cx - sx) + (cy - sy) * (cy - sy) < 1e-6f)
                    break;
                for (j = 0u; j < seg_cnt; ++j)
                {
                    float d2;
                    if (segs[j].used)
                        continue;
                    d2 = (segs[j].x0 - cx) * (segs[j].x0 - cx) + (segs[j].y0 - cy) * (segs[j].y0 - cy);
                    if (d2 < best_d2)
                    {
                        best_d2 = d2;
                        best_j  = j;
                    }
                }
                if (best_j == UINT32_MAX)
                    break;
                segs[best_j].used = true;
                cx = segs[best_j].x1;
                cy = segs[best_j].y1;
                anim_vmob_line_to(s, res, ANIM_V3(cx, cy, 0.0f));
            }
            anim_vmob_close_subpath(s, res);
        }
        anim_set_fill_rule(s, res, ANIM_FILL_EVEN_ODD, false);
        anim_set_fill(s, res, ANIM_TEAL, false);
        anim_set_stroke(s, res, ANIM_WHITE, 2.5f, false);
    }

    free(segs);
    poly2_list_free(&pa);
    poly2_list_free(&pb);
    return res;
}

/* ------------------------------------------- Polar, Implicit & StreamLines */

AnimId anim_polar_plane(AnimScene *s, float max_radius, float radius_step, uint32_t azimuth_divs, float size)
{
    uint32_t rings  = (uint32_t)MU_MAX(floorf(max_radius / MU_MAX(radius_step, 1e-3f)), 1.0f);
    uint32_t az     = MU_CLAMP(azimuth_divs > 0u ? azimuth_divs : 12u, 4u, 72u);
    float    scale  = (size > 0.0f ? size : 5.0f) / (2.0f * MU_MAX(max_radius, 1e-3f));
    uint32_t cap    = rings + az + 4u;
    AnimId  *kids   = (AnimId *)malloc((size_t)cap * sizeof *kids);
    uint32_t k      = 0u, i;
    AnimId   grp;
    VMobCold *gc;

    for (i = 1u; i <= rings; ++i)
    {
        float  r = (float)i * radius_step * scale;
        AnimId c = anim_circle(s, r);
        anim_set_fill(s, c, ANIM_CLEAR, false);
        anim_set_stroke(s, c, (i == rings) ? ANIM_WHITE : ANIM_RGBA(0.35f, 0.55f, 0.78f, 0.45f), (i == rings) ? 2.0f : 1.2f, false);
        kids[k++] = c;
    }
    for (i = 0u; i < az; ++i)
    {
        float    th = ANIM_TAU * ((float)i / (float)az);
        float    rr = max_radius * scale;
        AnimId   ln = anim_line(s, ANIM_ORIGIN, ANIM_V3(rr * cosf(th), rr * sinf(th), 0.0f));
        anim_set_stroke(s, ln, ANIM_RGBA(0.35f, 0.55f, 0.78f, 0.40f), 1.1f, false);
        kids[k++] = ln;
    }

    grp = anim_group(s, kids, k);
    free(kids);
    gc = anim__cold(s, grp);
    if (gc)
    {
        /* Store world-units-per-polar-unit scale in axes_x_len for anim_plot_polar */
        gc->axes_x_min = max_radius;
        gc->axes_x_len = scale;
    }
    return grp;
}

AnimId anim_plot_polar(AnimScene *s, AnimId plane_id, AnimScalarFn r_fn, float theta_min, float theta_max, uint32_t samples, void *user)
{
    const VMobCold *pc = anim__cold(s, plane_id);
    AnimVec3        org = anim_center(s, plane_id);
    float           sc  = (pc && pc->axes_x_len > 0.0f) ? pc->axes_x_len : 1.0f;
    uint32_t        n   = MU_CLAMP(samples > 0u ? samples : 180u, 8u, 1024u);
    AnimId          id  = anim_vmob_create(s);
    uint32_t        i;

    if (!ANIM_ID_VALID(id) || !r_fn)
        return ANIM_ID_NIL;
    for (i = 0u; i <= n; ++i)
    {
        float    th = theta_min + (theta_max - theta_min) * ((float)i / (float)n);
        float    r  = r_fn(th, user) * sc;
        AnimVec3 pt = ANIM_V3(org.x + r * cosf(th), org.y + r * sinf(th), org.z);
        if (i == 0u)
            anim_vmob_begin_subpath(s, id, pt);
        else
            anim_vmob_line_to(s, id, pt);
    }
    anim_set_stroke(s, id, ANIM_YELLOW, 3.0f, false);
    anim_set_fill(s, id, ANIM_CLEAR, false);
    return id;
}

AnimId anim_implicit_plot(AnimScene *s, AnimId axes_id, AnimImplicitFn fn,
                          float x_min, float x_max, float y_min, float y_max,
                          uint32_t nx, uint32_t ny, void *user)
{
    uint32_t gx = MU_CLAMP(nx > 0u ? nx : 64u, 8u, 160u);
    uint32_t gy = MU_CLAMP(ny > 0u ? ny : 64u, 8u, 160u);
    float   *vals;
    AnimId   id;
    uint32_t ix, iy;

    if (!fn)
        return ANIM_ID_NIL;
    vals = (float *)malloc((size_t)(gx + 1u) * (size_t)(gy + 1u) * sizeof *vals);
    MU_ASSERT(vals != NULL);

    for (iy = 0u; iy <= gy; ++iy)
    {
        float y = y_min + (y_max - y_min) * ((float)iy / (float)gy);
        for (ix = 0u; ix <= gx; ++ix)
        {
            float x = x_min + (x_max - x_min) * ((float)ix / (float)gx);
            vals[iy * (gx + 1u) + ix] = fn(x, y, user);
        }
    }

    id = anim_vmob_create(s);
    if (ANIM_ID_VALID(id))
    {
        float dx = (x_max - x_min) / (float)gx;
        float dy = (y_max - y_min) / (float)gy;

        for (iy = 0u; iy < gy; ++iy)
        {
            float y0 = y_min + (float)iy * dy;
            float y1 = y0 + dy;
            for (ix = 0u; ix < gx; ++ix)
            {
                float x0 = x_min + (float)ix * dx;
                float x1 = x0 + dx;
                float f00 = vals[iy * (gx + 1u) + ix];
                float f10 = vals[iy * (gx + 1u) + ix + 1u];
                float f11 = vals[(iy + 1u) * (gx + 1u) + ix + 1u];
                float f01 = vals[(iy + 1u) * (gx + 1u) + ix];
                int   mask = (f00 > 0.0f ? 1 : 0) | (f10 > 0.0f ? 2 : 0) |
                             (f11 > 0.0f ? 4 : 0) | (f01 > 0.0f ? 8 : 0);
                AnimVec3 pts[4];
                int      np = 0;

                if (mask == 0 || mask == 15)
                    continue;
                /* Edge crossings: bottom(0), right(1), top(2), left(3) */
                #define INTERP_PT(xa, ya, fa, xb, yb, fb) do { \
                    float denom = (fb) - (fa); \
                    float t = fabsf(denom) > 1e-9f ? MU_CLAMP(-(fa) / denom, 0.0f, 1.0f) : 0.5f; \
                    float cx = (xa) + ((xb) - (xa)) * t; \
                    float cy = (ya) + ((yb) - (ya)) * t; \
                    pts[np++] = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx, cy) : ANIM_V3(cx, cy, 0.0f); \
                } while (0)

                if (((mask & 1) != 0) != ((mask & 2) != 0)) INTERP_PT(x0, y0, f00, x1, y0, f10);
                if (((mask & 2) != 0) != ((mask & 4) != 0)) INTERP_PT(x1, y0, f10, x1, y1, f11);
                if (((mask & 4) != 0) != ((mask & 8) != 0)) INTERP_PT(x1, y1, f11, x0, y1, f01);
                if (((mask & 8) != 0) != ((mask & 1) != 0)) INTERP_PT(x0, y1, f01, x0, y0, f00);
                #undef INTERP_PT

                if (np >= 2)
                {
                    anim_vmob_begin_subpath(s, id, pts[0]);
                    anim_vmob_line_to(s, id, pts[1]);
                }
                if (np == 4)
                {
                    anim_vmob_begin_subpath(s, id, pts[2]);
                    anim_vmob_line_to(s, id, pts[3]);
                }
            }
        }
        anim_set_stroke(s, id, ANIM_CYAN, 2.8f, false);
        anim_set_fill(s, id, ANIM_CLEAR, false);
    }

    free(vals);
    return id;
}

AnimId anim_stream_lines(AnimScene *s, AnimId axes_id, AnimFieldFn fn,
                         float x_min, float x_max, float y_min, float y_max,
                         float x_step, float y_step, uint32_t max_steps, void *user)
{
    float    xs  = x_step > 1e-3f ? x_step : 0.7f;
    float    ys  = y_step > 1e-3f ? y_step : 0.7f;
    uint32_t nst = MU_CLAMP(max_steps > 0u ? max_steps : 32u, 4u, 128u);
    uint32_t nx  = (uint32_t)MU_MAX(floorf((x_max - x_min) / xs) + 1.0f, 1.0f);
    uint32_t ny  = (uint32_t)MU_MAX(floorf((y_max - y_min) / ys) + 1.0f, 1.0f);
    AnimId  *kids;
    uint32_t k   = 0u, ix, iy;
    AnimId   grp;

    if (!fn)
        return ANIM_ID_NIL;
    kids = (AnimId *)malloc((size_t)(nx * ny) * sizeof *kids);
    MU_ASSERT(kids != NULL);

    for (iy = 0u; iy < ny; ++iy)
    {
        float y0 = y_min + ((float)iy + 0.5f) * ((y_max - y_min) / (float)ny);
        for (ix = 0u; ix < nx; ++ix)
        {
            float    x0 = x_min + ((float)ix + 0.5f) * ((x_max - x_min) / (float)nx);
            float    x = x0, y = y0;
            AnimId   ln = anim_vmob_create(s);
            uint32_t st;
            float    mag0 = 0.0f;

            for (st = 0u; st <= nst; ++st)
            {
                AnimVec3 wp = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, x, y) : ANIM_V3(x, y, 0.0f);
                AnimVec3 v1 = fn(ANIM_V3(x, y, 0.0f), user);
                float    m  = sqrtf(v1.x * v1.x + v1.y * v1.y);
                float    dt = 0.08f;
                AnimVec3 v2;
                if (st == 0u)
                {
                    mag0 = m;
                    anim_vmob_begin_subpath(s, ln, wp);
                }
                else
                {
                    anim_vmob_line_to(s, ln, wp);
                }
                if (m < 1e-4f || x < x_min - 0.5f || x > x_max + 0.5f || y < y_min - 0.5f || y > y_max + 0.5f)
                    break;
                v1.x /= m; v1.y /= m;
                v2 = fn(ANIM_V3(x + 0.5f * dt * v1.x, y + 0.5f * dt * v1.y, 0.0f), user);
                m  = sqrtf(v2.x * v2.x + v2.y * v2.y);
                if (m < 1e-4f)
                    break;
                x += dt * (v2.x / m);
                y += dt * (v2.y / m);
            }
            {
                float     t_col = MU_CLAMP(mag0 * 0.35f, 0.0f, 1.0f);
                AnimColor col   = anim_color_lerp_oklab(ANIM_BLUE_D, ANIM_YELLOW, t_col);
                anim_set_stroke(s, ln, col, 2.1f, false);
                anim_set_fill(s, ln, ANIM_CLEAR, false);
                anim_set_dash(s, ln, 18.0f, 10.0f, (float)((ix + iy) * 5u), false);
            }
            kids[k++] = ln;
        }
    }
    grp = anim_group(s, kids, k);
    free(kids);
    return grp;
}

/* --------------------------------------------------- Typography & Variables */

AnimId anim_bulleted_list(AnimScene *s, const char *font_path, const char *const *items,
                          uint32_t count, float text_height, float line_spacing)
{
    AnimId  *rows;
    uint32_t i;
    AnimId   grp;
    float    th = text_height > 0.0f ? text_height : 0.38f;
    float    sp = line_spacing > 0.0f ? line_spacing : th * 0.75f;

    if (!items || count == 0u)
        return anim_group(s, NULL, 0u);
    rows = (AnimId *)malloc((size_t)count * sizeof *rows);
    MU_ASSERT(rows != NULL);

    for (i = 0u; i < count; ++i)
    {
        AnimId dot = anim_dot(s, ANIM_ORIGIN, th * 0.14f);
        AnimId txt = anim_text(s, font_path, items[i] ? items[i] : "", th);
        AnimId pair[2];
        anim_set_fill(s, dot, ANIM_TEAL, false);
        anim_next_to(s, txt, dot, ANIM_RIGHT, th * 0.45f);
        pair[0] = dot;
        pair[1] = txt;
        rows[i] = anim_group(s, pair, 2u);
    }
    anim_arrange(s, rows, count, ANIM_DOWN, sp);
    for (i = 1u; i < count; ++i)
        anim_align_to(s, rows[i], rows[0], ANIM_LEFT);
    grp = anim_group(s, rows, count);
    free(rows);
    return grp;
}

typedef struct VarSyncData {
    AnimTrackerId tracker;
    AnimId        num_id;
    AnimId        eq_id;
} VarSyncData;

static void variable_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    VarSyncData *vd = (VarSyncData *)user;
    (void)target;
    (void)dt;
    if (!vd || !anim_vmob_alive(s, vd->num_id))
        return;
    anim_decimal_number_set(s, vd->num_id, anim_tracker_get(s, vd->tracker));
    anim_next_to(s, vd->num_id, vd->eq_id, ANIM_RIGHT, 0.10f);
}

AnimId anim_variable(AnimScene *s, const char *label, AnimTrackerId tracker,
                     uint32_t decimals, const char *font_path, float height)
{
    float  h      = height > 0.0f ? height : 0.48f;
    AnimId lbl_id = anim_text(s, font_path, label ? label : "x", h);
    AnimId eq_id  = anim_text(s, font_path, "=", h);
    AnimId num_id = anim_decimal_number(s, anim_tracker_get(s, tracker), decimals, font_path, h);
    AnimId kids[3];
    AnimId grp;
    VarSyncData *vd;

    anim_next_to(s, eq_id, lbl_id, ANIM_RIGHT, 0.10f);
    anim_next_to(s, num_id, eq_id, ANIM_RIGHT, 0.10f);
    kids[0] = lbl_id;
    kids[1] = eq_id;
    kids[2] = num_id;
    grp = anim_group(s, kids, 3u);

    vd = (VarSyncData *)malloc(sizeof *vd);
    if (vd)
    {
        vd->tracker = tracker;
        vd->num_id  = num_id;
        vd->eq_id   = eq_id;
        anim_add_updater(s, grp, variable_updater, vd, NULL, 0u, &tracker, 1u);
    }
    return grp;
}

/* ------------------------------------ True Z-Buffered 3D Solids & Polyhedra */

static AnimId create_mesh3d_mob(AnimScene *s, const SurfaceVert *verts, uint32_t vcnt,
                                const uint32_t *indices, uint32_t icnt, AnimColor fill_col)
{
    AnimId    id = anim_vmob_create(s);
    VMobHot  *h  = anim__hot(s, id);
    VMobCold *c  = anim__cold(s, id);
    uint32_t  vo = s->surf_vert_count;
    uint32_t  io = s->surf_idx_count;

    if (!h || !c)
        return ANIM_ID_NIL;
    anim__ensure_surf_vert_cap(s, vcnt);
    anim__ensure_surf_idx_cap(s, icnt);

    memcpy(&s->surf_verts[vo], verts, (size_t)vcnt * sizeof *verts);
    memcpy(&s->surf_indices[io], indices, (size_t)icnt * sizeof *indices);
    s->surf_vert_count += vcnt;
    s->surf_idx_count  += icnt;

    h->surf_vert_offset    = vo;
    h->surf_vert_count     = vcnt;
    h->surf_idx_offset     = io;
    h->surf_idx_count      = icnt;
    h->fill_color          = fill_col;
    h->stroke_width        = 0.0f;
    c->surf_grid_density   = 6.0f;
    c->surf_grid_alpha     = 0.25f;
    c->surf_roughness      = 0.32f;
    c->surf_use_fill_color = true;
    return id;
}

AnimId anim_prism(AnimScene *s, AnimVec3 dims)
{
    float hx = 0.5f * fabsf(dims.x > 0.0f ? dims.x : 1.0f);
    float hy = 0.5f * fabsf(dims.y > 0.0f ? dims.y : 1.0f);
    float hz = 0.5f * fabsf(dims.z > 0.0f ? dims.z : 1.0f);
    static const float fn[6][3] = {
        { 0, 0, 1 }, { 0, 0,-1 }, { 1, 0, 0 }, {-1, 0, 0 }, { 0, 1, 0 }, { 0,-1, 0 }
    };
    static const float fv[6][4][3] = {
        { {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1} },
        { { 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}, { 1, 1,-1} },
        { { 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}, { 1, 1, 1} },
        { {-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}, {-1, 1,-1} },
        { {-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}, {-1, 1,-1} },
        { {-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}, {-1,-1, 1} }
    };
    SurfaceVert verts[24];
    uint32_t    indices[36];
    uint32_t    f, k;

    for (f = 0u; f < 6u; ++f)
    {
        for (k = 0u; k < 4u; ++k)
        {
            SurfaceVert *sv = &verts[f * 4u + k];
            sv->pos   = ANIM_V3(fv[f][k][0] * hx, fv[f][k][1] * hy, fv[f][k][2] * hz);
            sv->norm  = ANIM_V3(fn[f][0], fn[f][1], fn[f][2]);
            sv->u     = (k == 1u || k == 2u) ? 1.0f : 0.0f;
            sv->v     = (k >= 2u) ? 1.0f : 0.0f;
            sv->color = ANIM_WHITE;
        }
        indices[f * 6u + 0u] = f * 4u + 0u;
        indices[f * 6u + 1u] = f * 4u + 1u;
        indices[f * 6u + 2u] = f * 4u + 2u;
        indices[f * 6u + 3u] = f * 4u + 0u;
        indices[f * 6u + 4u] = f * 4u + 2u;
        indices[f * 6u + 5u] = f * 4u + 3u;
    }
    return create_mesh3d_mob(s, verts, 24u, indices, 36u, ANIM_BLUE_C);
}

AnimId anim_cube(AnimScene *s, float side)
{
    float d = side > 0.0f ? side : 1.5f;
    return anim_prism(s, ANIM_V3(d, d, d));
}

static AnimId create_oriented_frustum(AnimScene *s, AnimVec3 a, AnimVec3 b,
                                      float r_bot, float r_top, uint32_t slices, AnimColor col)
{
    uint32_t     n     = MU_CLAMP(slices > 0u ? slices : 28u, 6u, 96u);
    AnimVec3     axis  = v3_sub(b, a);
    float        len   = v3_len(axis);
    AnimVec3     dir   = (len > 1e-5f) ? v3_mul(axis, 1.0f / len) : ANIM_UP;
    AnimVec3     ref   = (fabsf(dir.y) < 0.9f) ? ANIM_UP : ANIM_RIGHT;
    AnimVec3     t1    = v3_norm(v3_cross(ref, dir));
    AnimVec3     t2    = v3_cross(dir, t1);
    uint32_t     vcnt  = (n + 1u) * 2u + (n + 1u) * 2u; /* side + 2 caps */
    uint32_t     icnt  = n * 6u + n * 3u * 2u;
    SurfaceVert *verts = (SurfaceVert *)malloc((size_t)vcnt * sizeof *verts);
    uint32_t    *inds  = (uint32_t *)malloc((size_t)icnt * sizeof *inds);
    uint32_t     vi    = 0u, ii = 0u, i;
    float        slope = (len > 1e-5f) ? (r_bot - r_top) / len : 0.0f;
    AnimId       id;

    /* Side band */
    uint32_t side_base = vi;
    for (i = 0u; i <= n; ++i)
    {
        float    u   = (float)i / (float)n;
        float    th  = u * ANIM_TAU;
        AnimVec3 rad = v3_add(v3_mul(t1, cosf(th)), v3_mul(t2, sinf(th)));
        AnimVec3 nrm = v3_norm(v3_add(rad, v3_mul(dir, slope)));
        verts[vi++] = (SurfaceVert){ v3_add(a, v3_mul(rad, r_bot)), nrm, u, 0.0f, ANIM_WHITE };
        verts[vi++] = (SurfaceVert){ v3_add(b, v3_mul(rad, r_top)), nrm, u, 1.0f, ANIM_WHITE };
    }
    for (i = 0u; i < n; ++i)
    {
        uint32_t i0 = side_base + i * 2u;
        inds[ii++] = i0;     inds[ii++] = i0 + 2u; inds[ii++] = i0 + 3u;
        inds[ii++] = i0;     inds[ii++] = i0 + 3u; inds[ii++] = i0 + 1u;
    }

    /* Bottom cap */
    if (r_bot > 1e-5f)
    {
        uint32_t c_idx = vi;
        verts[vi++] = (SurfaceVert){ a, v3_mul(dir, -1.0f), 0.5f, 0.5f, ANIM_WHITE };
        for (i = 0u; i < n; ++i)
        {
            float    th  = ANIM_TAU * ((float)i / (float)n);
            AnimVec3 rad = v3_add(v3_mul(t1, cosf(th)), v3_mul(t2, sinf(th)));
            verts[vi++] = (SurfaceVert){ v3_add(a, v3_mul(rad, r_bot)), v3_mul(dir, -1.0f), 0.5f + 0.5f * cosf(th), 0.5f + 0.5f * sinf(th), ANIM_WHITE };
        }
        for (i = 0u; i < n; ++i)
        {
            inds[ii++] = c_idx;
            inds[ii++] = c_idx + 1u + ((i + 1u) % n);
            inds[ii++] = c_idx + 1u + i;
        }
    }

    /* Top cap */
    if (r_top > 1e-5f)
    {
        uint32_t c_idx = vi;
        verts[vi++] = (SurfaceVert){ b, dir, 0.5f, 0.5f, ANIM_WHITE };
        for (i = 0u; i < n; ++i)
        {
            float    th  = ANIM_TAU * ((float)i / (float)n);
            AnimVec3 rad = v3_add(v3_mul(t1, cosf(th)), v3_mul(t2, sinf(th)));
            verts[vi++] = (SurfaceVert){ v3_add(b, v3_mul(rad, r_top)), dir, 0.5f + 0.5f * cosf(th), 0.5f + 0.5f * sinf(th), ANIM_WHITE };
        }
        for (i = 0u; i < n; ++i)
        {
            inds[ii++] = c_idx;
            inds[ii++] = c_idx + 1u + i;
            inds[ii++] = c_idx + 1u + ((i + 1u) % n);
        }
    }

    id = create_mesh3d_mob(s, verts, vi, inds, ii, col);
    free(verts);
    free(inds);
    return id;
}

AnimId anim_cylinder(AnimScene *s, float radius, float height, uint32_t slices)
{
    float r = radius > 0.0f ? radius : 0.7f;
    float h = height > 0.0f ? height : 1.8f;
    return create_oriented_frustum(s, ANIM_V3(0.0f, -0.5f * h, 0.0f), ANIM_V3(0.0f, 0.5f * h, 0.0f), r, r, slices, ANIM_TEAL);
}

AnimId anim_cone(AnimScene *s, float radius, float height, uint32_t slices)
{
    float r = radius > 0.0f ? radius : 0.8f;
    float h = height > 0.0f ? height : 1.8f;
    return create_oriented_frustum(s, ANIM_V3(0.0f, -0.5f * h, 0.0f), ANIM_V3(0.0f, 0.5f * h, 0.0f), r, 0.0f, slices, ANIM_ORANGE);
}

AnimId anim_line3d(AnimScene *s, AnimVec3 a, AnimVec3 b, float radius, uint32_t slices)
{
    float r = radius > 0.0f ? radius : 0.03f;
    return create_oriented_frustum(s, a, b, r, r, slices > 0u ? slices : 14u, ANIM_WHITE);
}

AnimId anim_arrow3d(AnimScene *s, AnimVec3 a, AnimVec3 b, float shaft_radius, float tip_radius, float tip_len, uint32_t slices)
{
    AnimVec3 diff = v3_sub(b, a);
    float    len  = v3_len(diff);
    float    sr   = shaft_radius > 0.0f ? shaft_radius : 0.045f;
    float    tr   = tip_radius   > 0.0f ? tip_radius   : 0.14f;
    float    tl   = tip_len      > 0.0f ? tip_len      : MU_MIN(0.38f, len * 0.35f);
    AnimVec3 dir  = (len > 1e-5f) ? v3_mul(diff, 1.0f / len) : ANIM_UP;
    AnimVec3 mid  = v3_sub(b, v3_mul(dir, tl));
    uint32_t sl   = slices > 0u ? slices : 16u;
    AnimId   shaft = create_oriented_frustum(s, a, mid, sr, sr, sl, ANIM_YELLOW);
    AnimId   tip   = create_oriented_frustum(s, mid, b, tr, 0.0f, sl, ANIM_YELLOW);
    AnimId   kids[2] = { shaft, tip };
    return anim_group(s, kids, 2u);
}

AnimId anim_dot3d(AnimScene *s, AnimVec3 pos, float radius)
{
    AnimId sp = anim_sphere(s, radius > 0.0f ? radius : 0.12f, 20u, 12u);
    VMobCold *c = anim__cold(s, sp);
    if (c)
    {
        c->surf_grid_alpha     = 0.0f;
        c->surf_use_fill_color = true;
    }
    anim_set_fill(s, sp, ANIM_YELLOW, false);
    anim_move_to(s, sp, pos);
    return sp;
}

static AnimId build_flat_polyhedron(AnimScene *s, const float (*vtx)[3], uint32_t nv,
                                    const uint32_t *faces, uint32_t nf, uint32_t face_stride,
                                    float radius, AnimColor col)
{
    /* Face fan triangulation with exact per-face flat normals */
    uint32_t     max_tris = nf * (face_stride - 2u);
    SurfaceVert *verts    = (SurfaceVert *)malloc((size_t)(max_tris * 3u) * sizeof *verts);
    uint32_t    *inds     = (uint32_t *)malloc((size_t)(max_tris * 3u) * sizeof *inds);
    uint32_t     vi = 0u, ii = 0u, f, k;
    AnimId       id;
    (void)nv;

    for (f = 0u; f < nf; ++f)
    {
        uint32_t f_verts[8];
        uint32_t fv_cnt = 0u;
        AnimVec3 p0, p1, p2, nrm;

        for (k = 0u; k < face_stride; ++k)
        {
            uint32_t idx = faces[f * face_stride + k];
            if (idx == UINT32_MAX)
                break;
            f_verts[fv_cnt++] = idx;
        }
        if (fv_cnt < 3u)
            continue;
        p0 = v3_mul(v3_norm(ANIM_V3(vtx[f_verts[0]][0], vtx[f_verts[0]][1], vtx[f_verts[0]][2])), radius);
        p1 = v3_mul(v3_norm(ANIM_V3(vtx[f_verts[1]][0], vtx[f_verts[1]][1], vtx[f_verts[1]][2])), radius);
        p2 = v3_mul(v3_norm(ANIM_V3(vtx[f_verts[2]][0], vtx[f_verts[2]][1], vtx[f_verts[2]][2])), radius);
        nrm = v3_norm(v3_cross(v3_sub(p1, p0), v3_sub(p2, p0)));
        /* Flip if inward */
        if (v3_dot(nrm, p0) < 0.0f)
            nrm = v3_mul(nrm, -1.0f);

        for (k = 1u; k + 1u < fv_cnt; ++k)
        {
            AnimVec3 pa = p0;
            AnimVec3 pb = v3_mul(v3_norm(ANIM_V3(vtx[f_verts[k]][0], vtx[f_verts[k]][1], vtx[f_verts[k]][2])), radius);
            AnimVec3 pc = v3_mul(v3_norm(ANIM_V3(vtx[f_verts[k + 1u]][0], vtx[f_verts[k + 1u]][1], vtx[f_verts[k + 1u]][2])), radius);
            verts[vi + 0u] = (SurfaceVert){ pa, nrm, 0.0f, 0.0f, ANIM_WHITE };
            verts[vi + 1u] = (SurfaceVert){ pb, nrm, 1.0f, 0.0f, ANIM_WHITE };
            verts[vi + 2u] = (SurfaceVert){ pc, nrm, 0.5f, 1.0f, ANIM_WHITE };
            inds[ii++] = vi + 0u;
            inds[ii++] = vi + 1u;
            inds[ii++] = vi + 2u;
            vi += 3u;
        }
    }
    id = create_mesh3d_mob(s, verts, vi, inds, ii, col);
    free(verts);
    free(inds);
    return id;
}

AnimId anim_polyhedron(AnimScene *s, AnimPolyhedronKind kind, float radius)
{
    float r = radius > 0.0f ? radius : 1.25f;

    if (kind == ANIM_POLY_CUBE)
        return anim_cube(s, r * 1.1547f);

    if (kind == ANIM_POLY_TETRAHEDRON)
    {
        static const float v[4][3] = {
            { 1, 1, 1 }, { 1,-1,-1 }, {-1, 1,-1 }, {-1,-1, 1 }
        };
        static const uint32_t f[12] = { 0,1,2, 0,3,1, 0,2,3, 1,3,2 };
        return build_flat_polyhedron(s, v, 4u, f, 4u, 3u, r, ANIM_CORAL);
    }
    if (kind == ANIM_POLY_OCTAHEDRON)
    {
        static const float v[6][3] = {
            { 1,0,0 }, {-1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 }
        };
        static const uint32_t f[24] = {
            0,2,4, 4,2,1, 1,2,5, 5,2,0,
            4,3,0, 1,3,4, 5,3,1, 0,3,5
        };
        return build_flat_polyhedron(s, v, 6u, f, 8u, 3u, r, ANIM_GREEN);
    }
    if (kind == ANIM_POLY_ICOSAHEDRON)
    {
        const float phi = 1.61803398875f;
        const float v[12][3] = {
            {-1, phi, 0}, { 1, phi, 0}, {-1,-phi, 0}, { 1,-phi, 0},
            { 0,-1, phi}, { 0, 1, phi}, { 0,-1,-phi}, { 0, 1,-phi},
            { phi, 0,-1}, { phi, 0, 1}, {-phi, 0,-1}, {-phi, 0, 1}
        };
        static const uint32_t f[60] = {
            0,11,5, 0,5,1, 0,1,7, 0,7,10, 0,10,11,
            1,5,9, 5,11,4, 11,10,2, 10,7,6, 7,1,8,
            3,9,4, 3,4,2, 3,2,6, 3,6,8, 3,8,9,
            4,9,5, 2,4,11, 6,2,10, 8,6,7, 9,8,1
        };
        return build_flat_polyhedron(s, v, 12u, f, 20u, 3u, r, ANIM_PURPLE);
    }
    /* ANIM_POLY_DODECAHEDRON */
    {
        const float phi = 1.61803398875f;
        const float ip  = 1.0f / phi;
        const float v[20][3] = {
            { 1, 1, 1}, { 1, 1,-1}, { 1,-1, 1}, { 1,-1,-1},
            {-1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}, {-1,-1,-1},
            { 0, ip, phi}, { 0,-ip, phi}, { 0, ip,-phi}, { 0,-ip,-phi},
            { ip, phi, 0}, {-ip, phi, 0}, { ip,-phi, 0}, {-ip,-phi, 0},
            { phi, 0, ip}, { phi, 0,-ip}, {-phi, 0, ip}, {-phi, 0,-ip}
        };
        static const uint32_t f[60] = {
            0, 8, 9, 2,16,
            0,16,17, 1,12,
            0,12,13, 4, 8,
            1,17, 3,11,10,
            1,10, 5,13,12,
            2, 9, 6,15,14,
            2,14, 3,17,16,
            3,14,15, 7,11,
            4,13, 5,19,18,
            4,18, 6, 9, 8,
            5,10,11, 7,19,
            6,18,19, 7,15
        };
        return build_flat_polyhedron(s, v, 20u, f, 12u, 5u, r, ANIM_TEAL);
    }
}

/* ---------------- Computational Geometry, Multi-Contour, Frustum, Particles */

static int cmp_v2_lex(const void *a, const void *b)
{
    const AnimVec3 *pa = (const AnimVec3 *)a;
    const AnimVec3 *pb = (const AnimVec3 *)b;
    if (pa->x < pb->x) return -1;
    if (pa->x > pb->x) return 1;
    if (pa->y < pb->y) return -1;
    if (pa->y > pb->y) return 1;
    return 0;
}

static float cross2d(AnimVec3 o, AnimVec3 a, AnimVec3 b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

AnimId anim_convex_hull(AnimScene *s, const AnimVec3 *pts, uint32_t count)
{
    AnimVec3 *sorted, *hull;
    uint32_t  k = 0u, i, t;
    AnimId    poly;

    if (!pts || count == 0u)
        return ANIM_ID_NIL;
    if (count < 3u)
        return anim_polygon(s, pts, count);

    sorted = (AnimVec3 *)malloc((size_t)count * sizeof *sorted);
    hull   = (AnimVec3 *)malloc((size_t)(count * 2u) * sizeof *hull);
    MU_ASSERT(sorted != NULL && hull != NULL);
    memcpy(sorted, pts, (size_t)count * sizeof *sorted);
    qsort(sorted, count, sizeof *sorted, cmp_v2_lex);

    for (i = 0u; i < count; ++i)
    {
        while (k >= 2u && cross2d(hull[k - 2u], hull[k - 1u], sorted[i]) <= 0.0f)
            k--;
        hull[k++] = sorted[i];
    }
    t = k + 1u;
    for (i = count - 1u; i > 0u; --i)
    {
        AnimVec3 p = sorted[i - 1u];
        while (k >= t && cross2d(hull[k - 2u], hull[k - 1u], p) <= 0.0f)
            k--;
        hull[k++] = p;
    }
    if (k > 1u)
        k--;

    poly = anim_polygon(s, hull, k);
    anim_set_fill(s, poly, anim_rgba(0.22f, 0.72f, 0.90f, 0.28f), false);
    anim_set_stroke(s, poly, ANIM_TEAL, 3.0f, false);
    free(sorted);
    free(hull);
    return poly;
}

AnimId anim_voronoi(AnimScene *s, const AnimVec3 *sites, uint32_t count,
                    AnimVec3 bmin, AnimVec3 bmax, const AnimColor *colors)
{
    AnimId  *cells;
    uint32_t i, j, k;
    AnimId   grp;

    if (!sites || count == 0u)
        return anim_group(s, NULL, 0u);
    cells = (AnimId *)malloc((size_t)count * sizeof *cells);
    MU_ASSERT(cells != NULL);

    for (i = 0u; i < count; ++i)
    {
        AnimVec3 poly_a[128], poly_b[128];
        uint32_t na = 4u;
        poly_a[0] = ANIM_V3(bmin.x, bmin.y, 0.0f);
        poly_a[1] = ANIM_V3(bmax.x, bmin.y, 0.0f);
        poly_a[2] = ANIM_V3(bmax.x, bmax.y, 0.0f);
        poly_a[3] = ANIM_V3(bmin.x, bmax.y, 0.0f);

        for (j = 0u; j < count; ++j)
        {
            float    nx, ny, mx, my;
            uint32_t nb = 0u;
            if (i == j)
                continue;
            nx = sites[j].x - sites[i].x;
            ny = sites[j].y - sites[i].y;
            mx = 0.5f * (sites[i].x + sites[j].x);
            my = 0.5f * (sites[i].y + sites[j].y);

            /* Keep half-plane dot(p - m, n) <= 0 */
            for (k = 0u; k < na && nb < 126u; ++k)
            {
                AnimVec3 a  = poly_a[k];
                AnimVec3 b  = poly_a[(k + 1u) % na];
                float    da = (a.x - mx) * nx + (a.y - my) * ny;
                float    db = (b.x - mx) * nx + (b.y - my) * ny;
                bool     ia = (da <= 1e-6f);
                bool     ib = (db <= 1e-6f);
                if (ia)
                    poly_b[nb++] = a;
                if (ia != ib)
                {
                    float t = da / (da - db + 1e-20f);
                    poly_b[nb++] = ANIM_V3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0f);
                }
            }
            na = nb;
            if (na == 0u)
                break;
            memcpy(poly_a, poly_b, (size_t)na * sizeof(AnimVec3));
        }
        cells[i] = anim_polygon(s, poly_a, na);
        {
            float     tf = (count > 1u) ? ((float)i / (float)(count - 1u)) : 0.5f;
            AnimColor fc = colors ? colors[i] : anim_color_lerp_oklab(ANIM_BLUE_E, ANIM_TEAL, tf);
            if (!colors) fc.a = 0.55f;
            anim_set_fill(s, cells[i], fc, false);
            anim_set_stroke(s, cells[i], ANIM_WHITE, 2.0f, false);
        }
    }
    grp = anim_group(s, cells, count);
    free(cells);
    return grp;
}

typedef struct ContourLevelUser {
    AnimImplicitFn fn;
    void          *user;
    float          level;
} ContourLevelUser;

static float contour_level_fn(float x, float y, void *u)
{
    const ContourLevelUser *cu = (const ContourLevelUser *)u;
    return cu->fn(x, y, cu->user) - cu->level;
}

AnimId anim_contour_plot(AnimScene *s, AnimId axes_id, AnimImplicitFn fn,
                         float x_min, float x_max, float y_min, float y_max,
                         uint32_t nx, uint32_t ny, const float *levels, uint32_t num_levels,
                         AnimColor c_low, AnimColor c_high, void *user)
{
    uint32_t nl = (num_levels > 0u && levels) ? num_levels : 5u;
    AnimId  *kids;
    uint32_t i;
    AnimId   grp;

    if (!fn)
        return ANIM_ID_NIL;
    kids = (AnimId *)malloc((size_t)nl * sizeof *kids);
    MU_ASSERT(kids != NULL);
    for (i = 0u; i < nl; ++i)
    {
        float lv = levels ? levels[i] : (-1.0f + 2.0f * ((float)i / (float)MU_MAX(nl - 1u, 1u)));
        float tf = (nl > 1u) ? ((float)i / (float)(nl - 1u)) : 0.5f;
        ContourLevelUser cu = { fn, user, lv };
        AnimId c = anim_implicit_plot(s, axes_id, contour_level_fn, x_min, x_max, y_min, y_max, nx, ny, &cu);
        anim_set_stroke(s, c, anim_color_lerp_oklab(c_low, c_high, tf), 2.4f, false);
        kids[i] = c;
    }
    grp = anim_group(s, kids, nl);
    free(kids);
    return grp;
}

AnimId anim_frustum3d(AnimScene *s, float fov_deg, float aspect, float near_z, float far_z,
                      AnimVec3 pos, AnimVec3 target)
{
    AnimVec3 fwd   = v3_norm(v3_sub(target, pos));
    AnimVec3 up0   = (fabsf(fwd.y) < 0.9f) ? ANIM_UP : ANIM_RIGHT;
    AnimVec3 right = v3_norm(v3_cross(fwd, up0));
    AnimVec3 up    = v3_cross(right, fwd);
    float    th    = tanf(0.5f * fov_deg * (ANIM_PI / 180.0f));
    float    nh    = near_z * th, nw = nh * aspect;
    float    fh    = far_z  * th, fw = fh * aspect;
    AnimVec3 nc    = v3_add(pos, v3_mul(fwd, near_z));
    AnimVec3 fc    = v3_add(pos, v3_mul(fwd, far_z));
    AnimVec3 n[4], f[4];
    AnimId   kids[16];
    uint32_t k = 0u, i;

    n[0] = v3_add(nc, v3_add(v3_mul(right, -nw), v3_mul(up, -nh)));
    n[1] = v3_add(nc, v3_add(v3_mul(right,  nw), v3_mul(up, -nh)));
    n[2] = v3_add(nc, v3_add(v3_mul(right,  nw), v3_mul(up,  nh)));
    n[3] = v3_add(nc, v3_add(v3_mul(right, -nw), v3_mul(up,  nh)));

    f[0] = v3_add(fc, v3_add(v3_mul(right, -fw), v3_mul(up, -fh)));
    f[1] = v3_add(fc, v3_add(v3_mul(right,  fw), v3_mul(up, -fh)));
    f[2] = v3_add(fc, v3_add(v3_mul(right,  fw), v3_mul(up,  fh)));
    f[3] = v3_add(fc, v3_add(v3_mul(right, -fw), v3_mul(up,  fh)));

    for (i = 0u; i < 4u; ++i)
    {
        kids[k++] = anim_line3d(s, pos, f[i], 0.018f, 8u);
        kids[k++] = anim_line3d(s, n[i], n[(i + 1u) % 4u], 0.024f, 8u);
        kids[k++] = anim_line3d(s, f[i], f[(i + 1u) % 4u], 0.024f, 8u);
    }
    /* Near and far translucent clip quads */
    {
        AnimId nq = anim_polygon(s, n, 4u);
        AnimId fq = anim_polygon(s, f, 4u);
        anim_set_fill(s, nq, anim_rgba(0.35f, 0.85f, 0.95f, 0.22f), false);
        anim_set_stroke(s, nq, ANIM_TEAL, 2.2f, false);
        anim_set_fill(s, fq, anim_rgba(0.95f, 0.72f, 0.25f, 0.14f), false);
        anim_set_stroke(s, fq, ANIM_GOLD, 2.2f, false);
        kids[k++] = nq;
        kids[k++] = fq;
    }
    return anim_group(s, kids, k);
}

AnimId anim_pixel_grid(AnimScene *s, uint32_t cols, uint32_t rows, float cell_size, bool show_centers, AnimId *out_cells)
{
    uint32_t nc = MU_CLAMP(cols > 0u ? cols : 8u, 1u, 32u);
    uint32_t nr = MU_CLAMP(rows > 0u ? rows : 6u, 1u, 32u);
    float    cs = cell_size > 0.0f ? cell_size : 0.42f;
    float    ox = -0.5f * (float)nc * cs;
    float    oy =  0.5f * (float)nr * cs;
    uint32_t tot = nc * nr * (show_centers ? 2u : 1u);
    AnimId  *kids = (AnimId *)malloc((size_t)tot * sizeof *kids);
    uint32_t k = 0u, r, c;
    AnimId   grp;

    MU_ASSERT(kids != NULL);
    for (r = 0u; r < nr; ++r)
    {
        for (c = 0u; c < nc; ++c)
        {
            float  cx   = ox + ((float)c + 0.5f) * cs;
            float  cy   = oy - ((float)r + 0.5f) * cs;
            AnimId cell = anim_square(s, cs);
            anim_move_to(s, cell, ANIM_V3(cx, cy, 0.0f));
            anim_set_fill(s, cell, anim_rgba(0.10f, 0.13f, 0.20f, 0.55f), false);
            anim_set_stroke(s, cell, anim_rgba(0.35f, 0.48f, 0.68f, 0.70f), 1.4f, false);
            if (out_cells)
                out_cells[r * nc + c] = cell;
            kids[k++] = cell;
            if (show_centers)
            {
                AnimId d = anim_dot(s, ANIM_V3(cx, cy, 0.0f), cs * 0.08f);
                anim_set_fill(s, d, anim_rgba(0.55f, 0.65f, 0.80f, 0.75f), false);
                kids[k++] = d;
            }
        }
    }
    grp = anim_group(s, kids, k);
    free(kids);
    return grp;
}

typedef struct ParticleSimDesc {
    uint32_t  count;
    AnimVec3  origin;
    AnimVec3  gravity;
    float     life_max;
    AnimColor c_start;
    AnimColor c_end;
    AnimVec3 *vel;
    float    *phase;
    AnimId   *dots;
} ParticleSimDesc;

static void particle_sys_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    ParticleSimDesc *ps = (ParticleSimDesc *)user;
    VMobCold        *gc = anim__cold(s, target);
    uint32_t         i;

    if (!ps || !gc)
        return;
    if (dt > 0.0f)
        gc->axes_x_min += dt;

    for (i = 0u; i < ps->count; ++i)
    {
        float    t_raw = gc->axes_x_min + ps->phase[i];
        float    tau   = fmodf(t_raw, ps->life_max);
        float    u     = tau / ps->life_max;
        AnimVec3 p     = ANIM_V3(
            ps->origin.x + ps->vel[i].x * tau + 0.5f * ps->gravity.x * tau * tau,
            ps->origin.y + ps->vel[i].y * tau + 0.5f * ps->gravity.y * tau * tau,
            ps->origin.z + ps->vel[i].z * tau + 0.5f * ps->gravity.z * tau * tau);
        AnimColor col  = anim_color_lerp_oklab(ps->c_start, ps->c_end, u);
        float     fade = sinf(ANIM_PI * u);
        float     sc   = 0.35f + 0.85f * fade;
        VMobHot  *dh   = anim__hot(s, ps->dots[i]);
        if (!dh)
            continue;
        col.a *= fade;
        anim_move_to(s, ps->dots[i], p);
        anim_set_fill(s, ps->dots[i], col, false);
        dh->scale = ANIM_V3(sc, sc, sc);
        dh->mat_dirty = true;
    }
}

AnimId anim_particle_system(AnimScene *s, uint32_t count, AnimVec3 origin,
                            AnimVec3 vel_min, AnimVec3 vel_max, AnimVec3 gravity,
                            float life_max, AnimColor c_start, AnimColor c_end, float radius)
{
    uint32_t         n   = MU_CLAMP(count > 0u ? count : 24u, 1u, 256u);
    float            lm  = life_max > 0.05f ? life_max : 1.5f;
    float            rad = radius > 0.0f ? radius : 0.055f;
    ParticleSimDesc *ps  = (ParticleSimDesc *)calloc(1, sizeof *ps);
    AnimId           grp;
    uint32_t         i;
    uint32_t         seed = 0x9E3779B9u;

    MU_ASSERT(ps != NULL);
    ps->count    = n;
    ps->origin   = origin;
    ps->gravity  = gravity;
    ps->life_max = lm;
    ps->c_start  = c_start;
    ps->c_end    = c_end;
    ps->vel      = (AnimVec3 *)malloc((size_t)n * sizeof *ps->vel);
    ps->phase    = (float *)malloc((size_t)n * sizeof *ps->phase);
    ps->dots     = (AnimId *)malloc((size_t)n * sizeof *ps->dots);
    MU_ASSERT(ps->vel && ps->phase && ps->dots);

    for (i = 0u; i < n; ++i)
    {
        float r0, r1, r2;
        seed = seed * 1664525u + 1013904223u; r0 = (float)(seed & 0xFFFFu) / 65535.0f;
        seed = seed * 1664525u + 1013904223u; r1 = (float)(seed & 0xFFFFu) / 65535.0f;
        seed = seed * 1664525u + 1013904223u; r2 = (float)(seed & 0xFFFFu) / 65535.0f;
        ps->vel[i]   = ANIM_V3(vel_min.x + (vel_max.x - vel_min.x) * r0,
                               vel_min.y + (vel_max.y - vel_min.y) * r1,
                               vel_min.z + (vel_max.z - vel_min.z) * r2);
        ps->phase[i] = ((float)i / (float)n) * lm;
        ps->dots[i]  = anim_dot(s, origin, rad);
        anim_set_glow(s, ps->dots[i], 0.95f, false);
    }
    grp = anim_group(s, ps->dots, n);
    anim_add_updater(s, grp, particle_sys_updater, ps, NULL, 0u, NULL, 0u);
    particle_sys_updater(s, grp, 0.0f, ps);
    return grp;
}

/* ------------------------------------------- 3D Vector Fields & Streamlines */

static void append_frustum_mesh(SurfaceVert *verts, uint32_t *io_v,
                                uint32_t *indices, uint32_t *io_i,
                                AnimVec3 a, AnimVec3 b, float r0, float r1,
                                uint32_t slices, AnimColor col)
{
    AnimVec3 diff = v3_sub(b, a);
    float    len  = v3_len(diff);
    AnimVec3 dir  = (len > 1e-6f) ? v3_mul(diff, 1.0f / len) : ANIM_UP;
    AnimVec3 up_r = (fabsf(dir.y) < 0.9f) ? ANIM_UP : ANIM_RIGHT;
    AnimVec3 u_ax = v3_norm(v3_cross(up_r, dir));
    AnimVec3 v_ax = v3_cross(dir, u_ax);
    uint32_t base_v = *io_v;
    uint32_t i;

    for (i = 0u; i <= slices; ++i)
    {
        float    u   = (float)i / (float)slices;
        float    ang = u * ANIM_TAU;
        float    ca  = cosf(ang);
        float    sa  = sinf(ang);
        AnimVec3 rad = v3_add(v3_mul(u_ax, ca), v3_mul(v_ax, sa));
        AnimVec3 nrm = v3_norm(v3_add(rad, v3_mul(dir, (len > 1e-6f) ? (r0 - r1) / len : 0.0f)));
        SurfaceVert sv0, sv1;
        sv0.pos   = v3_add(a, v3_mul(rad, r0));
        sv0.norm  = nrm;
        sv0.u     = u;
        sv0.v     = 0.0f;
        sv0.color = col;
        sv1.pos   = v3_add(b, v3_mul(rad, r1));
        sv1.norm  = nrm;
        sv1.u     = u;
        sv1.v     = 1.0f;
        sv1.color = col;
        verts[base_v + i * 2u + 0u] = sv0;
        verts[base_v + i * 2u + 1u] = sv1;
    }
    for (i = 0u; i < slices; ++i)
    {
        uint32_t i0 = base_v + i * 2u;
        uint32_t i1 = i0 + 1u;
        uint32_t i2 = i0 + 2u;
        uint32_t i3 = i0 + 3u;
        indices[(*io_i)++] = i0; indices[(*io_i)++] = i2; indices[(*io_i)++] = i3;
        indices[(*io_i)++] = i0; indices[(*io_i)++] = i3; indices[(*io_i)++] = i1;
    }
    *io_v = base_v + (slices + 1u) * 2u;
}

AnimId anim_vector_field3d(AnimScene *s, AnimFieldFn fn, AnimVec3 bmin, AnimVec3 bmax,
                           uint32_t nx, uint32_t ny, uint32_t nz, float max_arrow_len,
                           AnimColor c_low, AnimColor c_high, void *user)
{
    uint32_t     cx = MU_CLAMP(nx > 0u ? nx : 4u, 1u, 12u);
    uint32_t     cy = MU_CLAMP(ny > 0u ? ny : 4u, 1u, 12u);
    uint32_t     cz = MU_CLAMP(nz > 0u ? nz : 4u, 1u, 12u);
    uint32_t     total = cx * cy * cz;
    const uint32_t slices = 8u;
    uint32_t     verts_per_arrow = (slices + 1u) * 4u;
    uint32_t     idxs_per_arrow  = slices * 12u;
    AnimVec3    *pts = (AnimVec3 *)malloc((size_t)total * sizeof *pts);
    AnimVec3    *vecs = (AnimVec3 *)malloc((size_t)total * sizeof *vecs);
    float       *mags = (float *)malloc((size_t)total * sizeof *mags);
    float        max_mag = 1e-5f;
    float        alen_max = max_arrow_len > 0.0f ? max_arrow_len : 0.7f;
    AnimColor    cl = (c_low.a > 0.0f) ? c_low : ANIM_BLUE_D;
    AnimColor    ch = (c_high.a > 0.0f) ? c_high : ANIM_YELLOW;
    AnimId       id;
    VMobHot     *hd;
    VMobCold    *cd;
    uint32_t     ix, iy, iz, k = 0u;
    uint32_t     vo, io, v_cur, i_cur;

    if (!s || !fn)
    {
        free(pts); free(vecs); free(mags);
        return ANIM_ID_NIL;
    }
    MU_ASSERT(pts != NULL && vecs != NULL && mags != NULL);

    for (iz = 0u; iz < cz; ++iz)
    {
        float tz = (cz > 1u) ? ((float)iz / (float)(cz - 1u)) : 0.5f;
        float pz = bmin.z + (bmax.z - bmin.z) * tz;
        for (iy = 0u; iy < cy; ++iy)
        {
            float ty = (cy > 1u) ? ((float)iy / (float)(cy - 1u)) : 0.5f;
            float py = bmin.y + (bmax.y - bmin.y) * ty;
            for (ix = 0u; ix < cx; ++ix)
            {
                float tx = (cx > 1u) ? ((float)ix / (float)(cx - 1u)) : 0.5f;
                float px = bmin.x + (bmax.x - bmin.x) * tx;
                AnimVec3 p = ANIM_V3(px, py, pz);
                AnimVec3 v = fn(p, user);
                float    m = v3_len(v);
                pts[k]  = p;
                vecs[k] = v;
                mags[k] = m;
                if (m > max_mag) max_mag = m;
                k++;
            }
        }
    }

    id = anim_vmob_create(s);
    if (!ANIM_ID_VALID(id))
    {
        free(pts); free(vecs); free(mags);
        return ANIM_ID_NIL;
    }

    anim__ensure_surf_vert_cap(s, total * verts_per_arrow);
    anim__ensure_surf_idx_cap(s, total * idxs_per_arrow);
    hd = &s->hot[id.index];
    cd = &s->cold[id.index];
    vo = s->surf_vert_count;
    io = s->surf_idx_count;
    v_cur = 0u;
    i_cur = 0u;

    for (k = 0u; k < total; ++k)
    {
        float m = mags[k];
        if (m < 1e-5f)
            continue;
        {
            float     t_norm = MU_CLAMP(m / max_mag, 0.0f, 1.0f);
            float     len    = alen_max * (0.28f + 0.72f * t_norm);
            AnimVec3  dir    = v3_mul(vecs[k], 1.0f / m);
            AnimVec3  a      = v3_sub(pts[k], v3_mul(dir, 0.5f * len));
            AnimVec3  b      = v3_add(pts[k], v3_mul(dir, 0.5f * len));
            float     tip_l  = MU_MIN(len * 0.34f, alen_max * 0.28f);
            AnimVec3  mid    = v3_sub(b, v3_mul(dir, tip_l));
            float     sr     = MU_MAX(alen_max * 0.036f, 0.012f);
            float     tr     = MU_MAX(alen_max * 0.105f, 0.032f);
            AnimColor col    = anim_color_lerp_oklab(cl, ch, t_norm);
            uint32_t  base   = v_cur;
            uint32_t  i_start = i_cur;
            append_frustum_mesh(&s->surf_verts[vo], &v_cur, &s->surf_indices[io], &i_cur, a, mid, sr, sr, slices, col);
            append_frustum_mesh(&s->surf_verts[vo], &v_cur, &s->surf_indices[io], &i_cur, mid, b, tr, 0.0f, slices, col);
            (void)base; (void)i_start;
        }
    }

    s->surf_vert_count += v_cur;
    s->surf_idx_count  += i_cur;
    hd->surf_vert_offset = vo;
    hd->surf_vert_count  = v_cur;
    hd->surf_idx_offset  = io;
    hd->surf_idx_count   = i_cur;
    hd->fill_color       = ANIM_WHITE;
    hd->stroke_color     = ANIM_CLEAR;
    cd->surf_grid_density = 0.0f;
    cd->surf_grid_alpha   = 0.0f;
    cd->surf_roughness    = 0.32f;

    free(pts); free(vecs); free(mags);
    return id;
}

static AnimVec3 eval_norm_field3d(AnimFieldFn fn, AnimVec3 p, void *user, float *out_mag)
{
    AnimVec3 v = fn(p, user);
    float    m = v3_len(v);
    if (out_mag) *out_mag = m;
    return (m > 1e-5f) ? v3_mul(v, 1.0f / m) : ANIM_ORIGIN;
}

AnimId anim_stream_lines3d(AnimScene *s, AnimFieldFn fn, AnimVec3 bmin, AnimVec3 bmax,
                           uint32_t nx, uint32_t ny, uint32_t nz, float dt, uint32_t max_steps,
                           AnimColor c_start, AnimColor c_end, void *user)
{
    uint32_t cx  = MU_CLAMP(nx > 0u ? nx : 3u, 1u, 10u);
    uint32_t cy  = MU_CLAMP(ny > 0u ? ny : 3u, 1u, 10u);
    uint32_t cz  = MU_CLAMP(nz > 0u ? nz : 3u, 1u, 10u);
    uint32_t nst = MU_CLAMP(max_steps > 0u ? max_steps : 40u, 4u, 160u);
    float    h   = dt > 1e-4f ? dt : 0.10f;
    AnimColor cs = (c_start.a > 0.0f) ? c_start : ANIM_BLUE_C;
    AnimColor ce = (c_end.a > 0.0f) ? c_end : ANIM_YELLOW;
    AnimId   *kids;
    uint32_t  k = 0u, ix, iy, iz;
    AnimId    grp;

    if (!s || !fn)
        return ANIM_ID_NIL;
    kids = (AnimId *)malloc((size_t)(cx * cy * cz) * sizeof *kids);
    MU_ASSERT(kids != NULL);

    for (iz = 0u; iz < cz; ++iz)
    {
        float z0 = bmin.z + ((float)iz + 0.5f) * ((bmax.z - bmin.z) / (float)cz);
        for (iy = 0u; iy < cy; ++iy)
        {
            float y0 = bmin.y + ((float)iy + 0.5f) * ((bmax.y - bmin.y) / (float)cy);
            for (ix = 0u; ix < cx; ++ix)
            {
                float    x0 = bmin.x + ((float)ix + 0.5f) * ((bmax.x - bmin.x) / (float)cx);
                AnimVec3 p  = ANIM_V3(x0, y0, z0);
                AnimId   ln = anim_vmob_create(s);
                uint32_t st;
                float    mag0 = 0.0f;

                for (st = 0u; st <= nst; ++st)
                {
                    float    m1 = 0.0f, m2 = 0.0f, m3 = 0.0f, m4 = 0.0f;
                    AnimVec3 k1 = eval_norm_field3d(fn, p, user, &m1);
                    AnimVec3 k2, k3, k4;
                    if (st == 0u)
                    {
                        mag0 = m1;
                        anim_vmob_begin_subpath(s, ln, p);
                    }
                    else
                    {
                        anim_vmob_line_to(s, ln, p);
                    }
                    if (m1 < 1e-4f ||
                        p.x < bmin.x - 0.5f || p.x > bmax.x + 0.5f ||
                        p.y < bmin.y - 0.5f || p.y > bmax.y + 0.5f ||
                        p.z < bmin.z - 0.5f || p.z > bmax.z + 0.5f)
                        break;
                    k2 = eval_norm_field3d(fn, v3_add(p, v3_mul(k1, 0.5f * h)), user, &m2);
                    k3 = eval_norm_field3d(fn, v3_add(p, v3_mul(k2, 0.5f * h)), user, &m3);
                    k4 = eval_norm_field3d(fn, v3_add(p, v3_mul(k3, h)), user, &m4);
                    if (m2 < 1e-4f || m3 < 1e-4f || m4 < 1e-4f)
                        break;
                    p.x += (h / 6.0f) * (k1.x + 2.0f * k2.x + 2.0f * k3.x + k4.x);
                    p.y += (h / 6.0f) * (k1.y + 2.0f * k2.y + 2.0f * k3.y + k4.y);
                    p.z += (h / 6.0f) * (k1.z + 2.0f * k2.z + 2.0f * k3.z + k4.z);
                }
                {
                    float t_col = MU_CLAMP(mag0 * 0.25f, 0.0f, 1.0f);
                    AnimColor col0 = anim_color_lerp_oklab(cs, ce, t_col * 0.45f);
                    anim_set_stroke(s, ln, col0, 2.4f, false);
                    anim_set_fill(s, ln, ANIM_CLEAR, false);
                    anim_vmob_set_stroke_gradient(s, ln, col0, ce);
                }
                kids[k++] = ln;
            }
        }
    }

    grp = anim_group(s, kids, k);
    free(kids);
    return grp;
}

/* ----------------------------------------------------- 2D Statistical Plots */

AnimId anim_scatter_plot(AnimScene *s, AnimId axes_id, const float *xs, const float *ys,
                         const float *radii, const AnimColor *colors, uint32_t count, float default_radius)
{
    AnimId  *kids;
    uint32_t i;
    float    dr = default_radius > 0.0f ? default_radius : 0.08f;
    AnimId   grp;

    if (!s || !xs || !ys || count == 0u)
        return anim_group(s, NULL, 0u);
    kids = (AnimId *)malloc((size_t)count * sizeof *kids);
    MU_ASSERT(kids != NULL);

    for (i = 0u; i < count; ++i)
    {
        AnimVec3  p = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, xs[i], ys[i]) : ANIM_V3(xs[i], ys[i], 0.0f);
        float     r = (radii && radii[i] > 0.0f) ? radii[i] : dr;
        AnimColor c = (colors && colors[i].a > 0.0f) ? colors[i] : ANIM_BLUE_C;
        AnimId    d = anim_dot(s, p, r);
        anim_set_fill(s, d, c, false);
        anim_set_stroke(s, d, anim_rgba(1.0f, 1.0f, 1.0f, 0.35f), 1.4f, false);
        kids[i] = d;
    }
    grp = anim_group(s, kids, count);
    free(kids);
    return grp;
}

AnimId anim_bar_plot(AnimScene *s, AnimId axes_id, const float *values, uint32_t categories,
                     uint32_t series, bool stacked, const AnimColor *series_colors, float bar_group_width)
{
    static const AnimColor DEFAULT_PAL[6] = {
        { 0.12f, 0.56f, 0.90f, 1.0f },
        { 0.15f, 0.78f, 0.45f, 1.0f },
        { 0.94f, 0.65f, 0.15f, 1.0f },
        { 0.88f, 0.25f, 0.35f, 1.0f },
        { 0.60f, 0.40f, 0.92f, 1.0f },
        { 0.25f, 0.80f, 0.78f, 1.0f }
    };
    uint32_t  total = categories * series;
    AnimId   *kids;
    uint32_t  c, k, idx = 0u;
    float     gw = bar_group_width > 0.0f ? bar_group_width : 0.72f;
    AnimId    grp;

    if (!s || !values || categories == 0u || series == 0u)
        return anim_group(s, NULL, 0u);
    kids = (AnimId *)malloc((size_t)total * sizeof *kids);
    MU_ASSERT(kids != NULL);

    for (c = 0u; c < categories; ++c)
    {
        float base_y = 0.0f;
        float cx     = (float)c + 1.0f;
        float bw     = stacked ? gw : (gw / (float)series);
        for (k = 0u; k < series; ++k)
        {
            float     val = values[c * series + k];
            float     y0  = stacked ? base_y : 0.0f;
            float     y1  = y0 + val;
            float     x0  = stacked ? (cx - 0.5f * bw) : (cx - 0.5f * gw + (float)k * bw);
            float     x1  = x0 + bw * (stacked ? 1.0f : 0.90f);
            AnimVec3  p00 = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, x0, y0) : ANIM_V3(x0, y0, 0.0f);
            AnimVec3  p11 = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, x1, y1) : ANIM_V3(x1, y1, 0.0f);
            float     rw  = fabsf(p11.x - p00.x);
            float     rh  = fabsf(p11.y - p00.y);
            AnimId    bar = anim_rect(s, MU_MAX(rw, 0.02f), MU_MAX(rh, 0.02f));
            AnimColor col = (series_colors && series_colors[k].a > 0.0f)
                ? series_colors[k]
                : anim_srgb_to_linear(DEFAULT_PAL[k % 6u].r, DEFAULT_PAL[k % 6u].g, DEFAULT_PAL[k % 6u].b, 1.0f);
            anim_move_to(s, bar, ANIM_V3(0.5f * (p00.x + p11.x), 0.5f * (p00.y + p11.y), 0.0f));
            anim_set_fill(s, bar, col, false);
            anim_set_stroke(s, bar, anim_rgba(0.05f, 0.07f, 0.12f, 0.9f), 1.6f, false);
            kids[idx++] = bar;
            if (stacked)
                base_y = y1;
        }
    }

    grp = anim_group(s, kids, idx);
    free(kids);
    return grp;
}

AnimId anim_box_plot(AnimScene *s, AnimId axes_id, const AnimBoxStats *boxes,
                     uint32_t count, float box_width, const AnimColor *colors)
{
    AnimId  *kids;
    uint32_t i;
    float    bw = box_width > 0.0f ? box_width : 0.48f;
    AnimId   grp;

    if (!s || !boxes || count == 0u)
        return anim_group(s, NULL, 0u);
    kids = (AnimId *)malloc((size_t)count * sizeof *kids);
    MU_ASSERT(kids != NULL);

    for (i = 0u; i < count; ++i)
    {
        float        cx     = (float)i + 1.0f;
        AnimBoxStats b      = boxes[i];
        AnimVec3     p_q1   = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx - 0.5f * bw, b.q1) : ANIM_V3(cx - 0.5f * bw, b.q1, 0.0f);
        AnimVec3     p_q3   = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx + 0.5f * bw, b.q3) : ANIM_V3(cx + 0.5f * bw, b.q3, 0.0f);
        AnimVec3     p_medL = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx - 0.5f * bw, b.median) : ANIM_V3(cx - 0.5f * bw, b.median, 0.0f);
        AnimVec3     p_medR = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx + 0.5f * bw, b.median) : ANIM_V3(cx + 0.5f * bw, b.median, 0.0f);
        AnimVec3     p_min  = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx, b.min_v) : ANIM_V3(cx, b.min_v, 0.0f);
        AnimVec3     p_max  = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, cx, b.max_v) : ANIM_V3(cx, b.max_v, 0.0f);
        AnimVec3     p_q1c  = ANIM_V3(0.5f * (p_q1.x + p_q3.x), p_q1.y, 0.0f);
        AnimVec3     p_q3c  = ANIM_V3(0.5f * (p_q1.x + p_q3.x), p_q3.y, 0.0f);
        float        cap_hw = 0.25f * fabsf(p_q3.x - p_q1.x);
        AnimColor    col    = (colors && colors[i].a > 0.0f) ? colors[i] : ANIM_BLUE_C;
        AnimColor    fill_c = col;
        AnimId       iqr    = anim_rect(s, MU_MAX(fabsf(p_q3.x - p_q1.x), 0.03f), MU_MAX(fabsf(p_q3.y - p_q1.y), 0.03f));
        AnimId       med    = anim_line(s, p_medL, p_medR);
        AnimId       w_lo   = anim_line(s, p_min, p_q1c);
        AnimId       w_hi   = anim_line(s, p_q3c, p_max);
        AnimId       cap_lo = anim_line(s, ANIM_V3(p_min.x - cap_hw, p_min.y, 0.0f), ANIM_V3(p_min.x + cap_hw, p_min.y, 0.0f));
        AnimId       cap_hi = anim_line(s, ANIM_V3(p_max.x - cap_hw, p_max.y, 0.0f), ANIM_V3(p_max.x + cap_hw, p_max.y, 0.0f));
        AnimId       parts[6] = { w_lo, w_hi, cap_lo, cap_hi, iqr, med };

        fill_c.a *= 0.35f;
        anim_move_to(s, iqr, ANIM_V3(0.5f * (p_q1.x + p_q3.x), 0.5f * (p_q1.y + p_q3.y), 0.0f));
        anim_set_fill(s, iqr, fill_c, false);
        anim_set_stroke(s, iqr, col, 2.4f, false);
        anim_set_stroke(s, med, ANIM_YELLOW, 3.2f, false);
        anim_set_stroke(s, w_lo, col, 2.0f, false);
        anim_set_stroke(s, w_hi, col, 2.0f, false);
        anim_set_stroke(s, cap_lo, col, 2.0f, false);
        anim_set_stroke(s, cap_hi, col, 2.0f, false);
        kids[i] = anim_group(s, parts, 6u);
    }

    grp = anim_group(s, kids, count);
    free(kids);
    return grp;
}

static AnimColor heatmap_ramp(float val, float v_min, float v_max, AnimColor c_low, AnimColor c_mid, AnimColor c_high)
{
    float span = (fabsf(v_max - v_min) > 1e-6f) ? (v_max - v_min) : 1.0f;
    float t    = MU_CLAMP((val - v_min) / span, 0.0f, 1.0f);
    if (c_mid.a > 0.0f)
    {
        if (t < 0.5f)
            return anim_color_lerp_oklab(c_low, c_mid, t * 2.0f);
        return anim_color_lerp_oklab(c_mid, c_high, (t - 0.5f) * 2.0f);
    }
    return anim_color_lerp_oklab(c_low, c_high, t);
}

AnimId anim_heatmap(AnimScene *s, AnimId axes_id, const float *values, uint32_t cols, uint32_t rows,
                    float v_min, float v_max, AnimColor c_low, AnimColor c_mid, AnimColor c_high)
{
    float    xmin = 0.0f, xmax = (float)cols, ymin = 0.0f, ymax = (float)rows;
    AnimId  *cells;
    uint32_t r, c;
    AnimId   grp;
    VMobCold *cd;

    if (!s || !values || cols == 0u || rows == 0u)
        return anim_group(s, NULL, 0u);
    if (ANIM_ID_VALID(axes_id))
    {
        const VMobCold *ac = anim__cold(s, axes_id);
        if (ac && ac->is_axes)
        {
            xmin = ac->axes_x_min; xmax = ac->axes_x_max;
            ymin = ac->axes_y_min; ymax = ac->axes_y_max;
        }
    }

    cells = (AnimId *)malloc((size_t)(cols * rows) * sizeof *cells);
    MU_ASSERT(cells != NULL);

    for (r = 0u; r < rows; ++r)
    {
        float y0 = ymin + ((float)r / (float)rows) * (ymax - ymin);
        float y1 = ymin + ((float)(r + 1u) / (float)rows) * (ymax - ymin);
        for (c = 0u; c < cols; ++c)
        {
            float    x0 = xmin + ((float)c / (float)cols) * (xmax - xmin);
            float    x1 = xmin + ((float)(c + 1u) / (float)cols) * (xmax - xmin);
            AnimVec3 p00 = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, x0, y0) : ANIM_V3(x0, y0, 0.0f);
            AnimVec3 p11 = ANIM_ID_VALID(axes_id) ? anim_axes_c2p(s, axes_id, x1, y1) : ANIM_V3(x1, y1, 0.0f);
            AnimId   cell = anim_rect(s, MU_MAX(fabsf(p11.x - p00.x), 0.01f), MU_MAX(fabsf(p11.y - p00.y), 0.01f));
            AnimColor col = heatmap_ramp(values[r * cols + c], v_min, v_max, c_low, c_mid, c_high);
            anim_move_to(s, cell, ANIM_V3(0.5f * (p00.x + p11.x), 0.5f * (p00.y + p11.y), 0.0f));
            anim_set_fill(s, cell, col, false);
            anim_set_stroke(s, cell, ANIM_CLEAR, 0.0f, false);
            cells[r * cols + c] = cell;
        }
    }

    grp = anim_group(s, cells, cols * rows);
    cd  = anim__cold(s, grp);
    if (cd)
    {
        cd->is_heatmap   = true;
        cd->heatmap_cols = cols;
        cd->heatmap_rows = rows;
    }
    free(cells);
    return grp;
}

void anim_heatmap_set(AnimScene *s, AnimId heatmap_id, const float *values, uint32_t cols, uint32_t rows,
                      float v_min, float v_max, AnimColor c_low, AnimColor c_mid, AnimColor c_high)
{
    uint32_t n, i;
    if (!s || !values || cols == 0u || rows == 0u)
        return;
    n = anim_child_count(s, heatmap_id);
    if (n != cols * rows)
        return;
    for (i = 0u; i < n; ++i)
    {
        AnimId    cell = anim_child_at(s, heatmap_id, i);
        AnimColor col  = heatmap_ramp(values[i], v_min, v_max, c_low, c_mid, c_high);
        anim_set_fill(s, cell, col, false);
    }
}

/* ------------------------------------------- Tangent & Secant Slope Helpers */

static AnimId find_first_path_leaf(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    uint32_t       ch;
    if (!h)
        return ANIM_ID_NIL;
    if (h->subpath_count > 0u)
        return id;
    ch = h->first_child;
    while (ch != 0u)
    {
        AnimId found = find_first_path_leaf(s, (AnimId){ ch, s->hot[ch].gen });
        if (ANIM_ID_VALID(found))
            return found;
        ch = s->hot[ch].next_sibling;
    }
    return ANIM_ID_NIL;
}

AnimId anim_tangent_line(AnimScene *s, AnimId vmob_id, float alpha, float length)
{
    AnimId         leaf = find_first_path_leaf(s, vmob_id);
    const VMobHot *h;
    float          a    = MU_CLAMP(alpha, 0.0f, 1.0f);
    float          eps  = 1e-3f;
    float          a0   = MU_MAX(a - eps, 0.0f);
    float          a1   = MU_MIN(a + eps, 1.0f);
    AnimVec3       lp, lp0, lp1;
    float          wp[3], wp0[3], wp1[3];
    float          tx, ty, tz, tlen, half_l;
    AnimId         line;

    if (!s || !ANIM_ID_VALID(leaf))
        return anim_line(s, ANIM_V3(-0.5f, 0.0f, 0.0f), ANIM_V3(0.5f, 0.0f, 0.0f));

    anim__sync_world_mats(s);
    h   = anim__hot_const(s, leaf);
    lp  = anim_vmob_point_at(s, leaf, a);
    lp0 = anim_vmob_point_at(s, leaf, a0);
    lp1 = anim_vmob_point_at(s, leaf, a1);
    mu_mat4_transform_point(wp,  h->world_mat, (float[3]){ lp.x,  lp.y,  lp.z  });
    mu_mat4_transform_point(wp0, h->world_mat, (float[3]){ lp0.x, lp0.y, lp0.z });
    mu_mat4_transform_point(wp1, h->world_mat, (float[3]){ lp1.x, lp1.y, lp1.z });

    tx   = wp1[0] - wp0[0];
    ty   = wp1[1] - wp0[1];
    tz   = wp1[2] - wp0[2];
    tlen = sqrtf(tx * tx + ty * ty + tz * tz);
    if (tlen < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; tlen = 1.0f; }
    tx /= tlen; ty /= tlen; tz /= tlen;

    half_l = 0.5f * (length > 0.0f ? length : 2.5f);
    line   = anim_line(s,
                       ANIM_V3(wp[0] - tx * half_l, wp[1] - ty * half_l, wp[2] - tz * half_l),
                       ANIM_V3(wp[0] + tx * half_l, wp[1] + ty * half_l, wp[2] + tz * half_l));
    anim_set_stroke(s, line, ANIM_GOLD, 3.5f, false);
    return line;
}

AnimId anim_secant_slope_group(AnimScene *s, AnimId axes_id, AnimScalarFn fn, float x, float dx,
                               float line_length, void *user, AnimColor secant_col,
                               AnimColor dx_col, AnimColor dy_col)
{
    float    y1 = fn ? fn(x, user) : 0.0f;
    float    y2 = fn ? fn(x + dx, user) : 0.0f;
    AnimVec3 p1 = anim_axes_c2p(s, axes_id, x, y1);
    AnimVec3 p2 = anim_axes_c2p(s, axes_id, x + dx, y2);
    AnimVec3 pc = anim_axes_c2p(s, axes_id, x + dx, y1);
    float    vx = p2.x - p1.x, vy = p2.y - p1.y, vz = p2.z - p1.z;
    float    vlen = sqrtf(vx * vx + vy * vy + vz * vz);
    float    hl   = 0.5f * (line_length > 0.0f ? line_length : 3.0f);
    AnimVec3 mid  = ANIM_V3(0.5f * (p1.x + p2.x), 0.5f * (p1.y + p2.y), 0.5f * (p1.z + p2.z));
    AnimId   dx_l = anim_line(s, p1, pc);
    AnimId   dy_l = anim_line(s, pc, p2);
    AnimId   sec_l, d1, d2;
    AnimId   kids[5];

    if (vlen < 1e-6f) { vx = 1.0f; vy = 0.0f; vz = 0.0f; vlen = 1.0f; }
    vx /= vlen; vy /= vlen; vz /= vlen;

    sec_l = anim_line(s,
                      ANIM_V3(mid.x - vx * hl, mid.y - vy * hl, mid.z - vz * hl),
                      ANIM_V3(mid.x + vx * hl, mid.y + vy * hl, mid.z + vz * hl));
    d1    = anim_dot(s, p1, 0.06f);
    d2    = anim_dot(s, p2, 0.06f);

    anim_set_stroke(s, dx_l,  dx_col.a     > 0.0f ? dx_col     : ANIM_BLUE,   3.0f, false);
    anim_set_stroke(s, dy_l,  dy_col.a     > 0.0f ? dy_col     : ANIM_GREEN,  3.0f, false);
    anim_set_stroke(s, sec_l, secant_col.a > 0.0f ? secant_col : ANIM_YELLOW, 3.5f, false);
    anim_set_fill(s, d1, secant_col.a > 0.0f ? secant_col : ANIM_YELLOW, false);
    anim_set_fill(s, d2, secant_col.a > 0.0f ? secant_col : ANIM_YELLOW, false);

    kids[0] = dx_l; kids[1] = dy_l; kids[2] = sec_l; kids[3] = d1; kids[4] = d2;
    return anim_group(s, kids, 5u);
}

/* --------------------------------- Wave 3: Planes, Cutouts & 3D Lathe/Extrude */

AnimId anim_number_plane(AnimScene *s, float x_min, float x_max, float x_step,
                         float y_min, float y_max, float y_step,
                         float x_len, float y_len, bool faded_subgrid)
{
    AnimAxesDesc desc = {
        .x_min = x_min, .x_max = x_max, .x_step = x_step > 0.0f ? x_step : 1.0f,
        .y_min = y_min, .y_max = y_max, .y_step = y_step > 0.0f ? y_step : 1.0f,
        .x_length = x_len > 0.0f ? x_len : 12.0f,
        .y_length = y_len > 0.0f ? y_len : 7.0f,
        .include_ticks = false,
        .include_tips  = false,
        .include_grid  = true,
        .include_numbers = false,
        .font_path = NULL
    };
    AnimId grp = anim_axes(s, &desc);
    if (faded_subgrid && ANIM_ID_VALID(grp))
    {
        float    xs = desc.x_step * 0.5f, ys = desc.y_step * 0.5f;
        AnimId   sub_lines[256];
        uint32_t sn = 0u;
        float    v;
        for (v = x_min + xs; v < x_max - 1e-4f && sn < 256u; v += desc.x_step)
        {
            AnimId ln = anim_line(s, anim_axes_c2p(s, grp, v, y_min), anim_axes_c2p(s, grp, v, y_max));
            anim_set_stroke(s, ln, anim_hex_a(0x29ABCA, 0.25f), 1.0f, false);
            anim_set_z_index(s, ln, -2.0f, false);
            sub_lines[sn++] = ln;
        }
        for (v = y_min + ys; v < y_max - 1e-4f && sn < 256u; v += desc.y_step)
        {
            AnimId ln = anim_line(s, anim_axes_c2p(s, grp, x_min, v), anim_axes_c2p(s, grp, x_max, v));
            anim_set_stroke(s, ln, anim_hex_a(0x29ABCA, 0.25f), 1.0f, false);
            anim_set_z_index(s, ln, -2.0f, false);
            sub_lines[sn++] = ln;
        }
        for (uint32_t k = 0u; k < sn; ++k)
            anim_add_child(s, grp, sub_lines[k]);
    }
    return grp;
}

AnimId anim_complex_plane(AnimScene *s, float re_min, float re_max, float re_step,
                          float im_min, float im_max, float im_step,
                          float x_len, float y_len, bool include_labels, const char *font_path)
{
    AnimId plane = anim_number_plane(s, re_min, re_max, re_step, im_min, im_max, im_step, x_len, y_len, true);
    if (include_labels && ANIM_ID_VALID(plane))
    {
        float rs = re_step > 0.0f ? re_step : 1.0f;
        float is = im_step > 0.0f ? im_step : 1.0f;
        float v;
        for (v = ceilf(re_min / rs) * rs; v <= re_max + 1e-4f; v += rs)
        {
            char buf[32];
            AnimId lbl;
            if (fabsf(v) < 1e-4f) continue;
            snprintf(buf, sizeof buf, "%.0f", (double)v);
            lbl = anim_text(s, font_path, buf, 0.22f);
            anim_set_color(s, lbl, ANIM_GREY_B, true);
            anim_move_to(s, lbl, anim_axes_c2p(s, plane, v, 0.0f));
            anim_shift(s, lbl, ANIM_V3(0.0f, -0.24f, 0.0f));
            anim_add_child(s, plane, lbl);
        }
        for (v = ceilf(im_min / is) * is; v <= im_max + 1e-4f; v += is)
        {
            char buf[32];
            AnimId lbl;
            if (fabsf(v) < 1e-4f) continue;
            if (fabsf(v - 1.0f) < 1e-4f) snprintf(buf, sizeof buf, "i");
            else if (fabsf(v + 1.0f) < 1e-4f) snprintf(buf, sizeof buf, "-i");
            else snprintf(buf, sizeof buf, "%.0fi", (double)v);
            lbl = anim_text(s, font_path, buf, 0.22f);
            anim_set_color(s, lbl, ANIM_TEAL, true);
            anim_move_to(s, lbl, anim_axes_c2p(s, plane, 0.0f, v));
            anim_shift(s, lbl, ANIM_V3(-0.30f, 0.0f, 0.0f));
            anim_add_child(s, plane, lbl);
        }
    }
    return plane;
}

AnimVec3 anim_complex_c2p(const AnimScene *s, AnimId plane_id, float re, float im)
{
    return anim_axes_c2p(s, plane_id, re, im);
}

void anim_complex_p2c(const AnimScene *s, AnimId plane_id, AnimVec3 world_pt, float *out_re, float *out_im)
{
    anim_axes_p2c(s, plane_id, world_pt, out_re, out_im);
}

void anim_number_plane_prepare_nonlinear(AnimScene *s, AnimId plane_id, uint32_t cuts_per_cubic)
{
    anim_vmob_subdivide(s, plane_id, cuts_per_cubic > 1u ? cuts_per_cubic : 4u);
}

AnimId anim_cutout(AnimScene *s, AnimId outer, const AnimId *holes, uint32_t hole_count)
{
    AnimId         res = anim_vmob_create(s);
    const VMobHot *oh  = anim__hot_const(s, outer);
    uint32_t       si, ci, hi;

    if (!s || !ANIM_ID_VALID(res) || !oh)
        return res;

    for (si = 0u; si < oh->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[oh->subpath_offset + si];
        if (sm.cubic_count == 0u) continue;
        anim_vmob_begin_subpath(s, res, s->cubics[sm.cubic_offset].p0);
        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic c = s->cubics[sm.cubic_offset + ci];
            anim_vmob_cubic_to(s, res, c.p1, c.p2, c.p3);
        }
        if (sm.closed) anim_vmob_close_subpath(s, res);
    }

    for (hi = 0u; hi < hole_count; ++hi)
    {
        const VMobHot *hh = holes ? anim__hot_const(s, holes[hi]) : NULL;
        if (!hh) continue;
        for (si = 0u; si < hh->subpath_count; ++si)
        {
            SubpathMeta sm = s->subpaths[hh->subpath_offset + si];
            if (sm.cubic_count == 0u) continue;
            anim_vmob_begin_subpath(s, res, s->cubics[sm.cubic_offset + sm.cubic_count - 1u].p3);
            for (ci = sm.cubic_count; ci > 0u; --ci)
            {
                AnimCubic c = s->cubics[sm.cubic_offset + ci - 1u];
                anim_vmob_cubic_to(s, res, c.p2, c.p1, c.p0);
            }
            anim_vmob_close_subpath(s, res);
        }
    }

    anim_set_fill(s, res, oh->fill_color, false);
    anim_set_stroke(s, res, oh->stroke_color, oh->stroke_width, false);
    anim_set_fill_rule(s, res, ANIM_FILL_EVEN_ODD, false);
    return res;
}

AnimId anim_rounded_polygon(AnimScene *s, const AnimVec3 *pts, uint32_t count, float corner_radius)
{
    AnimId   id = anim_vmob_create(s);
    uint32_t i;
    float    r  = corner_radius > 0.0f ? corner_radius : 0.15f;

    if (!pts || count < 3u || !ANIM_ID_VALID(id))
        return id;

    for (i = 0u; i < count; ++i)
    {
        AnimVec3 prev = pts[(i + count - 1u) % count];
        AnimVec3 cur  = pts[i];
        AnimVec3 next = pts[(i + 1u) % count];
        AnimVec3 d0   = ANIM_V3(prev.x - cur.x, prev.y - cur.y, prev.z - cur.z);
        AnimVec3 d1   = ANIM_V3(next.x - cur.x, next.y - cur.y, next.z - cur.z);
        float    l0   = sqrtf(d0.x*d0.x + d0.y*d0.y + d0.z*d0.z);
        float    l1   = sqrtf(d1.x*d1.x + d1.y*d1.y + d1.z*d1.z);
        float    re   = MU_MIN(r, 0.45f * MU_MIN(l0, l1));
        AnimVec3 a, b;

        if (l0 < 1e-6f || l1 < 1e-6f) continue;
        a = ANIM_V3(cur.x + d0.x * (re / l0), cur.y + d0.y * (re / l0), cur.z + d0.z * (re / l0));
        b = ANIM_V3(cur.x + d1.x * (re / l1), cur.y + d1.y * (re / l1), cur.z + d1.z * (re / l1));
        if (i == 0u) anim_vmob_begin_subpath(s, id, a);
        else         anim_vmob_line_to(s, id, a);
        anim_vmob_quad_to(s, id, cur, b);
    }
    anim_vmob_close_subpath(s, id);
    anim_set_fill(s, id, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_stroke(s, id, ANIM_WHITE, ANIM_DEFAULT_STROKE_W, false);
    return id;
}

AnimId anim_elbow(AnimScene *s, float width, float angle_rad)
{
    float    w = width > 0.0f ? width : 0.25f;
    float    c = cosf(angle_rad), sn = sinf(angle_rad);
    AnimId   id = anim_vmob_create(s);
    AnimVec3 p0 = ANIM_V3(w, 0.0f, 0.0f);
    AnimVec3 p1 = ANIM_V3(w * (1.0f + c), w * sn, 0.0f);
    AnimVec3 p2 = ANIM_V3(w * c, w * sn, 0.0f);
    anim_vmob_begin_subpath(s, id, p0);
    anim_vmob_line_to(s, id, p1);
    anim_vmob_line_to(s, id, p2);
    anim_set_fill(s, id, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_stroke(s, id, ANIM_WHITE, ANIM_DEFAULT_STROKE_W, false);
    return id;
}

static void append_arc_between(AnimScene *s, AnimId id, AnimVec3 a, AnimVec3 b, float angle_rad, bool first)
{
    if (first) anim_vmob_begin_subpath(s, id, a);
    if (fabsf(angle_rad) < 1e-4f)
    {
        anim_vmob_line_to(s, id, b);
        return;
    }
    {
        AnimVec3 mid   = ANIM_V3(0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z));
        float    dx    = b.x - a.x, dy = b.y - a.y;
        float    chord = sqrtf(dx * dx + dy * dy);
        float    h     = chord / (2.0f * tanf(0.5f * angle_rad));
        float    nx    = -dy / MU_MAX(chord, 1e-6f);
        float    ny    =  dx / MU_MAX(chord, 1e-6f);
        AnimVec3 ctr   = ANIM_V3(mid.x + nx * h, mid.y + ny * h, mid.z);
        float    rad   = sqrtf((a.x - ctr.x)*(a.x - ctr.x) + (a.y - ctr.y)*(a.y - ctr.y));
        float    a0    = atan2f(a.y - ctr.y, a.x - ctr.x);
        uint32_t segs  = (uint32_t)MU_CLAMP(ceilf(fabsf(angle_rad) / (ANIM_PI * 0.45f)), 1.0f, 8.0f);
        float    da    = angle_rad / (float)segs;
        float    k     = (4.0f / 3.0f) * tanf(da * 0.25f);
        uint32_t i;
        for (i = 0u; i < segs; ++i)
        {
            float th0 = a0 + (float)i * da;
            float th1 = th0 + da;
            float c0 = cosf(th0), s0 = sinf(th0), c1 = cosf(th1), s1 = sinf(th1);
            AnimVec3 p1 = ANIM_V3(ctr.x + rad * (c0 - k * s0), ctr.y + rad * (s0 + k * c0), a.z);
            AnimVec3 p2 = ANIM_V3(ctr.x + rad * (c1 + k * s1), ctr.y + rad * (s1 - k * c1), a.z);
            AnimVec3 p3 = ANIM_V3(ctr.x + rad * c1,            ctr.y + rad * s1,            b.z);
            anim_vmob_cubic_to(s, id, p1, p2, p3);
        }
    }
}

AnimId anim_arc_polygon(AnimScene *s, const AnimVec3 *pts, const float *arc_angles, uint32_t count)
{
    AnimId   id = anim_vmob_create(s);
    uint32_t i;
    if (!pts || count < 2u || !ANIM_ID_VALID(id))
        return id;
    for (i = 0u; i < count; ++i)
    {
        float ang = arc_angles ? arc_angles[i] : 0.0f;
        append_arc_between(s, id, pts[i], pts[(i + 1u) % count], ang, i == 0u);
    }
    anim_vmob_close_subpath(s, id);
    anim_set_fill(s, id, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_stroke(s, id, ANIM_WHITE, ANIM_DEFAULT_STROKE_W, false);
    return id;
}

AnimId anim_dashed_vmobject(AnimScene *s, AnimId src_vmob, uint32_t num_dashes, float dashed_ratio)
{
    AnimId         res = anim_vmob_create(s);
    const VMobHot *sh  = anim__hot_const(s, src_vmob);
    uint32_t       nd  = num_dashes > 0u ? num_dashes : 15u;
    float          dr  = MU_CLAMP(dashed_ratio, 0.05f, 0.95f);
    uint32_t       d;

    if (!s || !ANIM_ID_VALID(res) || !sh)
        return res;

    for (d = 0u; d < nd; ++d)
    {
        float t0 = (float)d / (float)nd;
        float t1 = t0 + dr / (float)nd;
        uint32_t k;
        AnimVec3 p0 = anim_vmob_point_at(s, src_vmob, t0);
        anim_vmob_begin_subpath(s, res, p0);
        for (k = 1u; k <= 4u; ++k)
        {
            float tk = t0 + (t1 - t0) * ((float)k / 4.0f);
            anim_vmob_line_to(s, res, anim_vmob_point_at(s, src_vmob, tk));
        }
    }
    anim_set_fill(s, res, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), false);
    anim_set_stroke(s, res, sh->stroke_color.a > 0.0f ? sh->stroke_color : ANIM_WHITE,
                    sh->stroke_width > 0.0f ? sh->stroke_width : ANIM_DEFAULT_STROKE_W, false);
    return res;
}

AnimId anim_labeled_dot(AnimScene *s, const char *label, const char *font_path,
                        float radius, AnimColor bg_col, AnimColor fg_col)
{
    float  r   = radius > 0.0f ? radius : 0.35f;
    AnimId d   = anim_dot(s, ANIM_ORIGIN, r);
    AnimId txt = anim_text(s, font_path, label ? label : "A", r * 1.15f);
    AnimId kids[2];
    anim_set_fill(s, d, bg_col.a > 0.0f ? bg_col : ANIM_BLUE, false);
    anim_set_stroke(s, d, ANIM_WHITE, 2.0f, false);
    anim_set_color(s, txt, fg_col.a > 0.0f ? fg_col : ANIM_WHITE, true);
    anim_move_to(s, txt, ANIM_ORIGIN);
    kids[0] = d;
    kids[1] = txt;
    return anim_group(s, kids, 2u);
}

static AnimId build_arrow_tip(AnimScene *s, AnimVec3 tip, AnimVec3 dir, float tip_len, AnimColor col)
{
    float    len = sqrtf(dir.x*dir.x + dir.y*dir.y);
    float    ux  = len > 1e-6f ? dir.x / len : 1.0f;
    float    uy  = len > 1e-6f ? dir.y / len : 0.0f;
    float    px  = -uy, py = ux;
    float    tl  = tip_len > 0.0f ? tip_len : 0.22f;
    float    hw  = tl * 0.45f;
    AnimVec3 pts[3] = {
        tip,
        ANIM_V3(tip.x - ux * tl + px * hw, tip.y - uy * tl + py * hw, tip.z),
        ANIM_V3(tip.x - ux * tl - px * hw, tip.y - uy * tl - py * hw, tip.z),
    };
    AnimId tri = anim_polygon(s, pts, 3u);
    anim_set_fill(s, tri, col, false);
    anim_set_stroke(s, tri, col, 1.0f, false);
    return tri;
}

AnimId anim_curved_arrow(AnimScene *s, AnimVec3 start, AnimVec3 end, float angle_rad, float tip_len)
{
    AnimId   arc = anim_arc_between_points(s, start, end, angle_rad);
    AnimVec3 p0  = anim_vmob_point_at(s, arc, 0.96f);
    AnimVec3 p1  = anim_vmob_point_at(s, arc, 1.0f);
    AnimId   tip = build_arrow_tip(s, p1, ANIM_V3(p1.x - p0.x, p1.y - p0.y, 0.0f), tip_len, ANIM_WHITE);
    AnimId   kids[2] = { arc, tip };
    return anim_group(s, kids, 2u);
}

AnimId anim_curved_double_arrow(AnimScene *s, AnimVec3 start, AnimVec3 end, float angle_rad, float tip_len)
{
    AnimId   arc  = anim_arc_between_points(s, start, end, angle_rad);
    AnimVec3 a0   = anim_vmob_point_at(s, arc, 0.0f);
    AnimVec3 a1   = anim_vmob_point_at(s, arc, 0.04f);
    AnimVec3 b0   = anim_vmob_point_at(s, arc, 0.96f);
    AnimVec3 b1   = anim_vmob_point_at(s, arc, 1.0f);
    AnimId   t0   = build_arrow_tip(s, a0, ANIM_V3(a0.x - a1.x, a0.y - a1.y, 0.0f), tip_len, ANIM_WHITE);
    AnimId   t1   = build_arrow_tip(s, b1, ANIM_V3(b1.x - b0.x, b1.y - b0.y, 0.0f), tip_len, ANIM_WHITE);
    AnimId   kids[3] = { arc, t0, t1 };
    return anim_group(s, kids, 3u);
}

AnimId anim_integer(AnimScene *s, int value, const char *font_path, float height)
{
    return anim_decimal_number(s, (float)value, 0u, font_path, height);
}

typedef struct RevCtx {
    AnimParamFn fn;
    void       *user;
    float       t_min, t_max, theta_max;
} RevCtx;

static AnimVec3 rev_surf_eval(float u, float v, void *user)
{
    const RevCtx *rc = (const RevCtx *)user;
    AnimVec3      p  = rc->fn ? rc->fn(u, rc->user) : ANIM_V3(1.0f, u, 0.0f);
    return ANIM_V3(p.x * cosf(v), p.y, p.x * sinf(v));
}

AnimId anim_surface_of_revolution(AnimScene *s, AnimParamFn profile_fn, float t_min, float t_max,
                                  float theta_max, uint32_t t_steps, uint32_t theta_steps, void *user)
{
    RevCtx rc = { profile_fn, user, t_min, t_max, theta_max > 0.0f ? theta_max : ANIM_TAU };
    return anim_surface3d(s, rev_surf_eval, NULL, t_min, t_max, 0.0f, rc.theta_max,
                          t_steps > 2u ? t_steps : 24u, theta_steps > 3u ? theta_steps : 32u, &rc);
}

AnimId anim_extrude_vmob(AnimScene *s, AnimId profile_2d, float depth, AnimColor side_col)
{
    const VMobHot *ph = anim__hot_const(s, profile_2d);
    uint32_t       N  = 48u;
    float          hz = 0.5f * (depth > 0.0f ? depth : 0.6f);
    AnimVec3       ring[48];
    AnimVec3       ctr = ANIM_ORIGIN;
    SurfaceVert   *verts;
    uint32_t      *idxs;
    uint32_t       vc = 0u, ic = 0u, i;
    AnimColor      col = side_col.a > 0.0f ? side_col : (ph ? ph->fill_color : ANIM_BLUE);
    AnimId         res;

    if (!s || !ph)
        return anim_cube(s, 1.0f);

    if (col.a <= 0.0f) col = ANIM_BLUE;
    for (i = 0u; i < N; ++i)
    {
        ring[i] = anim_vmob_point_at(s, profile_2d, (float)i / (float)N);
        ctr.x += ring[i].x;
        ctr.y += ring[i].y;
    }
    ctr.x /= (float)N;
    ctr.y /= (float)N;

    verts = (SurfaceVert *)calloc(N * 4u + (N + 1u) * 2u, sizeof *verts);
    idxs  = (uint32_t *)calloc(N * 12u, sizeof *idxs);
    if (!verts || !idxs) { free(verts); free(idxs); return ANIM_ID_NIL; }

    /* Side wall quads */
    for (i = 0u; i < N; ++i)
    {
        AnimVec3 a  = ring[i];
        AnimVec3 b  = ring[(i + 1u) % N];
        float    dx = b.x - a.x, dy = b.y - a.y;
        float    nl = sqrtf(dx * dx + dy * dy);
        AnimVec3 n  = nl > 1e-6f ? ANIM_V3(dy / nl, -dx / nl, 0.0f) : ANIM_V3(1.0f, 0.0f, 0.0f);
        uint32_t v0 = vc;
        verts[vc++] = (SurfaceVert){ ANIM_V3(a.x, a.y,  hz), n, (float)i / (float)N, 0.0f, col };
        verts[vc++] = (SurfaceVert){ ANIM_V3(b.x, b.y,  hz), n, (float)(i + 1u) / (float)N, 0.0f, col };
        verts[vc++] = (SurfaceVert){ ANIM_V3(b.x, b.y, -hz), n, (float)(i + 1u) / (float)N, 1.0f, col };
        verts[vc++] = (SurfaceVert){ ANIM_V3(a.x, a.y, -hz), n, (float)i / (float)N, 1.0f, col };
        idxs[ic++] = v0 + 0u; idxs[ic++] = v0 + 1u; idxs[ic++] = v0 + 2u;
        idxs[ic++] = v0 + 0u; idxs[ic++] = v0 + 2u; idxs[ic++] = v0 + 3u;
    }

    /* Front (+hz) and back (-hz) caps */
    {
        uint32_t f_ctr = vc;
        verts[vc++] = (SurfaceVert){ ANIM_V3(ctr.x, ctr.y, hz), ANIM_V3(0.0f, 0.0f, 1.0f), 0.5f, 0.5f, col };
        for (i = 0u; i < N; ++i)
            verts[vc++] = (SurfaceVert){ ANIM_V3(ring[i].x, ring[i].y, hz), ANIM_V3(0.0f, 0.0f, 1.0f), 0.0f, 0.0f, col };
        for (i = 0u; i < N; ++i)
        {
            idxs[ic++] = f_ctr;
            idxs[ic++] = f_ctr + 1u + i;
            idxs[ic++] = f_ctr + 1u + ((i + 1u) % N);
        }
    }
    {
        uint32_t b_ctr = vc;
        verts[vc++] = (SurfaceVert){ ANIM_V3(ctr.x, ctr.y, -hz), ANIM_V3(0.0f, 0.0f, -1.0f), 0.5f, 0.5f, col };
        for (i = 0u; i < N; ++i)
            verts[vc++] = (SurfaceVert){ ANIM_V3(ring[i].x, ring[i].y, -hz), ANIM_V3(0.0f, 0.0f, -1.0f), 0.0f, 0.0f, col };
        for (i = 0u; i < N; ++i)
        {
            idxs[ic++] = b_ctr;
            idxs[ic++] = b_ctr + 1u + ((i + 1u) % N);
            idxs[ic++] = b_ctr + 1u + i;
        }
    }

    res = create_mesh3d_mob(s, verts, vc, idxs, ic, col);
    free(verts);
    free(idxs);
    return res;
}

/* ------------------------------------------- Wave 4: Callouts, Animated Streams, Graph Layouts, Pixel Arrays */

AnimId anim_callout(AnimScene *s, AnimVec3 target_pt, AnimVec3 box_center, const char *label, const char *font_path, AnimColor accent_color)
{
    AnimColor col   = accent_color.a > 0.0f ? accent_color : ANIM_YELLOW;
    const char *txt = (label && label[0]) ? label : "note";
    AnimId    lbl   = anim_text(s, font_path, txt, 0.28f);
    AnimVec3  mn, mx;
    float     bw, bh;
    AnimId    box, dot, leader, grp;
    AnimVec3  elbow_pt, edge_pt;
    float     dx, dy;
    AnimId    kids[4];

    anim_bbox_world(s, lbl, &mn, &mx);
    bw = MU_MAX((mx.x - mn.x) + 0.36f, 0.65f);
    bh = MU_MAX((mx.y - mn.y) + 0.24f, 0.44f);

    anim_move_to(s, lbl, ANIM_V3(box_center.x, box_center.y, box_center.z + 0.02f));
    anim_set_color(s, lbl, ANIM_WHITE, true);

    box = anim_rounded_rect(s, bw, bh, 0.09f);
    anim_move_to(s, box, box_center);
    anim_set_fill(s, box, anim_rgba(0.06f, 0.09f, 0.14f, 0.90f), false);
    anim_set_stroke(s, box, col, 2.2f, false);

    dot = anim_dot(s, target_pt, 0.065f);
    anim_set_fill(s, dot, col, false);

    dx = box_center.x - target_pt.x;
    dy = box_center.y - target_pt.y;
    if (fabsf(dx) >= fabsf(dy))
    {
        float sx = dx >= 0.0f ? -0.5f * bw : 0.5f * bw;
        edge_pt  = ANIM_V3(box_center.x + sx, box_center.y, box_center.z);
        elbow_pt = ANIM_V3(target_pt.x + 0.55f * (edge_pt.x - target_pt.x), edge_pt.y, 0.5f * (target_pt.z + box_center.z));
    }
    else
    {
        float sy = dy >= 0.0f ? -0.5f * bh : 0.5f * bh;
        edge_pt  = ANIM_V3(box_center.x, box_center.y + sy, box_center.z);
        elbow_pt = ANIM_V3(edge_pt.x, target_pt.y + 0.55f * (edge_pt.y - target_pt.y), 0.5f * (target_pt.z + box_center.z));
    }

    leader = anim_vmob_create(s);
    anim_vmob_begin_subpath(s, leader, target_pt);
    anim_vmob_line_to(s, leader, elbow_pt);
    anim_vmob_line_to(s, leader, edge_pt);
    anim_set_fill(s, leader, ANIM_CLEAR, false);
    anim_set_stroke(s, leader, col, 2.0f, false);

    kids[0] = leader;
    kids[1] = dot;
    kids[2] = box;
    kids[3] = lbl;
    grp = anim_group(s, kids, 4u);
    return grp;
}

AnimId anim_dimension_line(AnimScene *s, AnimVec3 a, AnimVec3 b, float offset, const char *label, const char *font_path, AnimColor color)
{
    AnimColor col  = color.a > 0.0f ? color : ANIM_TEAL;
    AnimVec3  d    = v3_sub(b, a);
    float     len  = v3_len(d);
    AnimVec3  u    = len > 1e-5f ? v3_mul(d, 1.0f / len) : ANIM_RIGHT;
    AnimVec3  n    = ANIM_V3(-u.y, u.x, 0.0f);
    float     off  = fabsf(offset) > 1e-4f ? offset : 0.45f;
    float     sgn  = off >= 0.0f ? 1.0f : -1.0f;
    AnimVec3  a1   = v3_add(a, v3_mul(n, off));
    AnimVec3  b1   = v3_add(b, v3_mul(n, off));
    AnimVec3  a2   = v3_add(a1, v3_mul(n, 0.14f * sgn));
    AnimVec3  b2   = v3_add(b1, v3_mul(n, 0.14f * sgn));
    AnimId    ext_a = anim_line(s, v3_add(a, v3_mul(n, 0.05f * sgn)), a2);
    AnimId    ext_b = anim_line(s, v3_add(b, v3_mul(n, 0.05f * sgn)), b2);
    AnimId    arr   = anim_double_arrow(s, a1, b1, 0.13f, 0.09f);
    char      auto_buf[32];
    const char *txt;
    AnimId    lbl, pill, grp;
    AnimVec3  mid, lpos, mn, mx;
    AnimId    kids[5];

    if (label && label[0])
        txt = label;
    else
    {
        snprintf(auto_buf, sizeof auto_buf, "%.2f", (double)len);
        txt = auto_buf;
    }

    anim_set_stroke(s, ext_a, anim_rgba(col.r, col.g, col.b, col.a * 0.55f), 1.4f, false);
    anim_set_stroke(s, ext_b, anim_rgba(col.r, col.g, col.b, col.a * 0.55f), 1.4f, false);
    anim_set_color(s, arr, col, true);
    anim_set_stroke(s, arr, col, 2.2f, true);

    mid  = v3_mul(v3_add(a1, b1), 0.5f);
    lpos = v3_add(mid, v3_mul(n, 0.24f * sgn));
    lbl  = anim_text(s, font_path, txt, 0.24f);
    anim_set_color(s, lbl, col, true);
    anim_move_to(s, lbl, ANIM_V3(lpos.x, lpos.y, lpos.z + 0.02f));

    anim_bbox_world(s, lbl, &mn, &mx);
    pill = anim_rounded_rect(s, MU_MAX(mx.x - mn.x + 0.16f, 0.30f), MU_MAX(mx.y - mn.y + 0.10f, 0.22f), 0.06f);
    anim_move_to(s, pill, lpos);
    anim_set_fill(s, pill, anim_rgba(0.05f, 0.07f, 0.11f, 0.85f), false);
    anim_set_stroke(s, pill, ANIM_CLEAR, 0.0f, false);

    kids[0] = ext_a;
    kids[1] = ext_b;
    kids[2] = arr;
    kids[3] = pill;
    kids[4] = lbl;
    grp = anim_group(s, kids, 5u);
    return grp;
}

typedef struct AnimatedStreamDesc {
    float flow_speed;
} AnimatedStreamDesc;

static void animated_stream_updater(AnimScene *s, AnimId target, float dt, void *user)
{
    AnimatedStreamDesc *ad = (AnimatedStreamDesc *)user;
    const VMobHot      *gh = anim__hot_const(s, target);
    uint32_t            ch;
    float               delta;

    if (!ad || !gh || dt == 0.0f)
        return;
    delta = dt * ad->flow_speed * 64.0f;
    ch = gh->first_child;
    while (ch != 0u)
    {
        VMobHot *ch_hot = &s->hot[ch];
        ch_hot->dash_offset_px -= delta;
        ch = ch_hot->next_sibling;
    }
}

AnimId anim_animated_stream_lines(AnimScene *s, AnimId axes_id, AnimFieldFn fn,
                                  float x_min, float x_max, float y_min, float y_max,
                                  float x_step, float y_step, uint32_t max_steps,
                                  float flow_speed, void *user)
{
    AnimId grp = anim_stream_lines(s, axes_id, fn, x_min, x_max, y_min, y_max, x_step, y_step, max_steps, user);
    AnimatedStreamDesc *ad;

    if (!ANIM_ID_VALID(grp))
        return grp;
    ad = (AnimatedStreamDesc *)malloc(sizeof *ad);
    if (!ad)
        return grp;
    ad->flow_speed = fabsf(flow_speed) > 1e-4f ? flow_speed : 1.0f;
    anim_add_updater(s, grp, animated_stream_updater, ad, NULL, 0u, NULL, 0u);
    return grp;
}

void anim_graph_compute_layout(AnimGraphLayoutKind layout, uint32_t node_count,
                               uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to,
                               float width, float height, uint32_t seed, AnimVec3 *out_positions)
{
    uint32_t i;
    float    w = width > 0.2f ? width : 7.0f;
    float    h = height > 0.2f ? height : 5.0f;

    if (!out_positions || node_count == 0u)
        return;

    if (node_count == 1u)
    {
        out_positions[0] = ANIM_ORIGIN;
        return;
    }

    switch (layout)
    {
    case ANIM_GRAPH_LAYOUT_CIRCULAR:
    default:
        for (i = 0u; i < node_count; ++i)
        {
            float ang = ANIM_PI * 0.5f - ANIM_TAU * ((float)i / (float)node_count);
            out_positions[i] = ANIM_V3(0.44f * w * cosf(ang), 0.44f * h * sinf(ang), 0.0f);
        }
        break;

    case ANIM_GRAPH_LAYOUT_GRID:
    {
        uint32_t cols = (uint32_t)ceilf(sqrtf((float)node_count));
        uint32_t rows = (node_count + cols - 1u) / cols;
        float    dx   = cols > 1u ? w / (float)(cols - 1u) : 0.0f;
        float    dy   = rows > 1u ? h / (float)(rows - 1u) : 0.0f;
        for (i = 0u; i < node_count; ++i)
        {
            uint32_t r = i / cols;
            uint32_t c = i % cols;
            out_positions[i] = ANIM_V3(-0.5f * w + (float)c * dx, 0.5f * h - (float)r * dy, 0.0f);
        }
        break;
    }

    case ANIM_GRAPH_LAYOUT_BIPARTITE:
    {
        int      *part   = (int *)malloc((size_t)node_count * sizeof *part);
        uint32_t *queue  = (uint32_t *)malloc((size_t)node_count * sizeof *queue);
        uint32_t  left_n = 0u, right_n = 0u, li = 0u, ri = 0u;
        for (i = 0u; i < node_count; ++i) part[i] = -1;
        for (uint32_t s_i = 0u; s_i < node_count; ++s_i)
        {
            uint32_t qh = 0u, qt = 0u;
            if (part[s_i] >= 0) continue;
            part[s_i] = 0;
            queue[qt++] = s_i;
            while (qh < qt)
            {
                uint32_t u = queue[qh++];
                for (uint32_t e = 0u; e < edge_count; ++e)
                {
                    uint32_t a = edge_from ? edge_from[e] : 0u;
                    uint32_t b = edge_to ? edge_to[e] : 0u;
                    uint32_t v = UINT32_MAX;
                    if (a == u && b < node_count) v = b;
                    else if (b == u && a < node_count) v = a;
                    if (v != UINT32_MAX && part[v] < 0)
                    {
                        part[v] = 1 - part[u];
                        queue[qt++] = v;
                    }
                }
            }
        }
        for (i = 0u; i < node_count; ++i)
        {
            if (part[i] == 0) left_n++;
            else              right_n++;
        }
        for (i = 0u; i < node_count; ++i)
        {
            if (part[i] == 0)
            {
                float y = left_n > 1u ? (0.42f * h) * (1.0f - 2.0f * (float)li / (float)(left_n - 1u)) : 0.0f;
                out_positions[i] = ANIM_V3(-0.34f * w, y, 0.0f);
                li++;
            }
            else
            {
                float y = right_n > 1u ? (0.42f * h) * (1.0f - 2.0f * (float)ri / (float)(right_n - 1u)) : 0.0f;
                out_positions[i] = ANIM_V3(0.34f * w, y, 0.0f);
                ri++;
            }
        }
        free(part);
        free(queue);
        break;
    }

    case ANIM_GRAPH_LAYOUT_TREE:
    {
        int      *depth = (int *)malloc((size_t)node_count * sizeof *depth);
        uint32_t *queue = (uint32_t *)malloc((size_t)node_count * sizeof *queue);
        uint32_t *cnt   = (uint32_t *)calloc((size_t)node_count, sizeof *cnt);
        uint32_t *cur   = (uint32_t *)calloc((size_t)node_count, sizeof *cur);
        int       max_d = 0;

        for (i = 0u; i < node_count; ++i) depth[i] = -1;
        for (uint32_t s_i = 0u; s_i < node_count; ++s_i)
        {
            uint32_t qh = 0u, qt = 0u;
            if (depth[s_i] >= 0) continue;
            depth[s_i] = 0;
            queue[qt++] = s_i;
            while (qh < qt)
            {
                uint32_t u = queue[qh++];
                if (depth[u] > max_d) max_d = depth[u];
                for (uint32_t e = 0u; e < edge_count; ++e)
                {
                    uint32_t a = edge_from ? edge_from[e] : 0u;
                    uint32_t b = edge_to ? edge_to[e] : 0u;
                    uint32_t v = UINT32_MAX;
                    if (a == u && b < node_count) v = b;
                    else if (b == u && a < node_count) v = a;
                    if (v != UINT32_MAX && depth[v] < 0)
                    {
                        depth[v] = depth[u] + 1;
                        queue[qt++] = v;
                    }
                }
            }
        }
        for (i = 0u; i < node_count; ++i)
            cnt[depth[i]]++;
        for (i = 0u; i < node_count; ++i)
        {
            int      d = depth[i];
            uint32_t n_at_d = cnt[d];
            uint32_t idx    = cur[d]++;
            float    x = n_at_d > 1u ? (-0.44f * w + (0.88f * w) * ((float)idx / (float)(n_at_d - 1u))) : 0.0f;
            float    y = max_d > 0 ? (0.42f * h - (0.84f * h) * ((float)d / (float)max_d)) : 0.0f;
            out_positions[i] = ANIM_V3(x, y, 0.0f);
        }
        free(depth);
        free(queue);
        free(cnt);
        free(cur);
        break;
    }

    case ANIM_GRAPH_LAYOUT_SPRING:
    {
        float    area = w * h;
        float    k    = sqrtf(area / (float)node_count) * 0.85f;
        AnimVec3 *disp = (AnimVec3 *)calloc((size_t)node_count, sizeof *disp);
        uint32_t rng  = seed ? seed : 0xA511E9B3u;
        float    temp = 0.25f * MU_MAX(w, h);
        uint32_t it;

        for (i = 0u; i < node_count; ++i)
        {
            float ang = ANIM_TAU * ((float)i / (float)node_count);
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float jx = (((float)(rng & 0xFFFFu) / 65535.0f) - 0.5f) * 0.15f * w;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float jy = (((float)(rng & 0xFFFFu) / 65535.0f) - 0.5f) * 0.15f * h;
            out_positions[i] = ANIM_V3(0.38f * w * cosf(ang) + jx, 0.38f * h * sinf(ang) + jy, 0.0f);
        }

        for (it = 0u; it < 80u; ++it)
        {
            uint32_t u, v, e;
            for (i = 0u; i < node_count; ++i)
                disp[i] = ANIM_ORIGIN;

            for (u = 0u; u < node_count; ++u)
            {
                for (v = u + 1u; v < node_count; ++v)
                {
                    float dx = out_positions[u].x - out_positions[v].x;
                    float dy = out_positions[u].y - out_positions[v].y;
                    float d  = sqrtf(dx * dx + dy * dy);
                    float f;
                    if (d < 1e-4f) { dx = 0.01f; dy = 0.01f; d = 0.01414f; }
                    f = (k * k) / d;
                    disp[u].x += (dx / d) * f; disp[u].y += (dy / d) * f;
                    disp[v].x -= (dx / d) * f; disp[v].y -= (dy / d) * f;
                }
            }

            for (e = 0u; e < edge_count; ++e)
            {
                u = edge_from ? edge_from[e] : 0u;
                v = edge_to ? edge_to[e] : 0u;
                if (u < node_count && v < node_count && u != v)
                {
                    float dx = out_positions[u].x - out_positions[v].x;
                    float dy = out_positions[u].y - out_positions[v].y;
                    float d  = sqrtf(dx * dx + dy * dy);
                    if (d > 1e-4f)
                    {
                        float f = (d * d) / k;
                        disp[u].x -= (dx / d) * f; disp[u].y -= (dy / d) * f;
                        disp[v].x += (dx / d) * f; disp[v].y += (dy / d) * f;
                    }
                }
            }

            for (i = 0u; i < node_count; ++i)
            {
                float d = sqrtf(disp[i].x * disp[i].x + disp[i].y * disp[i].y);
                if (d > 1e-5f)
                {
                    float step = MU_MIN(d, temp);
                    out_positions[i].x = MU_CLAMP(out_positions[i].x + (disp[i].x / d) * step, -0.46f * w, 0.46f * w);
                    out_positions[i].y = MU_CLAMP(out_positions[i].y + (disp[i].y / d) * step, -0.46f * h, 0.46f * h);
                }
            }
            temp *= 0.94f;
        }
        free(disp);
        break;
    }
    }
}

AnimId anim_graph_with_layout(AnimScene *s, uint32_t node_count, const char *const *labels,
                              uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to,
                              bool directed, AnimGraphLayoutKind layout, float width, float height, const char *font_path)
{
    AnimVec3 *pos;
    AnimId    g;

    if (!s || node_count == 0u)
        return anim_group(s, NULL, 0u);
    pos = (AnimVec3 *)malloc((size_t)node_count * sizeof *pos);
    if (!pos)
        return ANIM_ID_NIL;
    anim_graph_compute_layout(layout, node_count, edge_count, edge_from, edge_to, width, height, 1337u, pos);
    g = anim_graph_network(s, node_count, pos, labels, edge_count, edge_from, edge_to, directed, font_path);
    free(pos);
    return g;
}

AnimId anim_pixel_array_grid(AnimScene *s, const AnimColor *pixels, uint32_t cols, uint32_t rows, float cell_size, float gap)
{
    uint32_t nc  = MU_CLAMP(cols > 0u ? cols : 4u, 1u, 64u);
    uint32_t nr  = MU_CLAMP(rows > 0u ? rows : 4u, 1u, 64u);
    float    cs  = cell_size > 0.0f ? cell_size : 0.35f;
    float    gp  = gap >= 0.0f ? gap : 0.0f;
    float    st  = cs + gp;
    float    ox  = -0.5f * (float)nc * st + 0.5f * st;
    float    oy  =  0.5f * (float)nr * st - 0.5f * st;
    AnimId  *kids = (AnimId *)malloc((size_t)(nc * nr) * sizeof *kids);
    uint32_t r, c, k = 0u;
    AnimId   grp;

    if (!kids)
        return ANIM_ID_NIL;
    for (r = 0u; r < nr; ++r)
    {
        for (c = 0u; c < nc; ++c)
        {
            AnimColor col  = pixels ? pixels[r * nc + c] : ANIM_WHITE;
            AnimId    cell = anim_square(s, cs);
            anim_move_to(s, cell, ANIM_V3(ox + (float)c * st, oy - (float)r * st, 0.0f));
            anim_set_fill(s, cell, col, false);
            anim_set_stroke(s, cell, ANIM_CLEAR, 0.0f, false);
            kids[k++] = cell;
        }
    }
    grp = anim_group(s, kids, k);
    free(kids);
    return grp;
}

