/* shaders/anim.slang — GPU analytical Bezier bands, Z-buffered 3D lit surfaces & HDR bloom.
 * ============================================================================
 *  Matches vk.h descriptor ABI (bindings 0..3 in set 0) and pulls band,
 *  shape, quadratic Bezier segment, and 3D surface vertex records from
 *  ARENA_TRANSIENT via BDA.
 *  Viewport has negative height (vk.c), so clip +Y is up and pixel (0,0) is
 *  the top-left corner: SV_Position.xy gives pixel centers (x + 0.5, y + 0.5).
 * ==========================================================================*/
#include "../third_party/mu_new/external/mu_lib/mu/shaders/common.slang"
#include "../third_party/mu_new/external/mu_lib/mu/shaders/dispatch.slang"
#include "../third_party/mu_new/external/mu_lib/mu/shaders/post.slang"
#include "../third_party/mu_new/external/mu_lib/mu/shaders/format.slang"

[[vk::binding(0, 0)]] Texture2D         g_textures[];
[[vk::binding(1, 0)]] SamplerState      g_samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_data;

/* 32 bytes: one tight horizontal band of one VMobject (fill or stroke). */
struct AnimBand
{
    float4 bounds; /* xy = min_px, zw = max_px                                */
    uint4  meta;   /* x = seg_offset, y = seg_count, z = shape_idx, w = flags */
};

/* 160 bytes: per-VMobject linear HDR style, gradients, dashes, and clip box. */
struct AnimShape
{
    float4 fill_color;   /* linear RGBA stop 0                                */
    float4 fill_color2;  /* linear RGBA stop 1 (a < 0 => solid fill_color)    */
    float4 stroke_color; /* linear RGBA                                       */
    float4 style;        /* x = glow, y = sheen, z = sheen_angle, w = shadow  */
    float4 box;          /* xy = min_px, zw = max_px                          */
    float4 grad_params;  /* xy = p0_px / center_px, zw = p1_px / radius_px    */
    float4 dash_params;  /* x = dash_px, y = gap_px, z = offset_px, w = gmode */
    float4 clip_box;     /* xy = clip_min_px, zw = clip_max_px                */
    float4 sheen_color;  /* optional sheen tint RGBA (a <= 0 => default white)*/
    float4 extra;        /* x = grad_idx+1, y = shape_z_ndc, zw = reserved    */
};

/* 144 bytes: optional multi-stop (up to 8) linear/radial gradient */
struct AnimGrad
{
    float4 stops[8];
    uint4  meta; /* x = stop_count */
};

/* 96 bytes: Y-monotonic (for fill) or general (for stroke) quadratic Bezier. */
struct AnimQuadSeg
{
    float4 p0_p1;   /* xy = p0 in px, zw = p1 in px                           */
    float4 p2_w;    /* xy = p2 in px, z = half_w0 in px, w = half_w1 in px    */
    float4 c0;      /* linear RGBA at t=0 (a < 0 => use shape.stroke_color)   */
    float4 c1;      /* linear RGBA at t=1                                     */
    float4 arc_len; /* x = s0_px, y = s1_px, z = z_ndc0, w = z_ndc1           */
    uint4  meta;    /* x = seg_flags (bit0 first_open, bit1 last_open, 2..3 cap)*/
};

/* 48 bytes: vertex for Z-buffered lit 3D parametric surfaces. */
struct GpuSurfaceVert
{
    float4 pos_u;  /* xyz = world position, w = parametric u                  */
    float4 norm_v; /* xyz = world normal,   w = parametric v                  */
    float4 color;  /* linear RGBA                                             */
};

/* 32 bytes: one scene light (directional or point). */
struct GpuLight
{
    float4 pos_type;  /* xyz = dir or world pos, w = 0 dir / 1 point         */
    float4 color_int; /* rgb = linear color, w = intensity                     */
};

/* 176 bytes: scene lighting + fog descriptor passed via BDA. */
struct GpuSceneLighting
{
    GpuLight lights[4];
    float4   ambient;    /* rgb = ambient linear color, w = light_count        */
    float4   fog_color;  /* rgb = fog linear color, w = fog_density            */
    float4   fog_params; /* x = start, y = end, z = mode (0=off,1=lin,2=exp2)  */
};

struct AnimPush
{
    AnimBand    *bands;
    AnimShape   *shapes;
    AnimQuadSeg *segs;
    AnimGrad    *grads;
    float4       viewport; /* xy = width, height; zw = 1/width, 1/height      */
    float4       bg_top;   /* linear RGBA                                     */
    float4       bg_bot;   /* linear RGBA                                     */
    float4       hud;      /* x = progress, y = playing, z = show, w = time   */
};
[[vk::push_constant]] AnimPush push;

