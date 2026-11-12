/* Direct3D 11 presenter: scales the 768x240 CD-i frame to any window or
   monitor resolution (up to 4K and beyond) with selectable filters and
   aspect-ratio modes, including blurred side panels for 16:9 displays. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "render.h"
#include "runtime.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static ID3D11RenderTargetView *rtv;
static ID3D11Texture2D *tex;
static ID3D11ShaderResourceView *srv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11Buffer *cbuf;
static ID3D11SamplerState *samp_lin, *samp_point;
static int tex_w, tex_h, tex_mips;
static UINT buf_w, buf_h;
static HWND rwnd;
#ifdef RECOMP_HD
static void hd_init(void);
#endif

typedef struct {
    float rect[4];   /* destination rect in NDC: left, top, right, bottom */
    float src[4];    /* texture w, h, destination w, h (pixels) */
    float prm[4];    /* filter, mip level for backdrop, brightness, mode (0 image, 1 backdrop) */
    float uvx[4];    /* uv offset xy, uv scale zw */
    float ext[4];    /* x: FMV upscaler on */
} CB;

static const char *shader_src =
"Texture2D tex : register(t0);\n"
"SamplerState sl : register(s0);\n"
"SamplerState sp : register(s1);\n"
"cbuffer C : register(b0) { float4 rect; float4 src; float4 prm; float4 uvx; float4 ext; };\n"
"struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"VO vsmain(uint id : SV_VertexID) {\n"
"  float2 c = float2(id & 1, id >> 1);\n"
"  VO o;\n"
"  o.pos = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0, 1);\n"
"  o.uv = uvx.xy + c * uvx.zw;\n"
"  return o;\n"
"}\n"
/* sharp bilinear: crisp pixels without shimmer at non-integer scales */
"float3 sharp(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 scale = src.zw / ts;\n"
"  float2 texel = uv * ts;\n"
"  float2 pre = max(floor(scale), 1.0);\n"
"  float2 fl = floor(texel);\n"
"  float2 cd = frac(texel) - 0.5;\n"
"  float2 rr = 0.5 - 0.5 / pre;\n"
"  float2 f = (cd - clamp(cd, -rr, rr)) * pre + 0.5;\n"
"  return tex.SampleLevel(sl, (fl + f) / ts, 0).rgb;\n"
"}\n"
/* Catmull-Rom bicubic with 9 bilinear taps */
"float3 bicubic(float2 uv) {\n"
"  float2 ts = src.xy;\n"
"  float2 sp2 = uv * ts;\n"
"  float2 t1 = floor(sp2 - 0.5) + 0.5;\n"
"  float2 f = sp2 - t1;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2;\n"
"  float2 o12 = w2 / w12;\n"
"  float2 t0 = (t1 - 1) / ts, t3 = (t1 + 2) / ts, t12 = (t1 + o12) / ts;\n"
"  float3 r = 0;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;\n"
"  r += tex.SampleLevel(sl, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;\n"
"  r += tex.SampleLevel(sl, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;\n"
"  return saturate(r);\n"
"}\n"
/* simple CRT: gaussian scanlines + aperture grille */
"float3 crt(float2 uv, float2 pos) {\n"
"  float ty = uv.y * src.y;\n"
"  float yc = floor(ty) + 0.5;\n"
"  float3 a = sharp(float2(uv.x, yc / src.y));\n"
"  float d = ty - yc;\n"
"  float lum = dot(a, float3(0.3, 0.6, 0.1));\n"
"  float width = lerp(6.0, 2.5, lum);\n"
"  float beam = exp(-d * d * width * 4.0);\n"
/* scanlines need several output pixels per source line to look right */
"  float strength = saturate((src.w / src.y - 2.5) / 4.0);\n"
"  float3 c = a * lerp(1.0, beam * 1.35, strength);\n"
"  int m = (int)pos.x % 3;\n"
"  float3 mask = m == 0 ? float3(1.0, 0.85, 0.85) : (m == 1 ? float3(0.85, 1.0, 0.85) : float3(0.85, 0.85, 1.0));\n"
"  mask = lerp(1.0, mask, strength);\n"
"  return saturate(c * mask * lerp(1.0, 1.1, strength));\n"
"}\n"
/* FMV / photo upscaler.  Video (RL7) and DYUV pictures are stored at 384
   pixels per line, each written twice into the 768-wide frame; the frame's
   alpha byte marks those pixels.  They are resampled on their true 384x240
   grid with an edge-directed Lanczos-2 kernel: a structure tensor gives the
   local edge orientation, and the kernel is stretched along edges (smooth,
   unjagged contours) while staying sharp across them.  The result is clamped
   to the nearest 2x2 texels to avoid ringing.  Graphics keep the normal filter. */
"float4 natf(int2 p) {\n"
"  p = clamp(p, int2(0, 0), int2((int)src.x / 2 - 1, (int)src.y - 1));\n"
"  return tex.Load(int3(p.x * 2, p.y, 0));\n"
"}\n"
"float natm(int2 p) {\n"
"  p = clamp(p, int2(0, 0), int2((int)src.x / 2 - 1, (int)src.y - 1));\n"
"  return min(tex.Load(int3(p.x * 2, p.y, 0)).a, tex.Load(int3(p.x * 2 + 1, p.y, 0)).a);\n"
"}\n"
"float lz2(float x2) {\n"
"  if (x2 >= 4.0) return 0.0;\n"
"  float a = 0.4 * x2 - 1.0, b = 0.25 * x2 - 1.0;\n"
"  return (1.5625 * a * a - 0.5625) * b * b;\n"
"}\n"
"float vmask(float2 uv) {\n"
"  float2 pp = uv * float2(src.x * 0.5, src.y) - 0.5;\n"
"  float2 fp = floor(pp), f = pp - fp; int2 b = int2(fp);\n"
"  return lerp(lerp(natm(b), natm(b + int2(1, 0)), f.x), lerp(natm(b + int2(0, 1)), natm(b + int2(1, 1)), f.x), f.y);\n"
"}\n"
"float3 upscale(float2 uv) {\n"
"  float2 pp = uv * float2(src.x * 0.5, src.y) - 0.5;\n"
"  float2 fp = floor(pp), f = pp - fp; int2 b = int2(fp);\n"
"  float3 c[16]; float l[16];\n"
"  [unroll] for (int j = 0; j < 4; j++) [unroll] for (int i = 0; i < 4; i++) {\n"
"    c[j * 4 + i] = natf(b + int2(i - 1, j - 1)).rgb;\n"
"    l[j * 4 + i] = dot(c[j * 4 + i], float3(0.299, 0.587, 0.114));\n"
"  }\n"
/* structure tensor from the 9 2x2 cells of the 4x4 window, gaussian-weighted
   around the sample point; only strongly coherent (real) edges are followed,
   so film grain and dithering stay isotropic instead of turning into swirls */
"  float3 J = 0;\n"
"  [unroll] for (int cy = 0; cy < 3; cy++) [unroll] for (int cx = 0; cx < 3; cx++) {\n"
"    int k = cy * 4 + cx;\n"
"    float gx = 0.5 * (l[k + 1] + l[k + 5] - l[k] - l[k + 4]);\n"
"    float gy = 0.5 * (l[k + 4] + l[k + 5] - l[k] - l[k + 1]);\n"
"    float2 dc = float2(cx - 0.5, cy - 0.5) - f;\n"
"    float w = exp(-dot(dc, dc) * 1.5);\n"
"    J += w * float3(gx * gx, gx * gy, gy * gy);\n"
"  }\n"
"  float tr = J.x + J.z;\n"
"  float dd = sqrt((J.x - J.z) * (J.x - J.z) + 4 * J.y * J.y);\n"
"  float coh = dd / (tr + 1e-5);\n"
"  float ang = (abs(J.y) + abs(J.x - J.z) > 1e-10) ? 0.5 * atan2(2 * J.y, J.x - J.z) : 0.0;\n"
"  float2 n = float2(cos(ang), sin(ang)), t = float2(-n.y, n.x);\n"
"  float aniso = smoothstep(0.65, 0.95, coh) * saturate(sqrt(tr) * 4.0 - 0.1);\n"
"  float st = 1.0 / (1.0 + aniso);\n"
"  float3 acc = 0; float ws = 0;\n"
"  [unroll] for (int k = 0; k < 16; k++) {\n"
"    float2 d = float2(k % 4 - 1, k / 4 - 1) - f;\n"
"    float dt = dot(d, t) * st, dn = dot(d, n);\n"
"    float w = lz2(dt * dt + dn * dn);\n"
"    acc += c[k] * w; ws += w;\n"
"  }\n"
"  float3 r = acc / ws;\n"
"  float3 mn = min(min(c[5], c[6]), min(c[9], c[10])), mx = max(max(c[5], c[6]), max(c[9], c[10]));\n"
"  return clamp(r, mn, mx);\n"
"}\n"
/* ---- voyeur_hd: planes composed at output resolution ------------------
   Each plane arrives separately (RGB + opacity).  Where the pack matched a
   plane, its AI-upscaled picture is drawn (row = display line + dy); during
   video playback the picture is warped toward the next frame with the
   precomputed optical flow (phase t), bringing 12-15 fps video to the
   60 Hz field rate.  Unmatched photographic planes use the edge-directed
   upscaler with premultiplied alpha (no colour-key fringes); graphics stay
   crisp.  Per-line parameters: icf, plane order, backdrop, mapped lines. */
"#ifdef HD\n"
"Texture2D plA : register(t1); Texture2D plB : register(t2); Texture2D ovl : register(t3); Texture2D lin : register(t4);\n"
"Texture2D hcA : register(t5); Texture2D hnA : register(t6); Texture2D mcA : register(t7); Texture2D mnA : register(t8); Texture2D flA : register(t9);\n"
"Texture2D hcB : register(t10); Texture2D hnB : register(t11); Texture2D mcB : register(t12); Texture2D mnB : register(t13); Texture2D flB : register(t14);\n"
"cbuffer HDC : register(b1) { float4 hdA; float4 hdB; float4 hdG; };\n"
"float4 plf(Texture2D t, int2 p) { p = clamp(p, int2(0, 0), int2(383, 239)); return t.Load(int3(p.x * 2, p.y, 0)); }\n"
"float4 upscale_pm(Texture2D tx, float2 uv) {\n"
"  float2 pp = uv * float2(384.0, 240.0) - 0.5;\n"
"  float2 fp = floor(pp), f = pp - fp; int2 b = int2(fp);\n"
"  float4 c[16]; float l[16];\n"
"  [unroll] for (int j = 0; j < 4; j++) [unroll] for (int i = 0; i < 4; i++) {\n"
"    float4 v = plf(tx, b + int2(i - 1, j - 1));\n"
"    c[j * 4 + i] = v;\n"
"    l[j * 4 + i] = dot(v.rgb * v.a, float3(0.299, 0.587, 0.114)) + v.a * 0.25;\n"
"  }\n"
"  float3 J = 0;\n"
"  [unroll] for (int cy = 0; cy < 3; cy++) [unroll] for (int cx = 0; cx < 3; cx++) {\n"
"    int k = cy * 4 + cx;\n"
"    float gx = 0.5 * (l[k + 1] + l[k + 5] - l[k] - l[k + 4]);\n"
"    float gy = 0.5 * (l[k + 4] + l[k + 5] - l[k] - l[k + 1]);\n"
"    float2 dc = float2(cx - 0.5, cy - 0.5) - f;\n"
"    J += exp(-dot(dc, dc) * 1.5) * float3(gx * gx, gx * gy, gy * gy);\n"
"  }\n"
"  float tr = J.x + J.z;\n"
"  float coh = sqrt((J.x - J.z) * (J.x - J.z) + 4 * J.y * J.y) / (tr + 1e-5);\n"
"  float ang = (abs(J.y) + abs(J.x - J.z) > 1e-10) ? 0.5 * atan2(2 * J.y, J.x - J.z) : 0.0;\n"
"  float2 n = float2(cos(ang), sin(ang)), t = float2(-n.y, n.x);\n"
"  float st = 1.0 / (1.0 + smoothstep(0.65, 0.95, coh) * saturate(sqrt(tr) * 4.0 - 0.1));\n"
"  float3 acc = 0; float aa = 0, ws = 0;\n"
"  [unroll] for (int k = 0; k < 16; k++) {\n"
"    float2 d = float2(k % 4 - 1, k / 4 - 1) - f;\n"
"    float dt = dot(d, t) * st, dn = dot(d, n);\n"
"    float w = lz2(dt * dt + dn * dn);\n"
"    acc += c[k].rgb * c[k].a * w; aa += c[k].a * w; ws += w;\n"
"  }\n"
"  float3 r = aa > 1e-4 ? acc / aa : 0;\n"
"  float3 mn = 1, mx = 0;\n"
"  int inner[4] = {5, 6, 9, 10};\n"
"  [unroll] for (int q = 0; q < 4; q++) if (c[inner[q]].a > 0.5) { mn = min(mn, c[inner[q]].rgb); mx = max(mx, c[inner[q]].rgb); }\n"
"  if (mx.x >= mn.x) r = clamp(r, mn, mx);\n"
"  return float4(r, saturate(aa / ws));\n"
"}\n"
"float4 sharp4(Texture2D tx, float2 uv) {\n"
"  float2 ts = float2(768.0, 240.0);\n"
"  float2 pre = max(floor(src.zw / ts), 1.0);\n"
"  float2 texel = uv * ts, fl = floor(texel), cd = frac(texel) - 0.5, rr = 0.5 - 0.5 / pre;\n"
"  return tx.SampleLevel(sl, (fl + (cd - clamp(cd, -rr, rr)) * pre + 0.5) / ts, 0);\n"
"}\n"
"float4 plane_hd(Texture2D pl, Texture2D hc, Texture2D hn, Texture2D mc, Texture2D mn, Texture2D fw,\n"
"                float4 h, float nat, bool okl, float2 uv) {\n"
"  if (okl && h.x > 0.5) {\n"
"    int fg = (int)(h.w + 0.5);\n"
"    float2 iuv = float2(uv.x, (uv.y * 240.0 + h.y) / 240.0);\n"
"    float tt = h.z;\n"
"    if (tt > 0.001 && (fg & 2)) {\n"
"      float2 fv = (fw.SampleLevel(sl, iuv, 0).rg * 255.0 - 128.0) / 4.0 / float2(384.0, 240.0);\n"
"      float2 u0 = iuv - tt * fv, u1 = iuv + (1.0 - tt) * fv;\n"
"      float3 c0 = hc.SampleLevel(sl, u0, 0).rgb, c1 = hn.SampleLevel(sl, u1, 0).rgb;\n"
"      float m0 = (fg & 1) ? mc.SampleLevel(sl, u0, 0).r : 1.0;\n"
"      float m1 = (fg & 4) ? mn.SampleLevel(sl, u1, 0).r : 1.0;\n"
"      return float4(lerp(c0, c1, tt), lerp(m0, m1, tt));\n"
"    }\n"
"    float a = (fg & 1) ? mc.SampleLevel(sl, iuv, 0).r : pl.SampleLevel(sl, uv, 0).a;\n"
"    return float4(hc.SampleLevel(sl, iuv, 0).rgb, a);\n"
"  }\n"
"  if (nat > 0.5) return upscale_pm(pl, uv);\n"
"  return sharp4(pl, uv);\n"
"}\n"
"bool hd_main(float2 uv, out float4 res) {\n"
"  res = 0;\n"
"  int ln = clamp((int)(uv.y * 240.0), 0, 239);\n"
"  float4 l0 = lin.Load(int3(0, ln, 0)), l1 = lin.Load(int3(1, ln, 0)), l2 = lin.Load(int3(2, ln, 0));\n"
"  if (l0.z < 0.5) return false;\n"
"  float4 A = plane_hd(plA, hcA, hnA, mcA, mnA, flA, hdA, l1.w, l2.y > 0.5, uv);\n"
"  float4 B = plane_hd(plB, hcB, hnB, mcB, mnB, flB, hdB, l2.x, l2.z > 0.5, uv);\n"
"  A.a = smoothstep(0.15, 0.85, A.a); B.a = smoothstep(0.15, 0.85, B.a);\n"
"  A.rgb *= l0.x; B.rgb *= l0.y;\n"
"  float3 o = l0.w > 0.5 ? lerp(lerp(l1.rgb, A.rgb, A.a), B.rgb, B.a) : lerp(lerp(l1.rgb, B.rgb, B.a), A.rgb, A.a);\n"
"  float4 ov = ovl.SampleLevel(sp, uv, 0);\n"
"  res = float4(lerp(o, ov.rgb, ov.a), 1);\n"
"  return true;\n"
"}\n"
"#endif\n"
"float4 psmain(VO i) : SV_Target {\n"
"#ifdef HD\n"
"  if (hdG.x > 0.5 && prm.w < 0.5 && (int)prm.x < 2) { float4 r; if (hd_main(i.uv, r)) return r; }\n"
"#endif\n"
"  if (prm.w > 0.5) {\n"
"    float2 st = exp2(prm.y) / src.xy;\n"
"    float3 b = 0; float wsum = 0;\n"
"    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
"      float w = exp(-(x * x + y * y) / 4.0);\n"
"      b += tex.SampleLevel(sl, i.uv + float2(x, y) * st, prm.y).rgb * w; wsum += w;\n"
"    }\n"
"    return float4(b / wsum * prm.z, 1);\n"
"  }\n"
"  int f = (int)prm.x;\n"
"  float3 c;\n"
"  if (f == 0) c = sharp(i.uv);\n"
"  else if (f == 1) c = bicubic(i.uv);\n"
"  else if (f == 2) c = crt(i.uv, i.pos.xy);\n"
"  else c = tex.SampleLevel(sp, i.uv, 0).rgb;\n"
"  if (ext.x > 0.5 && f < 2) {\n"
"    float m = vmask(i.uv);\n"
"    if (m > 0.0) c = lerp(c, upscale(i.uv), m);\n"
"  }\n"
"  return float4(c, 1);\n"
"}\n";

static const char *aspect_names[] = {"4:3 + blurred sides", "4:3", "stretch to fill", "zoom to fill width"};
static const char *filter_names[] = {"sharp", "smooth", "CRT", "nearest"};
RenderSettings render_settings = {0, 0, 1, 1};

const char *render_aspect_name(int a) { return aspect_names[a % RENDER_ASPECTS]; }
const char *render_filter_name(int f) { return filter_names[f % RENDER_FILTERS]; }

static void release_rtv(void) {
    if (rtv) { ID3D11RenderTargetView_Release(rtv); rtv = NULL; }
}

static int make_rtv(void) {
    ID3D11Texture2D *bb = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return -1;
    HRESULT hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);
    return FAILED(hr) ? -1 : 0;
}

