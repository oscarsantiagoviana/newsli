// nr_delta.hlsl — the engine's ONLY output (RGBA16F, render dims).
//
// Two payloads share this texture, selected by the nrTint FLAG:
//   0 (quality): the transported correction as a multiplicative GAIN
//                (R81) — the host composes it with a fixed-function
//                MULTIPLY onto the finished frame; neutral = 1.0.
//   1 (view):    the model's PURE display output (post tile-median) —
//                the host draws it OPAQUE over the frame (scissor-cut
//                at the split seam): "where and how is NR acting",
//                no eye-comparing subtleties. Not a paint, no mixing:
//                the screen IS the model output.
//
// The gain pipeline below runs in BOTH modes (tiles for the next frame's
// luma normalisation are always written) — flipping the flag never
// disturbs the quality path's calibration.

Texture2D<float4>      gInDisp : register(t0);   // model display output (work dims)
Texture2D<float4>      gOrigHdr : register(t1);  // ORIGINAL HDR (texInColor, render dims)
RWTexture2D<float4>    gOutDelta : register(u0); // gain or model output, render dims
RWStructuredBuffer<uint2> gGainTiles : register(u1); // per-tile gain RGB (f16 x3
                                                     // packed: x=RG, y=B pad)

cbuffer CodecDeltaParams : register(b0)
{
    uint  dWidth, dHeight;       // RENDER dims (delta dst)
    float dExpoScale;
    uint  dModelW, dModelH;      // dims of the model output (work dims)
    float dModelTop;             // R82f: first live row of the answer (the
                                 // runtime writes rows dModelTop..+dModelH;
                                 // rows above are its previous-frame slot)
    float dBoost;                // gain exponent (1 = exact; 0 = edit off)
    uint  dTint;                 // 1 = VIEW payload (pure model output)
    float dProbe;                // RESERVED (R89a: was the R82e probe flag —
                                 // 1 stashed raw texels into the tile tail
                                 // skipping the gain store; the host now
                                 // sends 0 unconditionally. Kept so the
                                 // cbuffer layout is unchanged; R92 removed
                                 // dJitterX/Y after it — 16 consts total)
    float3 dNormRGB;             // PREVIOUS frame's PER-CHANNEL gain medians
                                // (1-frame lag): a detail pass may not shift
                                // the frame's exposure NOR its colour — the
                                // luma-only anchor let the model's global
                                // blue bias through (measured B 0.85, R82
                                // P3; AMDNR anchors exposure per channel).
                                // (R92: dJitterX/Y REMOVED — the answer is
                                // sampled on the canonical grid; the seal
                                // and the depth stab put the model's input
                                // there, so a landing shift would
                                // double-correct.)
    float dDetail;               // R82k DETAIL STRENGTH: 1 = the model's
                                 // luminance verdict rides exact; 0 = edit
                                 // off; >1 amplifies the ratio (fork
                                 // transferStrength, our ctl 33).
    float dColour;               // R82k COLOUR STRENGTH: how much of the
                                 // MODEL'S OWN chroma shift passes (0.25
                                 // default; the forks ship ~0; ctl 34).
    float dGainBound;            // R84 PER-PIXEL GAIN BOUND (fork MaxRatio,
                                 // ctl 36): the composition bound itself,
                                 // 1..8 (default 2.0). Replaces the R82k
                                 // highlight-knee form (a bound of 1.0-1.12
                                 // global = edit off; the per-pixel guard
                                 // case is the fork's, not a special case).
    float dResidual;             // GOLPE 4 matched-residual: 1 = carry the
                                 // model's EDIT up from work res onto the
                                 // full-res proxy (edit = model - smallProxy,
                                 // re-seated on the full proxy) so the
                                 // downsample blur cancels instead of being
                                 // read as headroom. Only fires when the
                                 // model ran below render res (ws<1) — at
                                 // ws=1 the arithmetic collapses and this
                                 // is bit-identical to classic anyway.
    float dJx, dJy;              // R92b: this frame's jitter (render px).
                                 // The proxy/denominator samples shift by
                                 // -j so EVERYTHING the decode touches
                                 // lives in the canonical (un-jittered)
                                 // domain — the model's input was put
                                 // there by the seal and the depth stab.
                                 // 0 = POST-SR = plain Load identity.
};

float3 SanitizeFinite3(float3 v, float3 fallback)
{
    float3 r = fallback;
    if (isfinite(v.x) && isfinite(v.y) && isfinite(v.z)) r = v;
    return r;
}