struct SurfacePush
{
    GpuSurfaceVert   *verts;
    GpuSceneLighting *lighting;
    float4x4          mvp;
    float4            cam_pos_grid;   /* xyz = camera pos, w = grid density     */
    float4            style_params;   /* x = opacity, y = glow, z = grid_alpha, w = roughness */
    uint4             tex_params;     /* x = tex_id, y = sampler_id, z = textured, w = unlit */
    float4            checker_c1;     /* checkerboard color 1                   */
    float4            checker_c2;     /* checkerboard color 2                   */
    float4            checker_uv;     /* x = u_divs, y = v_divs, z = enabled    */
};
[[vk::push_constant]] SurfacePush surf_push;

struct PostPush
{
    float4 texel;          /* xy = 1/src_size, zw = 1/dst_size                */
    float4 grade;          /* x = exposure, y = bloom_str, z = sat, w = vig   */
    float4 bloom;          /* x = threshold, y = knee, z = alpha_out, w = weight */
    float4 hud;            /* x = progress, y = playing, z = show, w = hover  */
    float4 fx;             /* x = aberration_px, y = grain, z = tonemap, w = time */
    uint   color_map;
    uint   bloom_map;
    uint   linear_sampler;
    uint   depth_map;
    float4 dof;            /* x = aperture_px, y = focus_dist, z = focus_range, w = ortho_blend */
};
[[vk::push_constant]] PostPush post_push;

struct ScreenVaryings
{
    float4 clip : SV_Position;
    float2 uv   : TEXCOORD0;
};

struct BandVaryings
{
    float4               clip     : SV_Position;
    nointerpolation uint band_idx : BAND0;
};

struct SurfaceVaryings
{
    float4 clip       : SV_Position;
    float3 world_pos  : TEXCOORD0;
    float3 world_norm : TEXCOORD1;
    float2 surf_uv    : TEXCOORD2;
    float4 color      : TEXCOORD3;
};

/* ---------------------------------------------------------------- helpers */

float2 px_to_ndc(float2 px, float2 inv_vp)
{
    float2 uv = px * inv_vp;
    return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

/* Analytical distance to quadratic Bezier (p0, p1, p2).
 * Returns float3(distance, clamped_t, unclamped_raw_t_approx). */
float3 sd_quad_bezier(float2 pos, float2 A, float2 B, float2 C)
{
    float2 a  = B - A;
    float2 b  = A - 2.0 * B + C;
    float2 c  = a * 2.0;
    float2 d  = A - pos;
    float  bb = dot(b, b);

    if (bb < 1e-5)
    {
        float2 seg = C - A;
        float  ll  = dot(seg, seg);
        float  raw = ll > 1e-8 ? dot(pos - A, seg) / ll : 0.0;
        float  t   = saturate(raw);
        float2 q   = lerp(A, C, t);
        return float3(length(pos - q), t, raw);
    }

    float  kk = 1.0 / bb;
    float  kx = kk * dot(a, b);
    float  ky = kk * (2.0 * dot(a, a) + dot(d, b)) / 3.0;
    float  kz = kk * dot(d, a);
    float  p  = ky - kx * kx;
    float  p3 = p * p * p;
    float  q  = kx * (2.0 * kx * kx - 3.0 * ky) + kz;
    float  h  = q * q + 4.0 * p3;

    float  best_d2 = 1e30;
    float  best_t  = 0.0;
    float  raw_t   = 0.5;

    if (h >= 0.0)
    {
        h          = sqrt(h);
        float2 x   = (float2(h, -h) - q) * 0.5;
        float2 uv  = sign(x) * pow(abs(x), 1.0 / 3.0);
        raw_t      = uv.x + uv.y - kx;
        best_t     = saturate(raw_t);
        float2 vec = d + (c + b * best_t) * best_t;
        best_d2    = dot(vec, vec);
    }
    else
    {
        float  z  = sqrt(max(-p, 1e-12));
        float  v  = acos(clamp(q / (p * z * 2.0), -1.0, 1.0)) / 3.0;
        float  m  = cos(v);
        float  n  = sin(v) * 1.732050808;
        float3 r  = float3(m + m, -n - m, n - m) * z - kx;
        float3 rc = saturate(r);

        [ForceUnroll]
        for (int k = 0; k < 3; ++k)
        {
            float2 vec = d + (c + b * rc[k]) * rc[k];
            float  d2  = dot(vec, vec);
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best_t  = rc[k];
                raw_t   = r[k];
            }
        }
    }

    return float3(sqrt(best_d2), best_t, raw_t);
}