int render_init(HWND hwnd) {
    rwnd = hwnd;
    RECT r;
    GetClientRect(hwnd, &r);
    buf_w = r.right > 0 ? (UINT)r.right : 1;
    buf_h = r.bottom > 0 ? (UINT)r.bottom : 1;
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = buf_w;
    sd.BufferDesc.Height = buf_h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, NULL, &ctx);
    if (FAILED(hr)) {
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, NULL, &ctx);
    }
    if (FAILED(hr)) { LOG("Direct3D 11 unavailable (%08lx); using GDI", (unsigned long)hr); return -1; }
    /* we handle Alt+Enter ourselves (borderless fullscreen) */
    {
        IDXGIDevice *xd = NULL; IDXGIAdapter *ad = NULL; IDXGIFactory *fa = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd)) &&
            SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)) &&
            SUCCEEDED(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fa)))
            IDXGIFactory_MakeWindowAssociation(fa, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (fa) IDXGIFactory_Release(fa);
        if (ad) IDXGIAdapter_Release(ad);
        if (xd) IDXGIDevice_Release(xd);
    }
    if (make_rtv()) return -1;

    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    size_t sl = strlen(shader_src);
#ifdef RECOMP_HD
    D3D_SHADER_MACRO defs[] = {{"HD", "1"}, {NULL, NULL}};
