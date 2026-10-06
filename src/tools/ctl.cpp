// sli_ctl.cpp — CLI tool to read/write live tuning fields through the ctl
// mapping (Local\sli_ctl_v3). Generalizes the POC's ctl_set_nrstage.exe:
//   sli_ctl                     -> dump the whole mirror
//   sli_ctl get <field>         -> read one field from the mirror
//   sli_ctl <field> <value>     -> set a field and wait for ack
// Exit codes: 0 ok, 1 usage, 2 mapping missing (host not running),
// 3 ack timeout.
#include "shared/ctl_common.h"
#include "shared/abi.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace sli;

static const char* FieldName(uint32_t f)
{
    switch (f)
    {
    case CTL_FIELD_OFFLOAD:     return "offload";
    // (3 was CAPTURE — hole since R89b; 30 was PRESR — hole since R95)
    case CTL_FIELD_FORCERESET:  return "forceReset";
    case CTL_FIELD_NR_INT:      return "nr.intensity";
    case CTL_FIELD_NR_LS:       return "nr.localStructure";
    case CTL_FIELD_NR_LT:       return "nr.localTone";
    case CTL_FIELD_NR_SS:       return "nr.skinStructure";
    case CTL_FIELD_NR_ON:       return "nr.on";
    case CTL_FIELD_GPU_INDEX:   return "gpuIndex";
    case CTL_FIELD_NR_STYLE:    return "nr.style";
    case CTL_FIELD_NR_WORKSCALE:return "nr.workScale";
    case CTL_FIELD_NR_BOOST:    return "nr.boost";
    case CTL_FIELD_NR_TINT:     return "nr.tint";
    case CTL_FIELD_NR_SPLIT:    return "nr.testSplit";
    // (27 PREEXPOSURE: hole since R89d — the host never had a case)
    // (30 PRESR: hole since R95 — the pre-SR placement was extirpated)
    case CTL_FIELD_NR_DETAIL:   return "nr.detail";
    case CTL_FIELD_NR_COLOUR:   return "nr.colour";
    case CTL_FIELD_NR_AUTOSKIN: return "nr.autoSkin";
    case CTL_FIELD_NR_HIGUARD:  return "nr.gainBound";
    case CTL_FIELD_NR_PSTRIDE:  return "nr.presentStride (REMOVED)";
    case CTL_FIELD_NR_FLOWRESET: return "nr.flowReset";
    case CTL_FIELD_NR_GATEWAIT:  return "nr.gateWaitMs";
    case CTL_FIELD_NR_PERFQ:     return "nr.perfQuality";
    case CTL_FIELD_NR_RATIOPIN:  return "nr.ratioPin";
    case CTL_FIELD_NR_DEPTHMODE: return "nr.depthMode";
    case CTL_FIELD_NR_RESIDUAL: return "nr.residual";
    default:                    return "?";
    }
}

int main(int argc, char** argv)
{
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, CTL_MAP_NAME);
    if (!map)
    {
        std::printf("no ctl mapping (host not running?)\n");
        return 2;
    }
    auto* m = (CtlMsg*) MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CtlMsg));
    if (!m) { CloseHandle(map); return 2; }

    int rc = 0;
    if (argc == 1)
    {
        // dump the whole mirror
        std::printf("host v%u hb=%u enginePid=%u\n", m->version, m->hb, m->enginePid);
        for (uint32_t f = 1; f < CTL_FIELD_COUNT; ++f)
        {
            if (f == 2) continue; // reserved
            std::printf("  %-16s %8.3f  (%s)\n", FieldName(f), m->mirror[f],
                        m->mirrorSource[f] ? "panel" : "game/default");
        }
    }
    else if (argc >= 3 && std::strcmp(argv[1], "get") == 0)
    {
        uint32_t f = (uint32_t) std::strtoul(argv[2], nullptr, 10);
        if (f == 0 || f >= CTL_FIELD_COUNT)
        {
            std::printf("bad field %u (valid: 1..%u)\n", f,
                        CTL_FIELD_COUNT - 1);
            return 1;
        }
        std::printf("%s = %.4f (src %u)\n", FieldName(f), m->mirror[f], m->mirrorSource[f]);
    }
    else if (argc >= 3)
    {
        uint32_t f = (uint32_t) std::strtoul(argv[1], nullptr, 10);
        if (f == 0 || f >= CTL_FIELD_COUNT)
        {
            std::printf("bad field %u (valid: 1..%u)\n", f,
                        CTL_FIELD_COUNT - 1);
            return 1;
        }
        float    v = (float) std::atof(argv[2]);
        // same barrier discipline as the panel's writer (panel_host.cpp)
        uint32_t seq = m->seq + 1;
        m->verb  = 2; // set
        m->field = f;
        m->fval  = v;
        InterlockedExchange((volatile LONG*) &m->seq, (LONG) seq);
        // wait for ack (up to 2 s)
        for (int i = 0; i < 200 && m->ack != seq; ++i) Sleep(10);
        if (m->ack != seq) { std::printf("ACK TIMEOUT\n"); rc = 3; }
        else std::printf("ACK OK %s = %.4f (mirror %.4f)\n",
                         FieldName(f), v, m->mirror[f]);
    }
    else
    {
        std::printf("usage: sli_ctl | sli_ctl get <field> | sli_ctl <field> <value>\n");
        rc = 1;
    }
    UnmapViewOfFile(m);
    CloseHandle(map);
    return rc;
}