float3 apply_sheen(float3 rgb, float2 pos, AnimShape shape)
{
    if (shape.style.y <= 0.0001)
        return rgb;
    float2 sz   = max(shape.box.zw - shape.box.xy, float2(1.0, 1.0));
    float2 rel  = (pos - shape.box.xy) / sz - 0.5;
    float  ang  = shape.style.z;
    float  proj = rel.x * cos(ang) + rel.y * sin(ang);
    float  ramp = saturate(proj + 0.5);
    if (shape.sheen_color.a > 0.001)
    {
        float band = pow(max(1.0 - abs(proj) * 2.0, 0.0), 1.6) * shape.style.y * shape.sheen_color.a;
        return lerp(rgb, shape.sheen_color.rgb, saturate(band)) * lerp(1.0 - shape.style.y * 0.25, 1.0 + shape.style.y * 0.55, ramp);
    }
    return rgb * lerp(1.0 - shape.style.y * 0.45, 1.0 + shape.style.y * 0.75, ramp);
}

float4 sample_grad_stops(float t, AnimGrad grad)
{
    uint n = clamp(grad.meta.x, 2u, 8u);
    float scaled = saturate(t) * float(n - 1u);
    uint idx = min(uint(floor(scaled)), n - 2u);
    float frac_t = scaled - float(idx);
    return lerp(grad.stops[idx], grad.stops[idx + 1u], frac_t);
}

float4 eval_fill_gradient(float2 pos, AnimShape shape)
{
    int gmode = int(shape.dash_params.w + 0.5);
    if (gmode == 0)
        return shape.fill_color;

    float t;
    if (gmode == 1)
    {
        float2 p0 = shape.grad_params.xy;
        float2 p1 = shape.grad_params.zw;
        float2 d  = p1 - p0;
        float  ll = max(dot(d, d), 1e-5);
        t         = saturate(dot(pos - p0, d) / ll);
    }
    else
    {
        float2 ctr = shape.grad_params.xy;
        float  rad = max(shape.grad_params.z, 1.0);
        t          = saturate(length(pos - ctr) / rad);
    }

    uint gidx = uint(shape.extra.x + 0.5);
    if (gidx > 0u)
        return sample_grad_stops(t, push.grads[gidx - 1u]);
    if (shape.fill_color2.a < 0.0)
        return shape.fill_color;
    return lerp(shape.fill_color, shape.fill_color2, t);
}

/* ------------------------------------------------------------ background */