#else
    D3D_SHADER_MACRO *defs = NULL;
#endif
    if (FAILED(D3DCompile(shader_src, sl, "present", defs, NULL, "vsmain", "vs_4_0", 0, 0, &vb, &err)) ||
        FAILED(D3DCompile(shader_src, sl, "present", defs, NULL, "psmain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err))) {
        LOG("shader compile failed: %s", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return -1;
    }
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cbuf);

    D3D11_SAMPLER_DESC s = {0};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_lin);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(dev, &s, &samp_point);
#ifdef RECOMP_HD
    hd_init();
#endif
    LOG("renderer: Direct3D 11, %ux%u", buf_w, buf_h);
    return 0;
}

static int ensure_texture(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return 0;
    if (srv) { ID3D11ShaderResourceView_Release(srv); srv = NULL; }
    if (tex) { ID3D11Texture2D_Release(tex); tex = NULL; }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = 0;                 /* full chain: low mips give the blurred backdrop */
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex))) return -1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv))) return -1;
    ID3D11Texture2D_GetDesc(tex, &td);
    tex_w = w; tex_h = h; tex_mips = (int)td.MipLevels;
    return 0;
}


#ifdef RECOMP_HD
/* ---------------------------------------------------------------- voyeur_hd */
#include "hd/hd.h"
HdField g_hdf;

