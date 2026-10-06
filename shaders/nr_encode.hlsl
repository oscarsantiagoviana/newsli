// nr_encode.hlsl — GPU encode: HDR linear -> display BGRA8 for the vendor
// NR model. Port of the POC's nr_codec.hlsl CSMain (encode only; the
// full-frame decode died with the pre-SR delivery path).
//
// The model consumes BGRA8 sRGB (an RGBA16F INPUT crashes the runtime with
// 0xC0000409 — validated on sm_86), so each sealed HDR frame is encoded
// first: Reinhard d/(1+d), clamp 1, pow(1/2.2). Exposure is
// 0.18/median(lum), with the median computed from the PREVIOUS frame's tile
// grid (1-frame lag, fork style: exposure drifts slowly between frames, the
// lag is invisible and avoids a per-frame GPU round-trip).
//
// The tile grid (8x8 px -> 1 mean) is reduced on the CPU with nth_element
// over ~5k values — 50x cheaper than walking every pixel on the CPU.

// ---- encode ----
Texture2D<float4>           gInHdr  : register(t0);
Texture2D<float2>           gInMv   : register(t1);   // R84: motion vectors,
                                              // RENDER dims (guideW x guideH),
                                              // full-frame pixel units
RWTexture2D<unorm float4>   gOut8   : register(u0);   // BGRA8
RWStructuredBuffer<uint>    gTiles  : register(u1);   // f16 bits of the per-tile mean
RWStructuredBuffer<uint2>   gFlow   : register(u2);   // R84: per-tile max |MV|
                                              // in WORK px, packed f16 x2

cbuffer CodecParams : register(b0)
{
    uint  gWidth, gHeight;       // WORK dims (encode dst = model input)
    uint  gTilesX, gTilesY;
    float gExpoScale;   // 0.18 / median (lag 1 frame)
    uint  gSrcW, gSrcH; // dims of the ORIGINAL HDR (coverage box when != work)
    uint  gMvW, gMvH;   // R84: dims of the MV texture (gSrcW/H today)
    float gJx, gJy;     // golpe 3b/R92: the frame's render-px jitter. The
                        // -j sample applies WHENEVER NON-ZERO (structural
                        // de-jitter — R92 killed the flag; jitter==0 =
                        // POST-SR = plain Load, bit-identical identity).
                        // ORDER MATCHES the C++ EncCb (mvW/H then jx/jy).
};

