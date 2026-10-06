// present_add.hlsl — the Present-gate compose stage (R79).
//
// A fullscreen triangle drawn ONTO THE SWAPCHAIN BACKBUFFER inside the
// game's Present call; frame N on screen is always composed from game
// frame N (R79 directive: no frame mixing, no per-frame skips; slowness
// is paid in fps, not fidelity).
//
// Draw modes (root constants, live every frame) — selected BY THE HOST:
//   quality (cTint==0): out = gain.rgb — the PSO blends MULTIPLICATIVELY
//              (Src=ZERO, Dest=SRC_COLOR); alpha out 0 preserves the
//              game's backbuffer alpha. Test split: right of the seam
//              the gain is forced to neutral 1 (the game's own output).
//   view    (cTint!=0): out = the model's PURE display output — the PSO
//              draws OPAQUE (Src=ONE, Dest=ZERO): the screen IS the
//              model output, no mixing. The host scissors the draw to
//              the left of the split seam; no per-pixel logic here.
Texture2D<float4> gDelta : register(t0);  // gain OR model output, render-res
SamplerState      gLin   : register(s0);  // linear tap, clamp addressing

cbuffer PresentAddParams : register(b0)
{
    float cBoost;      // reserved (the boost is an engine-side exponent)
    uint  cTint;       // 0 = quality gain; non-zero = view (opaque payload)
    uint  cTestSplit;  // seam position in permille of width: 0 = off
    uint  cOutW;       // backbuffer width (seam line measured in pixels)
};

// Fullscreen triangle from SV_VertexID (no vertex buffer): ids 0,1,2 map
// to uv (0,0),(2,0),(0,2) — one triangle covers the whole viewport.
void VSMain(uint vid : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 uv : TEXCOORD0)
{
    uv = float2((vid << 1) & 2, vid & 2);
    pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // delta is render-res, the backbuffer display-res: one normalized
    // linear tap maps corner to corner (the sampler does the scaling).
    const float4 d = gDelta.Sample(gLin, uv);
    float3 v = d.rgb;
    if (cTint == 0u && cTestSplit != 0u)
    {
        // quality + split: right of the seam the game's original output —
        // the NEUTRAL gain (the view mode is cut by the host's scissor
        // instead, so its NR half reaches the seam untapered)
        const float seam = (float) cTestSplit * 0.001;
        if (uv.x >= seam)
            v = 1.0.xxx;
        // 2 px divider centred on the seam: one DARK px + one LIGHT px —
        // a multiply-only 1.3 divider disappears on bright scenes (the
        // daylight street clamps it to 1.0); a dark px is visible on
        // bright ground, a light px on dark ground. Always visible.
        const float px = abs(uv.x - seam) * (float) cOutW;
        if (px < 1.0)
            v = float3(0.25, 0.25, 0.25);
        else if (px < 2.0)
            v = float3(1.3, 1.3, 1.3);
    }
    // never compose garbage: neutral for the gain, black for the view
    if (!all(isfinite(v)))
        v = cTint == 0u ? 1.0.xxx : 0.0.xxx;
    return float4(v, 0.0);  // alpha out 0: the game's alpha survives
}