typedef struct { ID3D11Texture2D *t; ID3D11ShaderResourceView *s; int w, h, img; DXGI_FORMAT f; } HTex;
static HTex t_plane[2], t_ovl, t_lin;
static HTex hc[2], hn[2], mc[2], mn[2], fw[2];
static ID3D11Buffer *cb_hd;
typedef struct { float a[4], b[4], g[4]; } HDCB;
static long long presents;
static struct { int cur, prev, period; long long changed; } pst[2] = {{-1, -1, 4, 0}, {-1, -1, 4, 0}};
static float lin_data[HD_H][12];
int g_hd_matched[2];          /* for the window title / debugging */

static void htex(HTex *t, int w, int h, DXGI_FORMAT f) {
    if (t->t && t->w == w && t->h == h && t->f == f) return;
    if (t->s) ID3D11ShaderResourceView_Release(t->s);
    if (t->t) ID3D11Texture2D_Release(t->t);
    t->t = NULL; t->s = NULL; t->img = -1;
    D3D11_TEXTURE2D_DESC d = {0};
    d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t->t)))
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->t, NULL, &t->s);
    t->w = w; t->h = h; t->f = f;
}

static void hup(HTex *t, const void *data, int pitch) {
    if (t->t) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t->t, 0, NULL, data, (UINT)pitch, 0);
}

