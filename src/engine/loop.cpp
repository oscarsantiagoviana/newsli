// sli_engine — shared transport + per-frame loop (gain-only).
//
// This process runs no upscaler: the game's native NGX does, on GPU 0 via
// the proxy. The engine is ONLY the NR worker on GPU 1:
//   sealed color N -> encode -> vendor NR evaluate -> decode GAIN -> bufOut
// The engine's ONLY output is the display-domain multiplicative GAIN
// RGBA16F (1.0 = compose changes nothing — "zero-delta" is the slang for
// that identity delivery). When the vendor model is not armed (init
// failed, nrOn=0, DRS bypass, dead streak) the delivery is the UNITY
// buffer. There is no echo path and no badge anywhere.
#include "engine/engine_ctx.h"
#include "shared/log.h"

#include <algorithm>
#include <cstring>

// ---- pitch/format tables (POC-proven; footprints reject TYPELESS and
// empirically SINT/UINT members — declare the FLOAT member of the family) —

static unsigned BppFor(unsigned fmt)
{
    switch (fmt)
    {
        case 1: case 2: case 3: case 4: // R32G32B32A32
            return 16;
        case 5: case 6: case 7: case 8: // R32G32B32
        case 9: case 10: case 11: case 12: case 13: case 14: case 15: // 16bppx4
        case 16: case 17: case 18: case 19: // R32G32
        case 20: case 21: case 22: case 23: // R32G8X24
            return 8;
        case 56: case 57: case 58: case 59: // R16
            return 2;
        default:
            return 4; // R8G8B8A8, R32, R16G16, R10G10B10A2, ...
    }
}

static unsigned PitchFor(unsigned width, unsigned bpp)
{
    return (width * bpp + 255u) & ~255u;
}

static bool OpenSharedBuf(EngineCtx& c, unsigned long long hv, UINT64 bytes,
                          UINT64 heapOffset,
                          ComPtr<ID3D12Heap>& heap, ComPtr<ID3D12Resource>& buf,
                          const char* what)
{
    HRESULT hr = c.dev->OpenSharedHandle((HANDLE) hv, IID_PPV_ARGS(&heap));
    if (FAILED(hr))
    {
        sli::Log("%s: OpenSharedHandle heap failed 0x%08X", what, (unsigned) hr);
        return false;
    }
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    hr = c.dev->CreatePlacedResource(heap.Get(), heapOffset, &d,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr))
    {
        sli::Log("%s: CreatePlacedResource failed 0x%08X", what, (unsigned) hr);
        return false;
    }
    return true;
}

static bool MakeLocalTex(EngineCtx& c, DXGI_FORMAT fmt, unsigned w, unsigned h,
                         D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& tex,
                         const char* what)
{
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Flags = flags;
    const D3D12_RESOURCE_STATES state =
        (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
            ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            : D3D12_RESOURCE_STATE_COMMON;
    HRESULT hr = c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state,
                                                nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr))
        sli::Log("%s: CreateCommittedResource failed 0x%08X", what, (unsigned) hr);
    return SUCCEEDED(hr);
}

static DXGI_FORMAT FootprintFmtFor(unsigned fmt)
{
    switch (fmt)
    {
        case 1: case 3: case 4: return (DXGI_FORMAT) 2;
        case 5: case 7: case 8: return (DXGI_FORMAT) 6;
        case 9: case 11: case 12: case 13: case 14: case 15: return (DXGI_FORMAT) 10;
        case 16: case 18: case 19: return (DXGI_FORMAT) 17;
        case 20: case 22: case 23: return (DXGI_FORMAT) 21;
        case 24: case 26: return (DXGI_FORMAT) 25;
        case 28: case 29: case 30: case 31: return (DXGI_FORMAT) 29;
        case 33: case 35: case 36: return (DXGI_FORMAT) 34;
        case 40: case 42: case 43: return (DXGI_FORMAT) 41;
        case 45: case 47: case 48: return (DXGI_FORMAT) 46;
        case 56: case 58: case 59: return (DXGI_FORMAT) 57;
        default: return (DXGI_FORMAT) fmt;
    }
}

