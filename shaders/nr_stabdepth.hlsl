// nr_stabdepth.hlsl — R92: de-jitter the DEPTH guide.
//
// The 3b seal de-jitter stabilised only the COLOUR plane: the model saw a
// still colour against a depth that swims +-j with the Halton cycle —
// every depth edge oscillated (the user's residual "oscilaciones" with
// dej ON). This pass resamples the depth with the SAME global -j shift
// the colour got, so the colour/depth PAIR stays aligned to first order
// (both planes share the same parallax error; it cancels between them).
//
// The MV guide is NOT touched: the NGX MV contract EXCLUDES jitter.
//
// Dispatched ONLY when (jx|jy) != 0 (pre-SR frames). In POST-SR the
// jitter is 0 by construction: no dispatch, no transitions, no cost.

Texture2D<float>    gDepth  : register(t0);   // raw guide depth (R32_FLOAT, guide dims)
RWTexture2D<float>  gStab   : register(u0);   // stabilised copy (same dims)

cbuffer StabDepthParams : register(b0)
{
    uint  sWidth, sHeight;   // guide dims
    float sJx, sJy;          // THIS frame's render-px jitter
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= sWidth || dtid.y >= sHeight) return;
    // Same manual bilinear the 1:1 encode path uses (Load-based, no
    // sampler object): sample the depth at pixel - jitter. Log-depth
    // interpolates like the colour does — the smear the colour plane
    // already carries; the PAIR stays matched (that is the point).
    const float fx = (float) dtid.x + 0.5 - sJx;
    const float fy = (float) dtid.y + 0.5 - sJy;
    const int   ix = clamp((int) floor(fx), 0, (int) sWidth - 1);
    const int   iy = clamp((int) floor(fy), 0, (int) sHeight - 1);
    const float ax = saturate(fx - floor(fx));
    const float ay = saturate(fy - floor(fy));
    const int   ix1 = min(ix + 1, (int) sWidth - 1);
    const int   iy1 = min(iy + 1, (int) sHeight - 1);
    const float d00 = gDepth.Load(int3(ix,  iy,  0));
    const float d10 = gDepth.Load(int3(ix1, iy,  0));
    const float d01 = gDepth.Load(int3(ix,  iy1, 0));
    const float d11 = gDepth.Load(int3(ix1, iy1, 0));
    // clamp edge replicates (negative coords from -j): the border pixel's
    // depth extends out — same convention the colour bilinear assumes.
    const float d = d00 * ((1-ax)*(1-ay)) + d10 * (ax*(1-ay))
                  + d01 * ((1-ax)*ay)     + d11 * (ax*ay);
    gStab[dtid.xy] = d;
}