static void hd_init(void) {
    char p[600];
    const char *env = getenv("RECOMP_HD_PACK");
    if (env) snprintf(p, sizeof p, "%s", env);
    else {
        GetModuleFileNameA(NULL, p, sizeof p);
        char *s = strrchr(p, '\\');
        if (s) s[1] = 0;
        strncat(p, "hd_pack", sizeof p - strlen(p) - 1);
    }
    if (hd_pack_open(p)) LOG("HD pack not found at %s: video uses the shader upscaler (build it with tools/hd_pack.py)", p);
    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = sizeof(HDCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb_hd);
}

/* match the planes against the pack, schedule loads, upload textures */
static void hd_frame(HDCB *cb) {
    presents++;
    memset(cb, 0, sizeof *cb);
    cb->g[0] = render_settings.upscale ? 1.0f : 0.0f;
    for (int p = 0; p < 2; p++) htex(&t_plane[p], HD_FB_W, HD_H, DXGI_FORMAT_B8G8R8A8_UNORM);
    htex(&t_ovl, HD_FB_W, HD_H, DXGI_FORMAT_B8G8R8A8_UNORM);
    htex(&t_lin, 3, HD_H, DXGI_FORMAT_R32G32B32A32_FLOAT);
    hup(&t_plane[0], g_hdf.plane[0], HD_FB_W * 4);
    hup(&t_plane[1], g_hdf.plane[1], HD_FB_W * 4);
    hup(&t_ovl, g_hdf.overlay, HD_FB_W * 4);
    static HdMatch m[2];
    for (int y = 0; y < HD_H; y++) {
        const HdLine *L = &g_hdf.line[y];
        float *o = lin_data[y];
        o[0] = L->icf[0]; o[1] = L->icf[1]; o[2] = L->simple; o[3] = L->b_front;
        o[4] = L->bd[0]; o[5] = L->bd[1]; o[6] = L->bd[2]; o[7] = L->natural[0];
        o[8] = L->natural[1]; o[9] = 0; o[10] = 0; o[11] = 0;
    }
    for (int p = 0; p < 2; p++) {
        float *h = p ? cb->b : cb->a;
        int img = -1;
        g_hd_matched[p] = 0;
        if (hd_pack_loaded() && g_vfi.coding[p] && render_settings.upscale && hd_match(g_vfi.line_hash[p], &m[p]))
            img = m[p].img;
        if (img != pst[p].cur) {
            if (img >= 0 && pst[p].cur >= 0 && hd_next(pst[p].cur) == img) {
                long long d = presents - pst[p].changed;
                if (d >= 2 && d <= 8) pst[p].period = (int)d;
            }
            pst[p].prev = pst[p].cur;
            pst[p].cur = img;
            pst[p].changed = presents;
        }
        if (img < 0) continue;
        int nxt = hd_next(img);
        hd_request(img, 0);
        for (int k = 1, q = nxt; k <= 8 && q >= 0; k++, q = hd_next(q)) hd_request(q, k);
        HdPic pc;
        if (!hd_get(img, &pc)) continue;
        /* the next frame becomes current: swap instead of re-uploading */
        if (hc[p].img != img && hn[p].img == img) {
            HTex t = hc[p]; hc[p] = hn[p]; hn[p] = t;
            t = mc[p]; mc[p] = mn[p]; mn[p] = t;
        }
        if (hc[p].img != img) {
            htex(&hc[p], pc.w, pc.h, DXGI_FORMAT_B8G8R8A8_UNORM);
            hup(&hc[p], pc.rgb, pc.w * 4);
            hc[p].img = img;
        }
        int flags = 0;
        if (pc.mask) {
            if (mc[p].img != img) {
                htex(&mc[p], HD_W, HD_H, DXGI_FORMAT_R8_UNORM);
                hup(&mc[p], pc.mask, HD_W);
                mc[p].img = img;
            }
            flags |= 1;
        }
        float t = 0;
        int playing = pst[p].prev >= 0 && hd_next(pst[p].prev) == img && hd_is_video(img);
        HdPic pn;
        if (render_settings.interp && playing && nxt >= 0 && pc.flow && hd_get(nxt, &pn)) {
            if (hn[p].img != nxt) {
                htex(&hn[p], pn.w, pn.h, DXGI_FORMAT_B8G8R8A8_UNORM);
                hup(&hn[p], pn.rgb, pn.w * 4);
                hn[p].img = nxt;
            }
            if (pn.mask) {
                if (mn[p].img != nxt) {
                    htex(&mn[p], HD_W, HD_H, DXGI_FORMAT_R8_UNORM);
                    hup(&mn[p], pn.mask, HD_W);
                    mn[p].img = nxt;
                }
                flags |= 4;
            }
            if (fw[p].img != img) {
                htex(&fw[p], HD_W / 2, HD_H / 2, DXGI_FORMAT_R8G8_UNORM);
                hup(&fw[p], pc.flow, HD_W);
                fw[p].img = img;
            }
            int per = pst[p].period;
            long long k = presents - pst[p].changed;
            t = (float)k / per;
            if (t > (float)(per - 1) / per) t = (float)(per - 1) / per;
            flags |= 2;
        }
        h[0] = 1; h[1] = (float)m[p].dy; h[2] = t; h[3] = (float)flags;
        {
            static int hl = -1;
            if (hl < 0) hl = getenv("RECOMP_HDLOG") != NULL;
            if (hl) LOG("hd present %lld plane %c: picture %d dy %d lines %d t %.2f flags %d period %d", presents,
                        'A' + p, img, m[p].dy, m[p].nok, t, flags, pst[p].period);
        }
        for (int y = 0; y < HD_H; y++) lin_data[y][p ? 10 : 9] = m[p].ok[y];
        g_hd_matched[p] = 1;
    }
    hup(&t_lin, lin_data, 12 * 4);
}