// GOLPE 4 (fork dlssnr.hlsl:459): scale a residual so
// fullProxy + edit cannot leave the unit cube, without changing its
// direction. The peak channel that hits the wall first decides how far
// the WHOLE edit travels — one scalar on the triple keeps hue.
float CubeScaleResidual(float3 P, float3 T)
{
    // per-channel alpha_c = (1 - P_c) / (T_c - P_c); the edit may travel
    // until the FIRST channel saturates: alpha = min over channels with a
    // positive edit direction (negative directions have no wall below 0
    // worth guarding here — saturate at the end handles depth).
    float3 d = T - P;
    float a = 1.0;
    [unroll]
    for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)
            a = min(a, (1.0 - P[c]) / d[c]);
    }
    return saturate(a);
}

// GOLPE 4: the encode as a PURE FUNCTION (must match nr_encode.hlsl's
// per-pixel math exactly — white point E -> luma soft knee above 0.75 ->
// per-channel peak normalise -> saturate -> sRGB). Used to rebuild the
// SMALL proxy from the same bilinear taps that sample the model answer,
// so edit = model - smallProxy cancels the downsample kernel.
float3 EncodeProxy(float3 c, float E)
{
    float3 d = max(c, 0.0.xxx) * E;
    const float luma = dot(d, float3(0.2126, 0.7152, 0.0722));
    if (luma > 0.75)
    {
        const float rolled = 0.75 + 0.25 * (1.0 - exp(-(luma - 0.75) / 0.25));
        d *= rolled / luma;
    }
    const float peak = max(d.r, max(d.g, d.b));
    if (peak > 1.0)
        d /= peak;
    d = saturate(d);
    float3 srgb = d / 12.92;
    const float3 hi = select(d >= 0.0031308.xxx, 1.0.xxx, 0.0.xxx);
    srgb = lerp(srgb, 1.055 * pow(max(d, 0.0031308.xxx), 1.0 / 2.4) - 0.055.xxx,
                hi);
    return srgb;
}