static D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint(DXGI_FORMAT fmt, unsigned w,
                                                    unsigned h, unsigned pitch)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    fp.Footprint.Format = FootprintFmtFor((unsigned) fmt);
    fp.Footprint.Width = w;
    fp.Footprint.Height = h;
    fp.Footprint.Depth = 1;
    fp.Footprint.RowPitch = pitch;
    return fp;
}

bool OpenSharedTransport(EngineCtx& c)
{
    c.map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, c.args.mapName.c_str());
    if (c.map == nullptr)
    {
        sli::Log("OpenFileMapping failed (err %u)", GetLastError());
        return false;
    }
    c.scalars = (sli::FrameScalars*) MapViewOfFile(c.map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sli::MAP_SIZE);
    if (c.scalars == nullptr)
    {
        sli::Log("MapViewOfFile failed (err %u)", GetLastError());
        return false;
    }
    auto* hs = reinterpret_cast<sli::Handshake*>((uint8_t*) c.scalars +
                                                 sli::HANDSHAKE_OFFSET);
    if (hs->magic != sli::HANDSHAKE_MAGIC)
    {
        sli::Log("handshake magic missing (%llx)",
                 (unsigned long long) hs->magic);
        return false;
    }
    if (hs->version != sli::ABI_VERSION)
    {
        sli::Log("handshake version %u != engine %u — host/engine mismatch",
                 hs->version, sli::ABI_VERSION);
        return false;
    }
    c.hs = hs;
    c.args.hic = hs->hic;
    c.args.hoc = hs->hoc;
    c.args.hpf = hs->hpf;
    c.args.hdf = hs->hdf;
    // real game guides (triple buffer): metadata + buffers
    c.guideDepthFmt = hs->guideDepthFmt;
    c.guideMvFmt = hs->guideMvFmt;
    c.guideDepthPitch = hs->guideDepthPitch;
    c.guideMvPitch = hs->guideMvPitch;
    c.guideW = hs->guideW;
    c.guideH = hs->guideH;
    c.depthInverted = hs->depthInverted;

    // The return buffer is RENDER-sized (symmetric with the sealed input):
    // dims (w,h), format cf.
    const unsigned nrPitch = PitchFor(c.args.w, BppFor(c.args.cf));
    const UINT64 nrBytes = (UINT64) nrPitch * c.args.h;
    c.nrBytes = nrBytes;

    if (!OpenSharedBuf(c, c.args.hic, nrBytes, 0,
                       c.heapInColor, c.bufInColor, "inColor"))
        return false;

    // R90 (#11): open the out heap's THREE ring slots. The heap is 3x the
    // single-buffer size; slot k sits at k * RoundUp64K(nrBytes) (the
    // abi.h layout contract — the host places them identically).
    c.outSlotStride = (nrBytes + 65535) / 65536 * 65536;
    for (uint64_t k = 0; k < sli::kOutRingSlots; ++k)
    {
        char nm[32];
        snprintf(nm, sizeof(nm), "out%llu", (unsigned long long) k);
        if (!OpenSharedBuf(c, c.args.hoc, nrBytes, k * c.outSlotStride,
                           c.heapOut, c.bufOut[k], nm))
            return false;
    }

    if (!MakeLocalTex(c, (DXGI_FORMAT) c.args.cf, c.args.w, c.args.h,
                      D3D12_RESOURCE_FLAG_NONE, c.texInColor, "texInColor") ||
        !MakeLocalTex(c, (DXGI_FORMAT) c.args.cf, c.args.w, c.args.h,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, c.texOut,
                      "texOut"))
        return false;

    // R82l honesty fix: the transport carries a multiplicative GAIN, not an
    // additive delta — bufZero is filled with UNITY (1.0), and "zero-delta"
    // is the slang for the fail-safe identity delivery (compose changes
    // nothing). Kept as the function/segment name because the log strings
    // are tooling contract.
    {
        D3D12_HEAP_PROPERTIES up {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = nrBytes;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(c.dev->CreateCommittedResource(
                &up, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&c.bufZero))))
        {
            sli::Log("zero-delta buffer creation failed");
            return false;
        }
        uint8_t* p = nullptr;
        D3D12_RANGE rd {};
        if (FAILED(c.bufZero->Map(0, &rd, (void**) &p)))
        {
            sli::Log("zero-delta buffer map failed");
            return false;
        }
        // Per-pixel RGBA16F: rgb = 1.0 (0x3C00), a = 1.0 — bit-identical to
        // what nr_delta.hlsl writes for a NEUTRAL frame (R81: the transport
        // carries a multiplicative GAIN; unity = compose changes nothing),
        // so the host cannot distinguish "identity model" from "not armed".
        const uint8_t px[8] = { 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C, 0x00, 0x3C };
        for (UINT64 i = 0; i < nrBytes; i += 8)
            memcpy(p + i, px, 8);
        c.bufZero->Unmap(0, nullptr);
    }

    // open the 6 guide bufs + local textures with the game's RAW format.
    // Without guides (guideW==0) the vendor Init will fail and the engine
    // stays in zero-delta mode (synthetic guides are gone for good).
    if (c.guideW && c.guideH)
    {
        const UINT64 gdBytes = (UINT64) c.guideDepthPitch * c.guideH;
        const UINT64 gmBytes = (UINT64) c.guideMvPitch * c.guideH;
        for (int i = 0; i < 3; ++i)
        {
            char nm[32];
            snprintf(nm, sizeof(nm), "guideDepth%d", i);
            if (!OpenSharedBuf(c, hs->hgd[i], gdBytes, 0, c.heapGuideD[i],
                               c.bufGuideD[i], nm))
                return false;
            snprintf(nm, sizeof(nm), "guideMV%d", i);
            if (!OpenSharedBuf(c, hs->hgm[i], gmBytes, 0, c.heapGuideM[i],
                               c.bufGuideM[i], nm))
                return false;
        }
        // Local depth uses the handshake fmt (e.g. 41 = R32_FLOAT): the
        // host already delivers floats (planar SRV of the game's R32G8X24
        // plane 0). Flags NONE — same as the MV.
        if (!MakeLocalTex(c, (DXGI_FORMAT) c.guideDepthFmt, c.guideW, c.guideH,
                          D3D12_RESOURCE_FLAG_NONE, c.texGuideDepth,
                          "texGuideDepth"))
            return false;
        // MV: flags NONE, SRV <-> COPY_DEST cycle.
        if (!MakeLocalTex(c, (DXGI_FORMAT) c.guideMvFmt, c.guideW, c.guideH,
                          D3D12_RESOURCE_FLAG_NONE, c.texGuideMV,
                          "texGuideMV"))
            return false;
        sli::Log("guides open: depth fmt %u pitch %u, mv fmt %u pitch %u, "
                 "%ux%u, depthInverted=%u",
                 c.guideDepthFmt, c.guideDepthPitch, c.guideMvFmt,
                 c.guideMvPitch, c.guideW, c.guideH, c.depthInverted);
    }
    else
        sli::Log("guides: host delivered none - vendor will not arm");

    if (FAILED(c.dev->OpenSharedHandle((HANDLE) c.args.hpf,
                                       IID_PPV_ARGS(&c.produceFence))) ||
        FAILED(c.dev->OpenSharedHandle((HANDLE) c.args.hdf,
                                       IID_PPV_ARGS(&c.doneFence))))
    {
        sli::Log("fence open failed (err %u)", GetLastError());
        return false;
    }
    sli::Log("transport open: produce=%llu done=%llu",
             (unsigned long long) c.produceFence->GetCompletedValue(),
             (unsigned long long) c.doneFence->GetCompletedValue());
    return true;
}