groupshared float gsLum[64];
groupshared float gsFlowX[64];   // R84: per-thread max |MV| (work px)
groupshared float gsFlowY[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 dtid : SV_DispatchThreadID,
            uint gi : SV_GroupIndex)
{
    // When the model runs below render-res (nrWorkScale, fork
    // DlssNrWorkingScale / Mode 2 downsample), the input pixel must be the
    // EXACT box average of its footprint — not a bilinear tap (most of the
    // image would never reach the model and the response would move with
    // the sub-pixel jitter). 1:1 falls through to a plain load.
    float3 c = 0.0.xxx;
    if (dtid.x < gWidth && dtid.y < gHeight)
    {
        if (gSrcW == gWidth && gSrcH == gHeight)
        {
            // golpe 3b/R92: 1:1 case — Load cannot sub-pixel shift, so the
            // de-jittered tap is a manual bilinear of 4 Loads (no sampler
            // object needed — exact and cheap on the shared heap). Fires
            // only when the frame carries jitter (j != 0); POST-SR takes
            // the plain Load and is bit-identical.
            if (gJx != 0.0 || gJy != 0.0)
            {
                const float fx = (float) dtid.x + 0.5 - gJx;
                const float fy = (float) dtid.y + 0.5 - gJy;
                const int   ix = clamp((int) floor(fx), 0, (int) gSrcW - 1);
                const int   iy = clamp((int) floor(fy), 0, (int) gSrcH - 1);
                const float ax = saturate(fx - floor(fx));
                const float ay = saturate(fy - floor(fy));
                const int   ix1 = min(ix + 1, (int) gSrcW - 1);
                const int   iy1 = min(iy + 1, (int) gSrcH - 1);
                c = gInHdr.Load(int3(ix,  iy,  0)).rgb * ((1-ax)*(1-ay))
                  + gInHdr.Load(int3(ix1, iy,  0)).rgb * (ax*(1-ay))
                  + gInHdr.Load(int3(ix,  iy1, 0)).rgb * ((1-ax)*ay)
                  + gInHdr.Load(int3(ix1, iy1, 0)).rgb * (ax*ay);
            }
            else
            {
                c = gInHdr.Load(int3(dtid.x, dtid.y, 0)).rgb;
            }
        }
        else
        {
            // golpe 3b/R92: the footprint shifts by -jitter (render px)
            // when the frame carries one — the coverage box already
            // interpolates, this is exact. j==0 = plain box (POST-SR,
            // bit-identical).
            const float djx = -gJx;
            const float djy = -gJy;
            const float x0 = ((float) dtid.x * (float) gSrcW) / (float) gWidth + djx;
            const float x1 = ((float) (dtid.x + 1) * (float) gSrcW) / (float) gWidth + djx;
            const float y0 = ((float) dtid.y * (float) gSrcH) / (float) gHeight + djy;
            const float y1 = ((float) (dtid.y + 1) * (float) gSrcH) / (float) gHeight + djy;
            const float area = max((x1 - x0) * (y1 - y0), 1e-12);
            const int i0 = (int) floor(x0);
            const int i1 = (int) ceil(x1) - 1;
            const int j0 = (int) floor(y0);
            const int j1 = (int) ceil(y1) - 1;
            float3 acc = 0.0.xxx;
            for (int j = j0; j <= j1; ++j)
            {
                const int jj = clamp(j, 0, (int) gSrcH - 1);
                const float aY = max(y0, (float) j);
                const float bY = min(y1, (float) j + 1.0);
                const float wy = max(bY - aY, 0.0);
                for (int i = i0; i <= i1; ++i)
                {
                    const int ii = clamp(i, 0, (int) gSrcW - 1);
                    const float aX = max(x0, (float) i);
                    const float bX = min(x1, (float) i + 1.0);
                    acc += gInHdr.Load(int3(ii, jj, 0)).rgb *
                           (max(bX - aX, 0.0) * wy);
                }
            }
            c = acc / area;
        }
    }
    // R84 FLOW: this work pixel's |MV| in WORK pixels, sampled at the same
    // coverage the colour used (the MV texture is RENDER-sized: work px ->
    // MV px = srcW/gW). Negative MVs (RG16F family) carry the real sign —
    // the absolute is taken HERE; a 0/0 unorm family folds everything to 0
    // and the auto-reset simply never fires (fail-soft: never a false
    // reset, only a missing reset — the diagnostic knob shows it).
    float2 flow = 0.0.xx;
    if (dtid.x < gWidth && dtid.y < gHeight)
    {
        const float2 mvPx = float2((float) dtid.x * (float) gMvW / (float) gWidth,
                                   (float) dtid.y * (float) gMvH / (float) gHeight);
        flow = abs(gInMv.Load(int3((int) mvPx.x, (int) mvPx.y, 0)).xy)
             * (float(gWidth) / float(gMvW));
    }
    c = max(c, 0.0.xxx);
    gsLum[gi] = dot(c, float3(0.2126, 0.7152, 0.0722));
    gsFlowX[gi] = flow.x;
    gsFlowY[gi] = flow.y;
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0)
    {
        float s = 0.0;
        [unroll]
        for (int i = 0; i < 64; ++i) s += gsLum[i];
        gTiles[gid.y * gTilesX + gid.x] = f32tof16(s * (1.0 / 64.0));
        // R84: per-tile PEAK |MV| (max, not mean — the ghost fires on the
        // tile whose history is most displaced, not the average tile)
        float fx = 0.0, fy = 0.0;
        [unroll]
        for (int i = 0; i < 64; ++i)
        {
            fx = max(fx, gsFlowX[i]);
            fy = max(fy, gsFlowY[i]);
        }
        gFlow[gid.y * gTilesX + gid.x] =
            uint2(f32tof16(fx), f32tof16(fy));
    }

    if (dtid.x >= gWidth || dtid.y >= gHeight) return;
    // R82f: fork-parity proxy (dlssnr.hlsl Mode 2). The Reinhard + median
    // exposure proxy "handed the model a scene value of 1.0 as 0.55 — flat,
    // dark, and nothing like the finished frame it was trained on. The model
    // then synthesised weakly" (the fork measured exactly our symptom:
    // washed-out input, weakly-synthesised wrong-hue answer). The fork's
    // default shows the model a scene-referred picture: white-point
    // normalise (paper white = 1.0 here), a soft knee that rolls highlights
    // off above 0.75 luma instead of clipping, per-channel peak headroom to
    // keep hue, then proper sRGB.
    float3 d = c * gExpoScale;                      // white point = frame exposure
                                                    // (the fork's gWhitePoint:
                                                    // paper white at 1.0)
    float luma = dot(d, float3(0.2126, 0.7152, 0.0722));
    if (luma > 0.75)
    {
        float rolled = 0.75 + 0.25 * (1.0 - exp(-(luma - 0.75) / 0.25));
        d *= rolled / luma;
    }
    float peak = max(d.r, max(d.g, d.b));
    if (peak > 1.0)
        d /= peak;
    d = saturate(d);
    // proper sRGB transfer (exact branch)
    float3 srgb = d / 12.92;
    float3 hi = select(d >= 0.0031308.xxx, 1.0.xxx, 0.0.xxx);
    srgb = lerp(srgb, 1.055 * pow(max(d, 0.0031308.xxx), 1.0 / 2.4) - 0.055.xxx,
                hi);
    // Write PER COMPONENT (never packed bytes): a unorm float4 over BGRA8
    // maps .r->R BY NAME — the CPU codec wrote bytes and its (b,g,r)
    // swizzle doubled up here into an R<->B swap (blue skins).
    gOut8[dtid.xy] = float4(srgb, 1.0);
}