[numthreads(8, 8, 1)]
void CSDecodeDelta(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= dWidth || dtid.y >= dHeight) return;
    const float E = dExpoScale;
    // R92b: `orig` sampled at -j (bilinear, clamped) — the gain's
    // DENOMINATOR (and the residual's fullProxy, and the VIEW payload)
    // move to the canonical domain the model's answer lives in. The old
    // plain Load left the proxy jittered while the answer was not: the
    // whole output rippled at the Halton frequency (a reported
    // output ripple). j==0 = the 4 taps collapse to the same texel —
    // bit-exact POST-SR identity.
    float3 orig;
    {
        const float fx = (float) dtid.x + 0.5 - dJx;
        const float fy = (float) dtid.y + 0.5 - dJy;
        const int ix = clamp((int) floor(fx), 0, (int) dWidth - 1);
        const int iy = clamp((int) floor(fy), 0, (int) dHeight - 1);
        const float ax = saturate(fx - floor(fx));
        const float ay = saturate(fy - floor(fy));
        const int ix1 = min(ix + 1, (int) dWidth - 1);
        const int iy1 = min(iy + 1, (int) dHeight - 1);
        orig = gOrigHdr.Load(int3(ix,  iy,  0)).rgb * ((1-ax)*(1-ay))
             + gOrigHdr.Load(int3(ix1, iy,  0)).rgb * (ax*(1-ay))
             + gOrigHdr.Load(int3(ix,  iy1, 0)).rgb * ((1-ax)*ay)
             + gOrigHdr.Load(int3(ix1, iy1, 0)).rgb * (ax*ay);
    }
    // The model responded at WORK dims, and only the HALF live region
    // (dModelH rows at dModelTop) carries the answer — the slot-pick
    // measured it (R82f). Map the delta FULL height onto that live
    // region: rp.y = dModelTop + uv.y * dModelH. The old mapping scaled
    // by dModelH first (answer squeezed into the top half, everything
    // below the clamp's last row stretched — the "split view" report),
    // and in quality mode gave the frame's bottom half the gain of ONE
    // model row (the washed bottom in the offline A/Bs).
    // R82f bars test (channel map, measured on the shared output): the
    // runtime writes byte0=B_real, byte1=R_real, byte2=G_real. With the
    // R8G8B8A8 view (.x=byte0) the swizzle .rgb rebuilds (R,G,B) real;
    // grey (R=G=B) is unaffected, which is why night frames looked
    // "plausible but wrong".
    // Live-region mapping (rp), computed for every pixel: with scale 1 and
    // top 0 it reduces to rp = dtid.xy, and the median window below
    // shares it. R92: the model's answer lives on the CANONICAL (un-
    // jittered) grid — its input was put there by the seal (-j sample)
    // and the depth stab. No -jitter landing shift (it would
    // double-correct; the old R82g.6 contract lived when the model saw
    // the JITTERED frame).
    float2 rp = float2(
        (float(dtid.x) + 0.5) / float(dWidth) * float(dModelW) - 0.5,
        dModelTop + (float(dtid.y) + 0.5) / float(dHeight)
                  * float(dModelH) - 0.5);
    // bilinear always: the decode's answer mapping is continuous (work ->
    // render at any ws), a nearest Load would quantise it back to whole
    // pixels and re-introduce stepping on the seam. (With scale 1 this
    // equals the old fast-path Load at integer dtid — same texels, one
    // extra tap.)
    const int2 b0 = (int2) floor(rp);
    const float2 fr = rp - (float2) b0;
    float3 modelDisp = 0.0.xxx;
    // GOLPE 4: the PURE bilinear (pre-median) of the model answer — kept
    // because the residual's smallProxy must be sampled through the
    // EXACT SAME kernel for edit = model - smallProxy to cancel the
    // downsample blur (a median on one side only would displace, not
    // cancel). The original-HDR bilinear rides the same taps.
    float3 origBil = 0.0.xxx;
    [unroll]
    for (int j = 0; j < 2; ++j)
    {
        [unroll]
        for (int i = 0; i < 2; ++i)
        {
            const int2 q = clamp(b0 + int2(i, j),
                                 int2(0, (int) dModelTop),
                                 int2((int) dModelW - 1,
                                      (int) dModelTop + (int) dModelH - 1));
            const float w = (i ? fr.x : 1 - fr.x) * (j ? fr.y : 1 - fr.y);
            modelDisp += gInDisp.Load(int3(q, 0)).rgb * w;
            // the ORIGINAL at the same work-res position, BILINEAR over
            // render coords (nearest here would alias the proxy side and
            // leak edit noise — the whole point is kernel symmetry).
            // R92b: rr carries the SAME -j the proxy's own sample got —
            // smallProxy and fullProxy must see the SAME domain or the
            // edit = model - smallProxy doesn't cancel the blur.
            const float2 rr = float2(
                ((float) q.x + 0.5) / float(dModelW) * float(dWidth) - 0.5 - dJx,
                ((float) q.y + 0.5 - dModelTop) / float(dModelH)
                    * float(dHeight) - 0.5 - dJy);
            const int2 ob = (int2) floor(rr);
            const float2 of = rr - (float2) ob;
            [unroll]
            for (int jj = 0; jj < 2; ++jj)
            {
                [unroll]
                for (int ii = 0; ii < 2; ++ii)
                {
                    const int2 oq = clamp(ob + int2(ii, jj), int2(0, 0),
                                          int2((int) dWidth - 1,
                                               (int) dHeight - 1));
                    const float ow = (ii ? of.x : 1 - of.x)
                                   * (jj ? of.y : 1 - of.y) * w;
                    origBil += gOrigHdr.Load(int3(oq, 0)).rgb * ow;
                }
            }
        }
    }
    // TILE-OUTLIER MEDIAN: the network answers per tile and an
    // extrapolated tile returns something nothing like its neighbours
    // (AMDNR measured max 4.16 in a 0.072-mean frame). A 3x3 median of
    // the model's display output kills blocky magenta/green patches in
    // flat sky before they ever form a gain. It also cleans the VIEW
    // payload — the pure output below is post-median.
    // R82f: 3x3 px was measured too fine — the model's answer carries
    // 1-px vertical stripes and a 3-tap horizontal window keeps them
    // (median of {stripe, scene, scene} = scene-half). A 9x9 window
    // (81 taps, ~11% stripe coverage) medians them away; the stride is
    // expressed in DELTA pixels (full frame), the offset lands inside
    // the live region only (rp mapping above: model rows beyond the
    // live half are the runtime's dead slot — never sampled).
    {
        float3 m9[81];
        const int yLo = (int) dModelTop;
        const int yHi = (int) dModelTop + (int) dModelH - 1;
        const float2 step2 = float2(float(dModelW) / float(dWidth),
                                    float(dModelH) / float(dHeight));
        [unroll]
        for (int j = 0; j < 9; ++j)
        {
            [unroll]
            for (int i = 0; i < 9; ++i)
            {
                const float2 p = rp + float2(i - 4, j - 4) * step2;
                const int2 q = clamp((int2) round(p),
                                     int2(0, yLo),
                                     int2((int) dModelW - 1, yHi));
                m9[j * 9 + i] = gInDisp.Load(int3(q, 0)).rgb;
            }
        }
        [unroll]
        for (int c = 0; c < 3; ++c)   // per-channel bubble median of 81
        {
            [unroll]
            for (int p = 0; p < 41; ++p)
            {
                [unroll]
                for (int q2 = p + 1; q2 < 81; ++q2)
                {
                    float vp = m9[p][c], vq = m9[q2][c];
                    if (vp > vq) { m9[p][c] = vq; m9[q2][c] = vp; }
                }
            }
        }
        modelDisp = m9[40];   // median
    }
    // ---- GOLPE 4: MATCHED RESIDUAL (fork dlssnr.hlsl:860-905) ----
    // When the model ran below render res, the classic compose below
    // compares a BLURRED-UP model picture against a full-res proxy: the
    // downsample blur is read as headroom the frame has (the
    // resolution-dependent colour shift, fork's divergence #1). Here the
    // proxy is rebuilt at FULL res from the ORIGINAL (a pure function of
    // the pixel — same math the encode used), and only the model's
    // EDIT comes up from small:
    //   smallProxy = EncodeProxy(origBil)   <- orig bilinear at the SAME
    //                                          work-res taps the answer used
    //   edit       = modelDisp - smallProxy
    //   modelDisp' = enc + CubeScale(edit)  <- re-seated on the full proxy
    // At ws=1 the arithmetic collapses (P + (m - p) = m) so the path
    // only fires when the model actually ran small — classic stays
    // bit-identical there.
    const bool modelRanSmall = (dModelW != dWidth) || (dModelH != dHeight);
    // R92d: the RAW model answer, captured BEFORE the residual
    // reconstruction — the VIEW payload paints THIS. Post-golpe-4 the
    // view had switched to the reconstruction (fullProxy+edit), which
    // at ws<1 is the game frame re-seated at full res: visually
    // indistinguishable from no-NR (user report: the NR
    // view appeared dead). The view is a DIAGNOSTIC
    // (what is the model doing) — it must show the model's own output.
    const float3 modelRaw = modelDisp;
    if (dResidual > 0.5 && modelRanSmall)
    {
        // 'enc' below is the full-res proxy of THIS pixel — but it is
        // computed after this block in the classic flow. Rebuild it here
        // from the same pure function (identical math, so no drift).
        const float3 fullProxy = EncodeProxy(orig, E);
        const float3 smallProxy = EncodeProxy(origBil, E);
        const float3 edit = modelDisp - smallProxy;
        const float a = CubeScaleResidual(fullProxy, fullProxy + edit);
        modelDisp = fullProxy + edit * a;
        // keep the model's empty-frame guard meaningful downstream
        modelDisp = max(modelDisp, 0.0.xxx);
    }
    // ---- THE GAIN (R82j: the forks' composed pattern, adapted to our
    // multiplicative contract). The model's picture is NOT a per-channel
    // ratio against our encode — it is a picture, re-anchored so the
    // frame's own luminance always decides brightness (the model only
    // adds the detail verdict), with a bilateral one-scalar guard. The
    // old per-channel model/enc ratio was exactly what the forks
    // measured as boiling + over-bright darks: an unbounded ratio on
    // near-black pixels, clamped per channel (a hue distorter).
    //   orig: the base this gain will multiply (display-referred, [0,1])
    //   modelDisp: the model's picture, post-median, at work res
    float3 o = max(orig, 0.0.xxx);
    // R84 B1: reproduce EXACTLY the encode the model consumed
    // (nr_encode.hlsl: white point E -> soft knee on luma above 0.75 ->
    // per-pixel peak normalise -> piecewise sRGB). The old Reinhard+2.2
    // reconstruction made `upgraded/enc` a CURVE ERROR even with a perfect
    // model: the displaced history multiplied by that error is the ghost
    // that survived the flow resets (split capture 2026-10-02: the ghost
    // stops at the compose seam — it is printed by this gain math).
    float3 d0 = o * E;
    const float l0 = dot(d0, float3(0.2126, 0.7152, 0.0722));
    if (l0 > 0.75)
    {
        const float rolled = 0.75 + 0.25 * (1.0 - exp(-(l0 - 0.75) / 0.25));
        d0 *= rolled / l0;
    }
    const float pk0 = max(d0.r, max(d0.g, d0.b));
    if (pk0 > 1.0)
        d0 /= pk0;
    d0 = saturate(d0);
    float3 enc = d0 / 12.92;
    const float3 hiS = select(d0 >= 0.0031308.xxx, 1.0.xxx, 0.0.xxx);
    enc = lerp(enc, 1.055 * pow(max(d0, 0.0031308.xxx), 1.0 / 2.4) - 0.055.xxx,
               hiS);

    // Strengths (fork knobs, our ctl — R82k now all live):
    //   transfer/boost: amplifies the luminance ratio (>1), 1 = exact.
    //   dDetail: how much of the model's luminance VERDICT rides on top
    //     (1 = exact, 0 = off) — applied as a deviation scale below.
    //   dColour: how much of the MODEL'S OWN chroma shift passes (the
    //     forks ship ~0 — model colour shifts are the part you do not
    //     want; we default 0.25).
    const float transfer = dBoost;
    const float colour = max(dColour, 0.0);

    // 1) the model's picture rides WHOLE (fork dlssnr.hlsl:928-963
    //    parity). In the fork the ratio term carries ONLY the
    //    above-knee headroom (originalLuma vs PROXY luma); the model's
    //    own luminance verdict enters THROUGH the picture and is
    //    measured afterwards by lumaRatio below. The R82j port
    //    re-anchored the model to the frame's luma in BOTH branches
    //    (frame vs MODEL): luma(upgraded) == origLuma by construction
    //    => lumaRatio == 1 ALWAYS — the verdict died before the guard
    //    ever saw it (quality NR invisible since R82j even with every
    //    knob maxed; the view payload shows modelDisp raw, which is why
    //    the view "had detail" the frame never received).
    //    In OUR transport proxyLuma == origLuma by construction (the
    //    compose space IS the encode, and the raw base o rides as the
    //    multiply base), so the fork's headroom term is identically
    //    zero here and parity means: NO re-anchor. Identity contract
    //    intact: modelDisp == enc => upgraded == enc => gain == 1.
    const float modelLuma = dot(modelDisp, float3(0.2126, 0.7152, 0.0722));
    const float origLuma  = dot(enc, float3(0.2126, 0.7152, 0.0722));
    float3 upgraded;
    if (modelLuma <= 1e-5)
    {
        upgraded = enc;   // empty model frame: no verdict, base untouched
    }
    else
    {
        upgraded = modelDisp;
    }

    // DETAIL STRENGTH is the forks' transfer lerp (dlssnr.hlsl:963):
    // upgraded = lerp(original, modelVerdict, saturate(strength)). At 0 the
    // frame is handed back untouched — the model's verdict (its warped
    // HISTORY, the ghosting source when the warp is short) does not reach
    // the composition at all. The pre-R82k.3 form scaled the ratio's
    // deviation, which still passed the full verdict at our shipped
    // boost=0 — the user's "edit off" was not neutral and its history
    // ghosting read as an offload defect.
    // (R95d BUG B: detail used to ALSO scale lumaRatio's deviation below
    //  — a second application; the fork applies strength ONCE, in this
    //  lerp. The lumaRatio measured on the lerped picture already carries
    //  the attenuation; doubling it put detail 0.5 at 25% verdict.)
    upgraded = lerp(enc, upgraded, saturate(max(dDetail, 0.0)));

    // 2) the DARK RATIO FLOOR (the forks' anti-boil): luminance ratio
    //    with a floor above and below falls smoothly to one where there
    //    is no light — no edit at all is the answer for an unlit pixel.
    const float kRatioFloor = 1.0 / 512.0;
    const float upgradedLuma = dot(upgraded, float3(0.2126, 0.7152, 0.0722));
    // ratio inside the ENCODE space (upgraded and enc live there; o does
    // not — a display-space denominator would break the identity contract:
    // model==encode must yield exactly 1)
    float lumaRatio = (upgradedLuma + kRatioFloor) / (origLuma + kRatioFloor);
    // (R95d BUG B: the second detail application lived here —
    //  lumaRatio = 1 + (lumaRatio-1)*detail. Removed: detail rides in the
    //  lerp above, fork parity.)
    // transfer strength >1 amplifies the ratio (still a ratio: boundable)
    lumaRatio = pow(max(lumaRatio, 1e-6), 1.0 + max(transfer - 1.0, 0.0));

    // 3) the GUARD — one scalar from luminance, bilateral, applied to the
    //    whole triple (per-channel bounds distort hue on saturated px).
    //    R84: the bound is the KNOB ITSELF (dGainBound, fork MaxRatio
    //    1..8, default 2.0) — per-pixel on the luma ratio. The R82k
    //    highlight-knee special case (×1.12 where the original is bright)
    //    is gone: the knee was a global patch for what is a per-pixel
    //    question, and the fork ships no such case (its guard :1016).
    const float guard = max(dGainBound, 1.0);
    const float bounded = clamp(lumaRatio, 1.0 / guard, guard);
    // result = base light (fork's luma-only end) + the model's relative
    // chroma at low strength. Both factors are 1 for an identity model =>
    // bit-exact: result == o, gain == 1.
    // R84: the chroma relative factor is BOUNDED by the same guard — an
    // unbounded chromaRel let the displaced history in through the side
    // door at colour strength (the luma guard clamped, the chroma ghost
    // printed anyway).
    const float3 chromaRel = upgraded / max(enc, (1.0 / 512.0).xxx);
    const float3 chromaKept =
        clamp(1.0.xxx + (chromaRel - 1.0.xxx) * colour,
              (1.0 / guard).xxx, guard.xxx);
    // (R95d BUG A: the fork's luma correction ("upgraded *= bounded /
    //  lumaRatio") pulls the COLOURED end of its picture blend back to
    //  the bounded luminance — its luma-only end (original*boundedRatio)
    //  is left UNCORRECTED on purpose. We had applied it to the WHOLE
    //  result: outside the guard it INVERTED the verdict (a 8x brighter
    //  pixel got gain 4/8 = 0.5, maximum darkening; a 4x darker one got
    //  2.0, maximum brightening). Every strong-contrast verdict was
    //  flattened — the "painted" look. Fix: our transport IS the
    //  luma-only end (a gain that multiplies the frame); the correction
    //  does not belong here at all.)
    float3 result = o * bounded * max(chromaKept, 0.0.xxx);

    // gain = what the Present multiplies the base by. The per-channel
    // median normalisation keeps its fixed point at 1 (a globally neutral
    // edit stays neutral — the runaway guard from R82e, fork pattern).
    const float3 nrm = clamp(dNormRGB, 0.5.xxx, 2.0.xxx);
    float3 gain = SanitizeFinite3(result / max(o, (1.0 / 512.0).xxx),
                                  1.0.xxx) / nrm;
    // R95 audit: the final clamp rides the SAME per-pixel guard as the
    // luma ratio (dGainBound, 1..8) — the old hard [0.5, 2.0] silently
    // killed the knob's documented 2..8 range (bound raised = no effect).
    gain = clamp(gain, (1.0 / guard).xxx, guard.xxx);

    // (5) per-tile gain RGB (f16 x3 packed in a uint2: x = R|G<<16,
    // y = B) for the NEXT frame's per-channel normalisation (CPU medians,
    // 1-frame lag, fork pattern) — written in EVERY mode so the quality
    // calibration survives view sessions.
    // (R89a: the R82e probe branch is gone with the dProbe flag — the
    // host sends 0 unconditionally and the sentinel path never ran.)
    if ((dtid.x & 7) == 0 && (dtid.y & 7) == 0)
    {
        // MUST match the CPU grid ((w+7)/8): the >>3 form desaligned the
        // readback one column per row (R82e probe finding)
        const uint tX = (dWidth + 7) >> 3, tY = (dHeight + 7) >> 3;
        const uint ti = (dtid.y >> 3) * tX + (dtid.x >> 3);
        gGainTiles[ti] = uint2(f32tof16(gain.r) | (f32tof16(gain.g) << 16),
                               f32tof16(gain.b));
    }

    // ONE store, two payloads (no early returns to starve the tiles):
    // quality = the gain; view = the model's RAW display output (R92d:
    // pre-residual — the reconstruction made the view indistinguishable
    // from the game frame and killed its diagnostic value).
    gOutDelta[dtid.xy] = float4(dTint != 0u ? modelRaw : gain, 1.0);
}