// Fail-safe delivery for a broken frame: bufOut slot <- bufZero through a
// standalone command list + fence (valid whatever state the transport
// textures were left in — buffers need no transitions for copies).
// R90 (#11): delivers into the RING SLOT of the frame it is failing, so
// the host's slot-N compose reads a coherent unity gain.
static bool ZeroDeltaToOut(EngineCtx& c, uint64_t slotFrame)
{
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&al))) ||
        FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        al.Get(), nullptr, IID_PPV_ARGS(&cl))))
    {
        // R89d: this is the fail-safe identity path — a silent false here
        // would leave the engine shipping garbage with no trace.
        sli::Log("zero-delta: command objects failed");
        return false;
    }
    cl->CopyBufferRegion(c.bufOut[slotFrame % sli::kOutRingSlots].Get(), 0,
                         c.bufZero.Get(), 0, c.nrBytes);
    if (FAILED(cl->Close()))
    {
        sli::Log("zero-delta: close failed");
        return false;
    }
    ID3D12CommandList* cls[] = { cl.Get() };
    c.queue->ExecuteCommandLists(1, cls);
    ComPtr<ID3D12Fence> fce;
    if (FAILED(c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fce))))
        return false;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c.queue->Signal(fce.Get(), 1);
    fce->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    return true;
}