static void hd_bind(const HDCB *cb) {
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_hd, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &cb_hd);
    ID3D11ShaderResourceView *v[14] = {
        t_plane[0].s, t_plane[1].s, t_ovl.s, t_lin.s,
        hc[0].s, hn[0].s, mc[0].s, mn[0].s, fw[0].s,
        hc[1].s, hn[1].s, mc[1].s, mn[1].s, fw[1].s,
    };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 1, 14, v);
}
#endif

static void draw(float l, float t, float r, float b, float u0, float v0, float us, float vs_, int backdrop,
                 float mip, float bright, float dw, float dh) {
    CB c;
    c.rect[0] = l; c.rect[1] = t; c.rect[2] = r; c.rect[3] = b;
    c.src[0] = (float)tex_w; c.src[1] = (float)tex_h; c.src[2] = dw; c.src[3] = dh;
    c.prm[0] = (float)render_settings.filter; c.prm[1] = mip; c.prm[2] = bright; c.prm[3] = (float)backdrop;
    c.uvx[0] = u0; c.uvx[1] = v0; c.uvx[2] = us; c.uvx[3] = vs_;
    c.ext[0] = (float)render_settings.upscale; c.ext[1] = c.ext[2] = c.ext[3] = 0;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cbuf, 0, NULL, &c, 0, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}