[shader("vertex")]
ScreenVaryings vs_screen(uint vertex_id : SV_VertexID)
{
    ScreenVaryings o;
    o.uv   = mu_fullscreen_triangle_uv(vertex_id);
    o.clip = float4(mu_fullscreen_triangle_pos(vertex_id).xy, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fs_bg(ScreenVaryings i) : SV_Target0
{
    float4 col = lerp(push.bg_top, push.bg_bot, i.uv.y);
    float2 d   = i.uv - 0.5;
    float  vig = 1.0 - dot(d, d) * 0.25;
    return float4(col.rgb * vig * col.a, col.a);
}

/* ------------------------------------------------------ 3D lit surfaces */

[shader("vertex")]
SurfaceVaryings vs_surface3d(uint vertex_id : SV_VertexID)
{
    GpuSurfaceVert  v = surf_push.verts[vertex_id];
    SurfaceVaryings o;
    o.world_pos  = v.pos_u.xyz;
    o.world_norm = v.norm_v.xyz;
    o.surf_uv    = float2(v.pos_u.w, v.norm_v.w);
    o.color      = v.color;
    o.clip       = mul(surf_push.mvp, float4(v.pos_u.xyz, 1.0));
    return o;
}

[shader("fragment")]
float4 fs_surface3d(SurfaceVaryings i, bool is_front : SV_IsFrontFace) : SV_Target0
{
    float3 N = normalize(i.world_norm);
    if (!is_front)
        N = -N;

    float3 V  = normalize(surf_push.cam_pos_grid.xyz - i.world_pos);
    float3 L1 = normalize(float3(0.45, 0.72, 0.53));
    float3 L2 = normalize(float3(-0.62, -0.30, 0.72));
    float3 R  = reflect(-V, N);

    float  rough = clamp(surf_push.style_params.w, 0.08, 0.98);
    float  alpha_r = rough * rough;
    float  a2    = alpha_r * alpha_r;
    float  ndotv = max(dot(N, V), 1e-4);

    /* Key + fill Cook-Torrance GGX specular + Schlick Fresnel */
    float3 H1     = normalize(L1 + V);
    float  ndotl1 = max(dot(N, L1), 0.0);
    float  ndoth1 = max(dot(N, H1), 0.0);
    float  denom1 = ndoth1 * ndoth1 * (a2 - 1.0) + 1.0;
    float  D1     = a2 / (3.14159265 * denom1 * denom1 + 1e-5);
    float  k_g    = (rough + 1.0) * (rough + 1.0) * 0.125;
    float  G1     = (ndotv / (ndotv * (1.0 - k_g) + k_g)) * (ndotl1 / (ndotl1 * (1.0 - k_g) + k_g + 1e-5));
    float  F1     = 0.04 + 0.96 * pow(1.0 - max(dot(H1, V), 0.0), 5.0);
    float  spec1  = (D1 * G1 * F1) / (4.0 * ndotv * max(ndotl1, 1e-4));

    float  ndotl2 = max(dot(N, L2), 0.0);
    float  fres   = pow(1.0 - ndotv, 3.4) * 0.42;

    float4 base_col = i.color;
    if (surf_push.checker_uv.z > 0.5)
    {
        int iu = int(floor(i.surf_uv.x * max(surf_push.checker_uv.x, 1.0)));
        int iv = int(floor(i.surf_uv.y * max(surf_push.checker_uv.y, 1.0)));
        base_col *= (((iu + iv) & 1) == 0) ? surf_push.checker_c1 : surf_push.checker_c2;
    }
    if (surf_push.tex_params.z != 0u)
    {
        float4 tex = g_textures[NonUniformResourceIndex(surf_push.tex_params.x)].Sample(
            g_samplers[NonUniformResourceIndex(surf_push.tex_params.y)], i.surf_uv);
        base_col *= tex;
    }

    float glow_mult = 1.0 + surf_push.style_params.y * 1.8;
    float alpha     = saturate(base_col.a * surf_push.style_params.x);
    if (alpha < 0.005)
        discard;

    float3 lit;
    if (surf_push.tex_params.w != 0u)
    {
        lit = base_col.rgb;
    }
    else
    {
        /* Studio sky/ground environment reflection + configurable or default lighting */
        float3 env_refl = lerp(float3(0.06, 0.08, 0.14), float3(0.32, 0.42, 0.62), smoothstep(-0.35, 0.75, R.y));
        float3 hemi     = lerp(float3(0.08, 0.10, 0.16), float3(0.24, 0.30, 0.40), N.y * 0.5 + 0.5);
        bool   has_lbuf = (surf_push.checker_uv.w > 0.5);
        uint   lcount   = has_lbuf ? min(uint(surf_push.lighting.ambient.w + 0.5), 4u) : 0u;
        if (lcount > 0u)
        {
            float3 amb  = surf_push.lighting.ambient.rgb * (0.65 + 0.35 * (N.y * 0.5 + 0.5));
            float3 diff = amb;
            float3 spec = float3(0.0, 0.0, 0.0);
            for (uint li = 0u; li < 4u; ++li)
            {
                if (li >= lcount)
                    break;
                GpuLight gl    = surf_push.lighting.lights[li];
                float3   lvec  = (gl.pos_type.w > 0.5) ? (gl.pos_type.xyz - i.world_pos) : gl.pos_type.xyz;
                float    dist2 = max(dot(lvec, lvec), 1e-4);
                float3   L     = lvec / sqrt(dist2);
                float    att   = (gl.pos_type.w > 0.5) ? (1.0 / (1.0 + 0.06 * dist2)) : 1.0;
                float    ndl   = max(dot(N, L), 0.0);
                float3   H     = normalize(L + V);
                float    ndh   = max(dot(N, H), 0.0);
                float    den   = ndh * ndh * (a2 - 1.0) + 1.0;
                float    D     = a2 / (3.14159265 * den * den + 1e-5);
                float    G     = (ndotv / (ndotv * (1.0 - k_g) + k_g)) * (ndl / (ndl * (1.0 - k_g) + k_g + 1e-5));
                float    F     = 0.04 + 0.96 * pow(1.0 - max(dot(H, V), 0.0), 5.0);
                float    sp    = (D * G * F) / (4.0 * ndotv * max(ndl, 1e-4));
                float3   lcol  = gl.color_int.rgb * (gl.color_int.w * att);
                diff += lcol * ndl;
                spec += lcol * (sp * ndl * 0.72);
            }
            lit = base_col.rgb * diff + spec + env_refl * (fres * lerp(0.85, 0.25, rough));
        }
        else
        {
            lit = base_col.rgb * (hemi + ndotl1 * 0.88 + ndotl2 * 0.34)
                + float3(spec1 * ndotl1 * 0.72)
                + env_refl * (fres * lerp(0.85, 0.25, rough));
        }

        /* Anti-aliased parametric (u, v) wireframe grid via screen-space derivatives */
        float grid_n = surf_push.cam_pos_grid.w;
        float grid_a = surf_push.style_params.z;
        if (grid_n > 0.5 && grid_a > 0.001)
        {
            float2 guv   = i.surf_uv * grid_n;
            float2 deriv = max(fwidth(guv), float2(1e-5, 1e-5));
            float2 dist  = abs(frac(guv - 0.5) - 0.5) / deriv;
            float  line_cov = 1.0 - saturate(min(dist.x, dist.y));
            lit = lerp(lit, lit * 0.35 + float3(0.85, 0.94, 1.0) * 0.32, line_cov * grid_a);
        }

        if (has_lbuf && surf_push.lighting.fog_params.z > 0.5)
        {
            float cam_d = length(surf_push.cam_pos_grid.xyz - i.world_pos);
            float f_fac = 0.0;
            if (surf_push.lighting.fog_params.z < 1.5)
                f_fac = saturate((cam_d - surf_push.lighting.fog_params.x) / max(surf_push.lighting.fog_params.y - surf_push.lighting.fog_params.x, 1e-4));
            else
            {
                float d_f = max(cam_d - surf_push.lighting.fog_params.x, 0.0) * surf_push.lighting.fog_color.w;
                f_fac = 1.0 - exp2(-d_f * d_f);
            }
            lit = lerp(lit, surf_push.lighting.fog_color.rgb, f_fac);
        }
    }

    return float4(lit * glow_mult * alpha, alpha);
}

/* -------------------------------------------------------- band fill/stroke */

[shader("vertex")]
BandVaryings vs_band(uint vertex_id : SV_VertexID)
{
    static const float2 CORNERS[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
        float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0)
    };
    uint         band_idx = vertex_id / 6u;
    uint         corner   = vertex_id % 6u;
    AnimBand     b        = push.bands[band_idx];
    float2       px       = lerp(b.bounds.xy, b.bounds.zw, CORNERS[corner]);
    BandVaryings o;
    o.clip     = float4(px_to_ndc(px, push.viewport.zw), 0.5, 1.0);
    o.band_idx = band_idx;
    return o;
}

struct BandDepthOut
{
    float4 color : SV_Target0;
    float  depth : SV_Depth;
};

float4 eval_band_impl(BandVaryings i, out float out_depth, out bool ok)
{
    AnimBand  b         = push.bands[i.band_idx];
    AnimShape shape     = push.shapes[b.meta.z];
    float2    pos       = i.clip.xy;
    uint      seg_off   = b.meta.x;
    uint      seg_count = b.meta.y;
    bool      is_stroke = (b.meta.w & 1u) != 0u;
    float     glow_mult = 1.0 + shape.style.x * 2.5;
    out_depth           = shape.extra.y;
    ok                  = true;

    /* GPU clip box */
    if (pos.x < shape.clip_box.x || pos.x > shape.clip_box.z ||
        pos.y < shape.clip_box.y || pos.y > shape.clip_box.w)
    {
        ok = false;
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    if (!is_stroke)
    {
        int   winding  = 0;
        float min_dist = 1e9;

        for (uint k = 0u; k < seg_count; ++k)
        {
            AnimQuadSeg s  = push.segs[seg_off + k];
            float2      p0 = s.p0_p1.xy;
            float2      p1 = s.p0_p1.zw;
            float2      p2 = s.p2_w.xy;

            /* Y-monotonic half-open horizontal ray winding test */
            float y_lo = min(p0.y, p2.y);
            float y_hi = max(p0.y, p2.y);
            if (pos.y >= y_lo && pos.y < y_hi)
            {
                float ay = p0.y - 2.0 * p1.y + p2.y;
                float by = p1.y - p0.y;
                float cy = p0.y - pos.y;
                float t;
                if (abs(ay) < 1e-4 * max(y_hi - y_lo, 1.0))
                {
                    float dy = p2.y - p0.y;
                    t        = abs(dy) > 1e-8 ? saturate((pos.y - p0.y) / dy) : 0.5;
                }
                else
                {
                    float disc = max(by * by - ay * cy, 0.0);
                    float sd   = sqrt(disc);
                    float t1   = (-by + sd) / ay;
                    float t2   = (-by - sd) / ay;
                    t          = abs(t1 - 0.5) < abs(t2 - 0.5) ? saturate(t1) : saturate(t2);
                }
                float x_cross = (1.0 - t) * (1.0 - t) * p0.x + 2.0 * t * (1.0 - t) * p1.x + t * t * p2.x;
                if (x_cross > pos.x)
                    winding += (p2.y > p0.y) ? 1 : -1;
            }

            float pad_r = max(1.75, shape.style.w + 1.5);
            float2 s_min = min(p0, min(p1, p2)) - pad_r;
            float2 s_max = max(p0, max(p1, p2)) + pad_r;
            if (pos.x >= s_min.x && pos.x <= s_max.x && pos.y >= s_min.y && pos.y <= s_max.y)
            {
                float d = sd_quad_bezier(pos, p0, p1, p2).x;
                min_dist = min(min_dist, d);
            }
        }

        bool  even_odd = (b.meta.w & 2u) != 0u;
        bool  inside   = even_odd ? ((abs(winding) & 1) != 0) : (winding != 0);
        float cov      = inside ? saturate(0.5 + min_dist) : saturate(0.5 - min_dist);

        if (cov > 0.0005)
        {
            float4 col = eval_fill_gradient(pos, shape);
            col.rgb    = apply_sheen(col.rgb, pos, shape) * glow_mult;
            float a    = col.a * cov;
            return float4(col.rgb * a, a);
        }

        /* Optional soft outer glow / shadow halo on fill silhouettes */
        if (shape.style.w > 0.25 && !inside && min_dist < shape.style.w * 2.5)
        {
            float sig = max(shape.style.w, 1.0);
            float sh  = 0.45 * exp(-(min_dist * min_dist) / (2.0 * sig * sig)) * shape.fill_color.a;
            if (sh > 0.001)
                return float4(0.0, 0.0, 0.0, sh);
        }
        ok = false;
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        float  best_cov = 0.0;
        float4 best_col = shape.stroke_color;
        float  best_z   = shape.extra.y;
        float  dash_len = shape.dash_params.x;
        float  gap_len  = shape.dash_params.y;
        float  dash_off = shape.dash_params.z;
        bool   has_dash = (dash_len > 0.05 && gap_len > 0.05);

        for (uint k = 0u; k < seg_count; ++k)
        {
            AnimQuadSeg s      = push.segs[seg_off + k];
            float2      p0     = s.p0_p1.xy;
            float2      p1     = s.p0_p1.zw;
            float2      p2     = s.p2_w.xy;
            float       w_max  = max(s.p2_w.z, s.p2_w.w) + 1.75;
            float2      s_min  = min(p0, min(p1, p2)) - w_max;
            float2      s_max  = max(p0, max(p1, p2)) + w_max;

            if (pos.x < s_min.x || pos.x > s_max.x || pos.y < s_min.y || pos.y > s_max.y)
                continue;

            float3 res    = sd_quad_bezier(pos, p0, p1, p2);
            float  dist   = res.x;
            float  t      = res.y;
            float  raw_t  = res.z;
            float  half_w = lerp(s.p2_w.z, s.p2_w.w, t);
            uint   flags  = s.meta.x;
            uint   cap    = (flags >> 2u) & 3u;

            if (cap != 0u && (((flags & 1u) != 0u && raw_t < 0.0) || ((flags & 2u) != 0u && raw_t > 1.0)))
            {
                bool   at_start = raw_t < 0.0;
                float2 end_pt   = at_start ? p0 : p2;
                float2 tang_raw = at_start ? (length(p1 - p0) > 1e-4 ? (p1 - p0) : (p2 - p0))
                                           : (length(p2 - p1) > 1e-4 ? (p2 - p1) : (p2 - p0));
                float  tlen     = length(tang_raw);
                if (tlen > 1e-5)
                {
                    float2 dir   = tang_raw / tlen;
                    float  along = dot(pos - end_pt, at_start ? dir : -dir);
                    float  perp  = abs(dot(pos - end_pt, float2(-dir.y, dir.x)));
                    float  ext   = (cap == 2u) ? half_w : 0.0;
                    float  box_d = max(perp - half_w, -along - ext);
                    dist = box_d + half_w;
                }
            }

            float cov = saturate(half_w + 0.5 - dist);
            if (has_dash && cov > 0.0005)
            {
                float period = dash_len + gap_len;
                float s_arc  = lerp(s.arc_len.x, s.arc_len.y, t) + dash_off;
                float ph     = s_arc - floor(s_arc / period) * period;
                float d_in   = min(ph, dash_len - ph);
                float d_out  = period - ph;
                float dash_c = (ph <= dash_len) ? saturate(d_in + 0.5) : saturate(0.5 - min(ph - dash_len, d_out));
                cov *= dash_c;
            }

            if (cov > best_cov)
            {
                best_cov = cov;
                best_col = (s.c0.a >= 0.0) ? lerp(s.c0, s.c1, t) : shape.stroke_color;
                best_z   = lerp(s.arc_len.z, s.arc_len.w, t);
            }
        }

        if (best_cov <= 0.0005)
        {
            ok = false;
            return float4(0.0, 0.0, 0.0, 0.0);
        }

        best_col.rgb = apply_sheen(best_col.rgb, pos, shape) * glow_mult;
        float a      = best_col.a * best_cov;
        out_depth    = saturate(best_z - 0.0004);
        return float4(best_col.rgb * a, a);
    }
}

[shader("fragment")]
float4 fs_band(BandVaryings i) : SV_Target0
{
    float z;
    bool  ok;
    float4 col = eval_band_impl(i, z, ok);
    if (!ok)
        discard;
    return col;
}

[shader("fragment")]
BandDepthOut fs_band_depth(BandVaryings i)
{
    float z;
    bool  ok;
    float4 col = eval_band_impl(i, z, ok);
    if (!ok || col.a <= 0.01)
        discard;
    BandDepthOut o;
    o.color = col;
    o.depth = z;
    return o;
}

/* ----------------------------------------------------- temporal accum blit */

[shader("fragment")]
float4 fs_accum_blit(ScreenVaryings i) : SV_Target0
{
    SamplerState lin = g_samplers[NonUniformResourceIndex(post_push.linear_sampler)];
    float4       c   = g_textures[NonUniformResourceIndex(post_push.color_map)].SampleLevel(lin, i.uv, 0.0);
    return c * post_push.bloom.w;
}

/* ----------------------------------------------------------------- bloom */

float3 bloom_tap(float2 uv, float2 offset)
{
    return g_textures[NonUniformResourceIndex(post_push.color_map)]
        .SampleLevel(g_samplers[NonUniformResourceIndex(post_push.linear_sampler)],
                     uv + offset * post_push.texel.xy, 0.0).rgb;
}

float3 bloom_downsample13(float2 uv, bool prefilter)
{
    float3 t[13];
    t[0]  = bloom_tap(uv, float2(-2, -2)); t[1]  = bloom_tap(uv, float2(0, -2)); t[2]  = bloom_tap(uv, float2(2, -2));
    t[3]  = bloom_tap(uv, float2(-2,  0)); t[4]  = bloom_tap(uv, float2(0,  0)); t[5]  = bloom_tap(uv, float2(2,  0));
    t[6]  = bloom_tap(uv, float2(-2,  2)); t[7]  = bloom_tap(uv, float2(0,  2)); t[8]  = bloom_tap(uv, float2(2,  2));
    t[9]  = bloom_tap(uv, float2(-1, -1)); t[10] = bloom_tap(uv, float2(1, -1));
    t[11] = bloom_tap(uv, float2(-1,  1)); t[12] = bloom_tap(uv, float2(1,  1));

    [ForceUnroll]
    for (int k = 0; k < 13; ++k)
        t[k] = select(prefilter, mu_karis_weight(mu_bloom_threshold(t[k], post_push.bloom.x, post_push.bloom.y)), t[k]);

    return mu_bloom_downsample13(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], t[12]);
}

[shader("fragment")]
float4 fs_bloom_prefilter(ScreenVaryings i) : SV_Target0
{
    return float4(bloom_downsample13(i.uv, true), 1.0);
}

[shader("fragment")]
float4 fs_bloom_down(ScreenVaryings i) : SV_Target0
{
    return float4(bloom_downsample13(i.uv, false), 1.0);
}

[shader("fragment")]
float4 fs_bloom_up(ScreenVaryings i) : SV_Target0
{
    return float4(mu_bloom_upsample9(
        bloom_tap(i.uv, float2(-1, -1)), bloom_tap(i.uv, float2(0, -1)), bloom_tap(i.uv, float2(1, -1)),
        bloom_tap(i.uv, float2(-1,  0)), bloom_tap(i.uv, float2(0,  0)), bloom_tap(i.uv, float2(1,  0)),
        bloom_tap(i.uv, float2(-1,  1)), bloom_tap(i.uv, float2(0,  1)), bloom_tap(i.uv, float2(1,  1))), 1.0);
}

/* ------------------------------------------------------------- composite */

float3 tonemap_aces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

[shader("fragment")]
float4 fs_composite(ScreenVaryings i) : SV_Target0
{
    SamplerState lin = g_samplers[NonUniformResourceIndex(post_push.linear_sampler)];
    float4       hdr = g_textures[NonUniformResourceIndex(post_push.color_map)].SampleLevel(lin, i.uv, 0.0);
    float3       blm = g_textures[NonUniformResourceIndex(post_push.bloom_map)].SampleLevel(lin, i.uv, 0.0).rgb;

    if (post_push.dof.x > 0.01 && post_push.depth_map != 0u)
    {
        float z_ndc = g_textures[NonUniformResourceIndex(post_push.depth_map)].SampleLevel(lin, i.uv, 0.0).r;
        float z_lin = lerp((0.1 * 200.0) / max(200.0 - z_ndc * 199.9, 1e-4), 0.1 + z_ndc * 199.9, post_push.dof.w);
        float coc_r = saturate((abs(z_lin - post_push.dof.y) - post_push.dof.z) / max(post_push.dof.z, 0.5)) * post_push.dof.x;
        if (coc_r > 0.25)
        {
            float4 acc  = hdr;
            float  wsum = 1.0;
            [ForceUnroll]
            for (int k = 0; k < 12; ++k)
            {
                float  ang  = float(k) * 2.3999632;
                float  rad  = sqrt((float(k) + 0.5) / 12.0) * coc_r;
                float2 tuv  = i.uv + float2(cos(ang), sin(ang)) * rad * post_push.texel.xy;
                float  tz   = g_textures[NonUniformResourceIndex(post_push.depth_map)].SampleLevel(lin, tuv, 0.0).r;
                float  tlin = lerp((0.1 * 200.0) / max(200.0 - tz * 199.9, 1e-4), 0.1 + tz * 199.9, post_push.dof.w);
                float  tcoc = saturate((abs(tlin - post_push.dof.y) - post_push.dof.z) / max(post_push.dof.z, 0.5));
                float  w    = (tlin >= z_lin - 0.5) ? 1.0 : tcoc;
                acc  += g_textures[NonUniformResourceIndex(post_push.color_map)].SampleLevel(lin, tuv, 0.0) * w;
                wsum += w;
            }
            hdr = acc / wsum;
        }
    }

    if (post_push.fx.x > 0.01)
    {
        float2 dir = (i.uv - float2(0.5, 0.5)) * (post_push.fx.x * post_push.texel.zw * 2.0);
        hdr.r = g_textures[NonUniformResourceIndex(post_push.color_map)].SampleLevel(lin, i.uv + dir, 0.0).r;
        hdr.b = g_textures[NonUniformResourceIndex(post_push.color_map)].SampleLevel(lin, i.uv - dir, 0.0).b;
    }

    float  out_a = (post_push.bloom.z > 0.5) ? saturate(hdr.a) : 1.0;
    float3 base  = (post_push.bloom.z > 0.5 && out_a > 1e-4) ? (hdr.rgb / out_a) : hdr.rgb;
    float3 rgb   = base * post_push.grade.x + blm * post_push.grade.y;

    int tm_mode = int(post_push.fx.z + 0.5);
    if (tm_mode == 1)
    {
        rgb = tonemap_aces(rgb);
    }
    else if (tm_mode == 2)
    {
        rgb = mu_tonemap_agx(rgb);
    }
    else
    {
        float peak = max(rgb.r, max(rgb.g, rgb.b));
        if (peak > 1.0)
            rgb *= (1.0 + (peak - 1.0) / (1.0 + (peak - 1.0))) / peak;
    }

    rgb = mu_saturation(rgb, post_push.grade.z);
    if (post_push.grade.w > 0.0)
        rgb = mu_vignette(rgb, i.uv, post_push.grade.w, 0.65);

    rgb = mu_linear_to_srgb(saturate(rgb));
    rgb = mu_dither_ign(rgb, i.clip.xy, (0.5 / 255.0) + post_push.fx.y);

    /* Interactive scrub HUD bar at bottom when enabled */
    if (post_push.hud.z > 0.5)
    {
        float2 px  = i.clip.xy;
        float2 res = 1.0 / post_push.texel.zw;
        if (px.y > res.y - 24.0 && px.y < res.y - 6.0 && px.x > 24.0 && px.x < res.x - 24.0)
        {
            float bar_w = res.x - 48.0;
            float u     = saturate((px.x - 24.0) / bar_w);
            float mid_y = res.y - 15.0;
            float dy    = abs(px.y - mid_y);
            if (dy < 3.0)
            {
                float3 bar = (u <= post_push.hud.x) ? float3(0.35, 0.77, 0.90) : float3(0.22, 0.25, 0.30);
                rgb        = lerp(rgb, bar, 0.88);
            }
            float knob_x = 24.0 + post_push.hud.x * bar_w;
            float kd     = length(px - float2(knob_x, mid_y));
            if (kd < 6.0)
                rgb = lerp(rgb, (post_push.hud.y > 0.5) ? float3(1.0, 0.9, 0.35) : float3(1.0, 0.45, 0.45), saturate(6.0 - kd));
        }
    }

    return float4(rgb, out_a);
}