int RunFrameLoop(EngineCtx& c)
{
    uint64_t frame = 0;
    unsigned timeouts = 0;
    unsigned nrDeadStreak = 0;
    ULONGLONG nrDeadProbeTick = 0;   // R95 audit: latched-streak probe clock
    uint64_t nrSeq = 0;
    // R90 (#2): event wait on the produce fence — Sleep(1) polling rounds
    // to the timer quantum (up to 15.6 ms on this OS with no
    // timeBeginPeriod caller) and that jitter landed INSIDE the engine's
    // produce->delta latency. The done side already used this primitive.
    HANDLE produceEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const volatile sli::Tuning* tuning =
        (const volatile sli::Tuning*) ((const uint8_t*) c.scalars +
                                       sli::TUNING_OFFSET);

    // Vendor backend init (once): in-process NR model. On failure the
    // engine stays alive in unity mode (no echo — the fail-safe is
    // base * 1.0 = the untouched game frame).
    bool vendorOn = false;
    // The ONE segment-submit lambda (R89b: a second identical one lived
    // inside the frame loop; its only delta was the log format).
    auto FlushSeg = [&](const char* what) -> bool {
        const HRESULT hr = c.cmd->Close();
        if (FAILED(hr))
        {
            sli::Log("seg %s close failed 0x%08X", what, (unsigned) hr);
            return false;
        }
        ID3D12CommandList* cl = c.cmd.Get();
        c.queue->ExecuteCommandLists(1, &cl);
        ++c.localFenceVal;
        c.queue->Signal(c.localFence.Get(), c.localFenceVal);
        c.localFence->SetEventOnCompletion(c.localFenceVal, c.localFenceEvt);
        if (WaitForSingleObject(c.localFenceEvt, 120000) != WAIT_OBJECT_0)
        {
            sli::Log("seg %s: fence wait FAILED (GPU hang?)", what);
            return false;
        }
        return true;
    };
    auto BeginSeg = [&c]() -> bool {
        return SUCCEEDED(c.cmd->Reset(c.alloc.Get(), nullptr));
    };
    auto knobOr = [](volatile float v, float dflt) {
        return (v >= 0.0f) ? (float) v : dflt;
    };
    if (tuning->magic == sli::TUNING_MAGIC)
    {
        wchar_t cwd[MAX_PATH] {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        vendorOn = c.vendor.Init(c.dev.Get(), c.cmd.Get(), cwd,
                                 c.args.w, c.args.h,
                                 c.texInColor.Get(), c.texOut.Get(),
                                 c.texGuideDepth.Get(), c.texGuideMV.Get(),
                                 c.guideW, c.guideH, c.depthInverted,
                                 c.guideDepthFmt, c.guideMvFmt,
                                 knobOr(tuning->nrIntensity, 1.0f),
                                 knobOr(tuning->nrLocalStructure, 1.0f),
                                 knobOr(tuning->nrLocalTone, 1.0f),
                                 knobOr(tuning->nrSkinStructure, -1.0f),
                                 tuning->nrStyle,
                                 knobOr(tuning->nrWorkScale, 1.0f),
                                 tuning,
                                 BeginSeg, FlushSeg);
        if (vendorOn)
            c.vendor.lastParamSeq = tuning->nrParamSeq;
        else
            sli::Log("vendor: init FAILED - NR falls back to zero-delta");
    }

    for (;;)
    {
        // wait for the NEXT frame; a timeout must NOT advance the frame
        // counter (it would desync from the game's numbering forever).
        // R90 (#2): SetEventOnCompletion + WaitForSingleObject — no
        // Sleep(1) quantum in the latency chain. Semantics preserved
        // exactly: the 60 s budget, the timeout counter/log, and the
        // no-advance rule. (SetEventOnCompletion on an already-completed
        // value signals immediately — same behavior as the poll.)
        ULONGLONG start = GetTickCount64();
        bool got = false;
        while (GetTickCount64() - start < 60000)
        {
            if (c.produceFence->GetCompletedValue() >= frame + 1)
            {
                got = true;
                break;
            }
            if (FAILED(c.produceFence->SetEventOnCompletion(frame + 1,
                                                            produceEvt)))
            {
                // rare (fence about to be destroyed): poll fallback
                Sleep(1);
                continue;
            }
            const ULONGLONG used = GetTickCount64() - start;
            const DWORD waitMs = used >= 60000 ? 0
                              : (DWORD) (60000 - used);
            if (WaitForSingleObject(produceEvt, waitMs) == WAIT_OBJECT_0 &&
                c.produceFence->GetCompletedValue() >= frame + 1)
            {
                got = true;
                break;
            }
        }
        if (!got)
        {
            ++timeouts;
            sli::Log("produce wait timeout #%u (next frame %llu)", timeouts,
                     (unsigned long long) (frame + 1));
            continue;
        }
        if (c.produceFence->GetCompletedValue() == UINT64_MAX)
        {
            sli::Log("poison - exiting");
            c.vendor.Shutdown();
            CloseHandle(produceEvt);
            return 0;
        }
        timeouts = 0;
        // R89a fixup (watchdog-loop regression): the gate label must be the
        // HOST frame whose seal this turn consumes — the produce fence
        // value read NOW, right after the wait confirmed it. The engine's
        // private 'frame' counter runs at model speed (130 ms) and falls
        // behind the host's numbering whenever produces outpace the run
        // (the wait fires instantly on an already-advanced fence), so
        // labelling with it starved the gate's host==N check: 10 s
        // watchdog -> drain -> re-arm, forever. A post-run
        // GetCompletedValue() is equally wrong the other way (A4: the host
        // seals N+1/N+2 while we evaluate).
        const uint64_t sealedFrame = c.produceFence->GetCompletedValue();
        ++frame;

        try
        {
        // this frame's render dims (DRS): dims different from the args
        // mean the model geometry no longer matches — bypass with a zero
        // delta (guard R60)
        const unsigned rw = c.scalars->renderW ? c.scalars->renderW : c.args.w;
        const unsigned rh = c.scalars->renderH ? c.scalars->renderH : c.args.h;
        const bool nrDimsOk = rw == c.args.w && rh == c.args.h;
        // R95 audit: nrDeadStreak used to latch at 10 with NO recovery —
        // a transient failure burst (driver hiccup, TDR recovery) silently
        // disabled NR for the rest of the process lifetime while the host
        // saw healthy unity deliveries. Now a latched streak re-probes
        // once per 5 s (max one wasted evaluate attempt per window).
        const bool nrProbing = nrDeadStreak >= 10 &&
                               (GetTickCount64() - nrDeadProbeTick) > 5000;
        const bool nrWant = tuning->magic == sli::TUNING_MAGIC &&
                            tuning->nrOn != 0 && nrDimsOk &&
                            (nrDeadStreak < 10 || nrProbing);
        if (!nrWant && (nrSeq == 0 || (nrSeq % 600) == 0))
            sli::LogRate(5000, "zero-delta this frame (on=%u streak %u, dims "
                        "%ux%u vs %ux%u)",
                        tuning->magic == sli::TUNING_MAGIC ? tuning->nrOn : 0u,
                        nrDeadStreak, rw, rh, c.args.w, c.args.h);

        bool computed = false;
        // R84 history auto-reset: the PREVIOUS frame's flow percentile over
        // the knob's threshold (latched here, consumed by ProcessFrame's
        // reset arg). 0 knob = off, byte-identical to pre-R84. The reset
        // costs one cold-denoise frame — that is the documented trade.
        // R90 (#6): compute the peak ONLY when the knob is armed — with
        // nrFlowReset=0 the percentile ran every frame and was discarded.
        const float thr = knobOr(tuning->nrFlowReset, 15.0f) * 0.01f
                        * (float) c.args.h * knobOr(tuning->nrWorkScale, 1.0f);
        const float flowPct = thr > 0.0f ? c.vendor.FlowPeakPx() : 0.0f;
        const bool flowReset = (thr > 0.0f) && (flowPct > thr);
        static uint64_t s_lastResetFrame = 0;   // log throttle (one per event)
        if (flowReset && frame != s_lastResetFrame)
        {
            s_lastResetFrame = frame;
            sli::Log("frame %llu: history reset (flow %.1f px > %.1f)",
                     (unsigned long long) frame, flowPct, thr);
        }
        if (nrWant && vendorOn)
        {
            // knobs moved on the panel -> rebuild (they are CREATE-time:
            // release + re-create of the feature)
            if (tuning->nrParamSeq != c.vendor.lastParamSeq)
            {
                if (!c.vendor.RebuildKnobs(
                        knobOr(tuning->nrIntensity, 1.0f),
                        knobOr(tuning->nrLocalStructure, 1.0f),
                        knobOr(tuning->nrLocalTone, 1.0f),
                        knobOr(tuning->nrSkinStructure, -1.0f),
                        tuning->nrStyle, BeginSeg, FlushSeg))
                {
                    vendorOn = false; // rebuild failed: zero-delta until re-arm
                    sli::Log("vendor: knob rebuild failed - zero-delta");
                }
                else
                    c.vendor.lastParamSeq = tuning->nrParamSeq;
            }
        }
        if (nrWant && vendorOn)
        {
            // R90 (#3): ONE submit per frame. The three segments (in-copy /
            // vendor-frame / out-copy) shared one queue and one list shape;
            // the flushes between them only forced the CPU to wait twice
            // more than the GPU ordering already guaranteed. The vendor
            // tolerates pre-recorded commands in its list (the guide copies
            // rode the same segment since R84). The readback consumers
            // (UpdateGainNorm / UpdateExposure / FlowPeakPx) moved AFTER
            // this single flush — same lag-1 semantics they had (they ran
            // after the segment-2 flush then; now after the frame flush).
            LARGE_INTEGER qpc0, qpc1, qpf;
            QueryPerformanceCounter(&qpc0);
            if (FAILED(c.cmd->Reset(c.alloc.Get(), nullptr)))
                throw 6;

            // (1) seal N -> texInColor (shared buffer -> local texture)
            {
                const unsigned inPitch = PitchFor(rw, BppFor(c.args.cf));
                D3D12_RESOURCE_BARRIER bar {};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.pResource = c.texInColor.Get();
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                c.cmd->ResourceBarrier(1, &bar);
                D3D12_TEXTURE_COPY_LOCATION src { c.bufInColor.Get(),
                    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                src.PlacedFootprint = Footprint((DXGI_FORMAT) c.args.cf, rw, rh,
                                                inPitch);
                D3D12_TEXTURE_COPY_LOCATION dst { c.texInColor.Get(),
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                c.cmd->ResourceBarrier(1, &bar);
            }

            // (2) vendor frame in the SAME cmd session: copy guides ->
            //     encode -> evaluate -> decode-delta -> tiles readback
            // R90 (#5): the guide ring is indexed by the HOST frame whose
            // seal this turn consumes (sealedFrame), never the private
            // counter — in lockstep they coincide, but the moment the game
            // outproduces the engine (the fps levers make that likely) a
            // private-indexed lookup reads the wrong slot and mixes frames.
            const bool vend = c.vendor.ProcessFrame(
                c.cmd.Get(),
                tuning->forceReset != 0 || c.scalars->reset != 0 || flowReset,
                c.texInColor.Get(),
                c.bufGuideD[sealedFrame % 3].Get(),
                c.bufGuideM[sealedFrame % 3].Get(),
                c.guideDepthPitch, c.guideMvPitch,
                c.scalars->mvScaleX, c.scalars->mvScaleY,
                c.scalars->jitterX, c.scalars->jitterY);

            // (3) delta -> bufOut slot (UAV -> COPY_SOURCE -> copy -> UAV).
            // R90 (#11): the out transport is a 3-slot ring on the same
            // shared heap — the engine no longer blocks frame N+1 on the
            // host's compose of N.
            if (vend)
            {
                D3D12_RESOURCE_BARRIER bar {};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.pResource = c.texOut.Get();
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                c.cmd->ResourceBarrier(1, &bar);
                D3D12_TEXTURE_COPY_LOCATION srcO { c.texOut.Get(),
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                D3D12_TEXTURE_COPY_LOCATION dstO { c.bufOut[sealedFrame % sli::kOutRingSlots].Get(),
                    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                dstO.PlacedFootprint = Footprint((DXGI_FORMAT) c.args.cf,
                                                 c.args.w, c.args.h,
                                                 PitchFor(c.args.w,
                                                          BppFor(c.args.cf)));
                c.cmd->CopyTextureRegion(&dstO, 0, 0, 0, &srcO, nullptr);
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                c.cmd->ResourceBarrier(1, &bar);
            }
            if (!FlushSeg("frame"))
                throw 7;

            // R91: the readback consumers moved AFTER the done signal
            // (below). They feed the NEXT frame's expoScale/normR — the
            // delivered frame needs none of them — but their CPU medians
            // (3x nth_element over the tile grid) sat between the flush
            // and the Signal, i.e. INSIDE the game's gate wait. Same
            // single thread, same 1-frame lag; the next ProcessFrame
            // still sees them complete.
            if (vend)
                nrDeadStreak = 0;
            else
            {
                ++nrDeadStreak;
                if (nrProbing)
                {
                    nrDeadProbeTick = GetTickCount64();
                    sli::Log("vendor: probe failed (streak %u) - unity, "
                             "next probe in 5 s", nrDeadStreak);
                }
            }
            computed = vend;
            QueryPerformanceCounter(&qpc1);
            QueryPerformanceFrequency(&qpf);
            const double rtMs = (double) (qpc1.QuadPart - qpc0.QuadPart)
                                * 1000.0 / (double) qpf.QuadPart;
            if (nrSeq <= 5 || (nrSeq % 1800) == 0)   // R91b: 1/min heartbeat
                sli::Log("vendor frame %llu: %s (%.1f ms)",
                         (unsigned long long) sealedFrame,
                         computed ? "DELTA" : "identity", rtMs);
        }
        ++nrSeq;

        // (4) fail-safe: no vendor frame this turn -> UNITY gain
        //     (bufZero = 1.0 everywhere: compose changes nothing).
        //     Own segment still — the vendor path may have left the list
        //     closed by an exception; Reset on a poisoned list is UB.
        if (!computed)
        {
            if (FAILED(c.cmd->Reset(c.alloc.Get(), nullptr)))
                throw 6;
            c.cmd->CopyBufferRegion(c.bufOut[sealedFrame % sli::kOutRingSlots].Get(),
                                    0, c.bufZero.Get(), 0, c.nrBytes);
            if (!FlushSeg("zero"))
                throw 7;
        }

        c.scalars->engineResult = 1;
        // tag the delivery with the HOST frame actually processed: the done
        // fence carries the ENGINE counter (starts at 0 when the engine
        // spawns, lags behind) — the host needs ITS OWN frame number for
        // the guide-ring lookups. R89a: the label is sealedFrame, captured
        // right after the produce wait (see the capture site above): the
        // seal this turn consumes, immune to the A4 race and to the
        // private counter's drift behind the host numbering.
        c.scalars->engineHostFrame = sealedFrame;
        c.queue->Signal(c.doneFence.Get(), frame);

        // R91: readback consumers AFTER the signal (was: before it). The
        // medians feed the NEXT frame (lag-1 semantics unchanged — single
        // engine thread: they always complete before the next ProcessFrame
        // reads them); keeping them before the Signal billed ~1-3 ms of
        // pure CPU to the game's Present gate for nothing.
        if (computed)
        {
            c.vendor.UpdateExposure(c.scalars->preExposure);
            c.vendor.UpdateGainNorm();
        }

        if (frame == 1)
            sli::Log("first frame %s - delta-only pipeline live",
                     computed ? "computed" : "UNITY (vendor path not taken)");
        }
        catch (const std::exception& e)
        {
            // the model threw (or the list died): deliver a zero delta,
            // recreate the command objects and keep going with
            // engineResult=0. The engine NEVER dies on a broken frame.
            sli::Log("frame %llu threw std::exception: %s",
                     (unsigned long long) frame, e.what());
            ZeroDeltaToOut(c, sealedFrame);
            c.cmd.Reset();
            c.alloc.Reset();
            if (FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&c.alloc))) ||
                FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                c.alloc.Get(), nullptr,
                                                IID_PPV_ARGS(&c.cmd))))
            {
                sli::Log("command object recreation failed - exiting");
                CloseHandle(produceEvt);
                return 8;
            }
            c.scalars->engineResult = 0;
            c.queue->Signal(c.doneFence.Get(), frame);
        }
        catch (const int code)
        {
            // segment discipline throw (6/7): name it
            sli::Log("frame %llu threw segment code %d",
                     (unsigned long long) frame, code);
            ZeroDeltaToOut(c, sealedFrame);
            c.cmd.Reset();
            c.alloc.Reset();
            if (FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&c.alloc))) ||
                FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                c.alloc.Get(), nullptr,
                                                IID_PPV_ARGS(&c.cmd))))
            {
                sli::Log("command object recreation failed - exiting");
                CloseHandle(produceEvt);
                return 8;
            }
            c.scalars->engineResult = 0;
            c.queue->Signal(c.doneFence.Get(), frame);
        }
        catch (...)
        {
            // non-std throw (SE, ...) — same treatment:
            // zero-delta delivery, never engine death.
            sli::Log("frame %llu threw non-std exception",
                     (unsigned long long) frame);
            ZeroDeltaToOut(c, sealedFrame);
            c.cmd.Reset();
            c.alloc.Reset();
            if (FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                     IID_PPV_ARGS(&c.alloc))) ||
                FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                c.alloc.Get(), nullptr,
                                                IID_PPV_ARGS(&c.cmd))))
            {
                sli::Log("command object recreation failed - exiting");
                CloseHandle(produceEvt);
                return 8;
            }
            c.scalars->engineResult = 0;
            c.queue->Signal(c.doneFence.Get(), frame);
        }
    }
}