/* RECOMP_SHOT=N[,N...]: save the presented window image of those fields as
   shot_N.bmp in the data folder (to check filters on real GPU output) */
static void shot_check(UINT cw, UINT ch) {
    static long long n;
    n++;
    const char *e = getenv("RECOMP_SHOT");
    if (!e) return;
    int hit = 0;
    for (const char *p = e; p && *p; ) {
        if (atoll(p) == n) hit = 1;
        p = strchr(p, ',');
        if (p) p++;
    }
    if (!hit) return;
    ID3D11Texture2D *bb = NULL, *st = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            char name[600];
            snprintf(name, sizeof name, "%s/shot_%06lld.bmp", g_data_dir, n);
            FILE *f = fopen(name, "wb");
            if (f) {
                BITMAPFILEHEADER bf = {0};
                BITMAPINFOHEADER bi = {0};
                bi.biSize = sizeof bi; bi.biWidth = (LONG)cw; bi.biHeight = -(LONG)ch; bi.biPlanes = 1; bi.biBitCount = 32;
                bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + cw * ch * 4;
                fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
                for (UINT y = 0; y < ch; y++) fwrite((const uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, cw, f);
                fclose(f);
                LOG("saved %s", name);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
}

int render_present(const uint32_t *fb, int w, int h) {
    if (!dev) return -1;
    RECT cr;
    GetClientRect(rwnd, &cr);
    UINT cw = cr.right > 0 ? (UINT)cr.right : 1, ch = cr.bottom > 0 ? (UINT)cr.bottom : 1;
    if (IsIconic(rwnd)) return 0;
    if (cw != buf_w || ch != buf_h) {
        release_rtv();
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return -1;
        buf_w = cw; buf_h = ch;
        if (make_rtv()) return -1;
    }
    if (ensure_texture(w, h)) return -1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, fb, (UINT)w * 4, 0);
    ID3D11DeviceContext_GenerateMips(ctx, srv);

    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)cw, (float)ch, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11SamplerState *ss[2] = {samp_lin, samp_point};
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
#ifdef RECOMP_HD
    {
        HDCB hcb;
        hd_frame(&hcb);
        hd_bind(&hcb);
    }
#endif

    /* picture size in pixels for the chosen aspect mode */
    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    float l = (float)(-pw / cw), r = (float)(pw / cw), t = (float)(ph / ch), b = (float)(-ph / ch);

    if (mode == 0 && pw < cw - 1) {
        /* blurred, dimmed copy of the picture zoomed to cover the whole window */
        double cover_w = cw, cover_h = cw / aspect;
        if (cover_h < ch) { cover_h = ch; cover_w = ch * aspect; }
        float us = (float)(cw / cover_w), vs2 = (float)(ch / cover_h);
        float mip = (float)(tex_mips > 3 ? 3 : tex_mips - 1);
        draw(-1, 1, 1, -1, (1 - us) / 2, (1 - vs2) / 2, us, vs2, 1, mip, 0.5f, (float)cw, (float)ch);
    }
    draw(l, t, r, b, 0, 0, 1, 1, 0, 0, 1, (float)pw, (float)ph);
    shot_check(cw, ch);
    IDXGISwapChain_Present(swap, 0, 0);
    return 0;
}
