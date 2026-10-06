// panel_ui.cpp — ImGui rendering: UI helpers, the row renderer (control +
// always-visible help), the tabs and the Apply/Save header. Every edit
// lands in g_st directly; the header's divergence indicator picks edits up
// by comparing snapshots — no per-control dirty flags to forget.

#include "panel_internal.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// UI helpers.
// ---------------------------------------------------------------------------

// Status glyphs. The default embedded font covers none of these, so a
// Windows UI font with the right ranges is loaded at init (panel.cpp);
// the ASCII spellings kick in only if that file is missing.
const char* Dot(bool on) { return on ? u8"\u25CF" : u8"\u25CB"; }  // ● ○
const char* kWarnMark = u8"\u26A0";                                // ⚠

void MutedText(const char* fmt, ...)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(
                             ImGuiCol_TextDisabled));
    ImGui::SetWindowFontScale(0.85f);
    va_list ap;
    va_start(ap, fmt);
    ImGui::TextV(fmt, ap);
    va_end(ap);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
}

// The always-visible help block under every control: WHAT / SENDS /
// DEVIATE + the live mirror readout. One layout for the whole panel
// (spec rule: pick one, be consistent).
void DrawHelp(const ControlSpec& s)
{
    MutedText("%s", s.what);
    ImGui::SetWindowFontScale(0.85f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(
                             ImGuiCol_TextDisabled));
    // re-arm tag: the table's `sends` text mentions the re-arm cost
    // ("a change re-arms ..."); an explicit "no re-arm" statement is the
    // opposite and must NOT light the tag (delta tint, boost)
    const bool rearms = strstr(s.sends, "re-arm") != nullptr &&
                        strstr(s.sends, "no re-arm") == nullptr;
    if (rearms)
    {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
        ImGui::Text("%s re-arms (~2 s)", kWarnMark);
        ImGui::PopStyleColor();
    }
    ImGui::Text("Sends: %s", s.sends);
    ImGui::Text("Deviating: %s", s.deviate);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

    // Mirror reality: the live mirror value (what the host has). Every
    // control is always-value — there is no "game wins" row anymore.
    if (s.ctlField > 0 && g_host.hostLive)
    {
        const float live = g_host.mirror[s.ctlField];
        // knobs legally hold -1 (e.g. skin structure follows Local
        // structure); show the mirror unconditionally for them
        if (live >= 0.0f)
            MutedText("Host: %.3f%s", (double) live,
                      g_host.mirrorSource[s.ctlField] ? " (panel)" : "");
        else if (s.ctlField == sli::CTL_FIELD_GPU_INDEX)
            MutedText("Host: -1 (auto — first non-game NVIDIA)");
        else
            MutedText("Host: -1 (vendor default)");
    }
    ImGui::Spacing();
}

// Render one table row (control + help).
void DrawControl(int i)
{
    if (i < 0 || i >= kNumControls)
    {
        // a missing table row is a programming error, not a UI state:
        // the static_asserts catch key mismatches, this catches a row
        // deleted while a tab still references its field id (the crash
        // was kSpecs[-1] feeding garbage to strlen)
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "!! ControlSpec row missing (id %d) !!", i);
        return;
    }
    const ControlSpec& s = kSpecs[i];
    ImGui::PushID(i);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

    switch (s.type)
    {
        case Ctrl::Checkbox:
        {
            bool b = g_st.value[i] != 0.0f;
            if (ImGui::Checkbox(s.label, &b))
                g_st.value[i] = b ? 1.0f : 0.0f;
            break;
        }
        case Ctrl::SliderF:
        {
            // R95c: quantize to the row's step — a drag lands on the grid
            // (no more 8.101), and the label shows only the decimals the
            // step can express (1 decimal at 0.1/0.5, 2 at 0.05).
            const float step = SliderStep(s.lo, s.hi);
            char fmt[16];
            snprintf(fmt, sizeof(fmt), "%%.%df",
                     step >= 0.1f ? 1 : 2);
            if (ImGui::SliderFloat(s.label, &g_st.value[i], s.lo, s.hi, fmt))
            {
                const float snapped = std::round(g_st.value[i] / step)
                                      * step;
                g_st.value[i] = snapped < s.lo ? s.lo
                             : snapped > s.hi ? s.hi : snapped;
            }
            break;
        }
        case Ctrl::SliderI:
        {
            int v = (int) g_st.value[i];
            // The split seam doubles as its own on/off: 0 = off. Give it an
            // explicit enable checkbox + a position slider that never has
            // to be dragged to zero (position is remembered while off).
            if (s.ctlField == sli::CTL_FIELD_NR_SPLIT)
            {
                static int seamPos = 500;   // remembered while disabled
                bool on = g_st.value[i] != 0.0f;
                if (on)
                    seamPos = v < 1 ? 1 : (v > 1000 ? 1000 : v);
                if (ImGui::Checkbox("Split A/B on", &on))
                {
                    // the checkbox is a first-class writer: without this the
                    // host mirror (still 0) reverts the tick on the next poll
                    // and the split "disables itself immediately" (R82d)
                    g_st.value[i] = on ? (float) seamPos : 0.0f;
                }
                if (on)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(
                        ImGui::GetContentRegionAvail().x * 0.55f);
                    if (ImGui::SliderInt("##seam", &seamPos, 1, 1000, "%d permille"))
                        g_st.value[i] = (float) seamPos;
                }
                break;
            }
            if (ImGui::SliderInt(s.label, &v, (int) s.lo, (int) s.hi))
                g_st.value[i] = (float) v;
            break;
        }
        case Ctrl::Combo:
        {
            // preview: live item name (the GPU combo builds its own list)
            const bool isGpu = s.ctlField == sli::CTL_FIELD_GPU_INDEX;
            char preview[160] = {};
            if (isGpu)
            {
                const int idx = (int) g_st.value[i];
                if (idx < 0)
                    strcpy_s(preview, "Auto (first fit)");
                else if (idx < g_host.gpuCount)
                    strcpy_s(preview, g_host.gpus[idx].name);
                else
                    _snprintf_s(preview, sizeof(preview), _TRUNCATE,
                                "GPU %d (not present)", idx);
            }
            else
            {
                const int idx = (int) g_st.value[i];
                const int n = (int) s.hi - (int) s.lo + 1;
                if (idx >= (int) s.lo && idx - (int) s.lo < n)
                    strcpy_s(preview, s.items[idx - (int) s.lo]);
            }
            if (ImGui::BeginCombo(s.label, preview))
            {
                if (isGpu)
                {
                    if (ImGui::Selectable("Auto (first fit)",
                                          g_st.value[i] < 0.0f))
                        g_st.value[i] = -1.0f;
                    for (int g = 0; g < g_host.gpuCount; ++g)
                        if (ImGui::Selectable(g_host.gpus[g].name,
                                              (int) g_st.value[i] == g))
                            g_st.value[i] = (float) g;
                }
                else
                {
                    const int n = (int) s.hi - (int) s.lo + 1;
                    for (int k = 0; k < n; ++k)
                    {
                        const char* item = s.items[k];
                        // "(planned)" convention: placeholder future
                        // features render disabled and unselectable
                        const bool planned =
                            strstr(item, "(planned)") != nullptr;
                        if (planned)
                            ImGui::BeginDisabled(true);
                        const int idx = (int) s.lo + k;
                        if (ImGui::Selectable(item,
                                              (int) g_st.value[i] == idx))
                            g_st.value[i] = (float) idx;
                        if (planned)
                            ImGui::EndDisabled();
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        default:
        {
            ImGui::Text("%s: %.3f", s.label, (double) g_st.value[i]);
            break;
        }
    }

    ImGui::PopID();
    DrawHelp(s);
}

// ---------------------------------------------------------------------------
// Tabs.
// ---------------------------------------------------------------------------
void DrawUpscalingTab()
{
    if (ImGui::BeginTabItem("Upscaling"))
    {
        ImGui::Spacing();
        // Draw order mirrors the kSpecs table (backend/offload/GPU first —
        // the R82i.5 game-override rows that used to follow are gone).
        DrawControl(KeyIdx("backend"));
        DrawControl(IdOf(sli::CTL_FIELD_OFFLOAD));
        DrawControl(IdOf(sli::CTL_FIELD_GPU_INDEX));
        ImGui::Spacing();
        ImGui::Separator();
        MutedText("Super Resolution: the game's own DLSS always runs on "
                  "the game's GPU. Additional SR backends (FSR, XeSS) are "
                  "a future item — none is integrated yet.");
        ImGui::Spacing();
        ImGui::EndTabItem();
    }
}

void DrawFgTab()
{
    if (ImGui::BeginTabItem("Frame Generation"))
    {
        ImGui::Spacing();
        DrawControl(KeyIdx("fgMode"));
        ImGui::Separator();
        ImGui::TextWrapped("Status: not designed yet.");
        MutedText("Frame Generation needs fixed in-list delivery; it will "
                  "be designed after the compose refactor. The combo "
                  "above is a placeholder so the pipeline shape stays "
                  "visible.");
        ImGui::EndTabItem();
    }
}

void DrawNrTab()
{
    if (ImGui::BeginTabItem("Neural Rendering"))
    {
        ImGui::Spacing();
        DrawControl(IdOf(sli::CTL_FIELD_NR_ON));
        DrawControl(IdOf(sli::CTL_FIELD_NR_STYLE));
        ImGui::Separator();
        MutedText("Model knobs — ALWAYS-VALUE: sent on every Apply, never "
                  "deferred to the game (the POC's fake Override checkbox "
                  "is gone). Style and strength changes rebuild the model "
                  "(~1 s).");
        ImGui::Spacing();
        ImGui::BeginDisabled(
            g_st.value[IdOf(sli::CTL_FIELD_NR_ON)] == 0.0f);
        DrawControl(IdOf(sli::CTL_FIELD_NR_INT));
        DrawControl(IdOf(sli::CTL_FIELD_NR_LS));
        DrawControl(IdOf(sli::CTL_FIELD_NR_PERFQ));   // R90 (#1)
        DrawControl(IdOf(sli::CTL_FIELD_NR_RATIOPIN)); // golpe 1b
        DrawControl(IdOf(sli::CTL_FIELD_NR_DEPTHMODE)); // golpe 2
        DrawControl(IdOf(sli::CTL_FIELD_NR_RESIDUAL)); // golpe 4 (dejitter structural R92)
        DrawControl(IdOf(sli::CTL_FIELD_NR_LT));
        DrawControl(IdOf(sli::CTL_FIELD_NR_SS));
        DrawControl(IdOf(sli::CTL_FIELD_NR_WORKSCALE));
        DrawControl(IdOf(sli::CTL_FIELD_NR_BOOST));
        DrawControl(IdOf(sli::CTL_FIELD_NR_DETAIL));    // R82k
        DrawControl(IdOf(sli::CTL_FIELD_NR_COLOUR));    // R82k
        DrawControl(IdOf(sli::CTL_FIELD_NR_AUTOSKIN));  // R82k
        DrawControl(IdOf(sli::CTL_FIELD_NR_HIGUARD));   // R82k
        ImGui::EndDisabled();
        if (g_st.value[IdOf(sli::CTL_FIELD_NR_ON)] == 0.0f)
            MutedText("Knobs are inert until Neural rendering is ON "
                      "(they still Apply and persist).");
        ImGui::EndTabItem();
    }
}

void DrawDebugTab()
{
    if (ImGui::BeginTabItem("Debug & Verification"))
    {
        ImGui::Spacing();
        DrawControl(IdOf(sli::CTL_FIELD_FORCERESET));
        DrawControl(IdOf(sli::CTL_FIELD_NR_TINT));
        DrawControl(IdOf(sli::CTL_FIELD_NR_SPLIT));
        DrawControl(IdOf(sli::CTL_FIELD_NR_FLOWRESET));
        DrawControl(IdOf(sli::CTL_FIELD_NR_GATEWAIT));  // R88
        ImGui::Separator();

        // host status + engine counters (spec status derivation)
        ImGui::Text("%s Host %s   %s Engine %s", Dot(g_host.hostLive),
                    g_host.hostLive ? "live" : "no host",
                    Dot(g_host.engineLive),
                    g_host.engineLive ? "composing" : "idle");
        MutedText("host heartbeat %s, protocol v%u, ping %u ms, engine "
                  "pid %u",
                  g_host.hostLive ? "fresh (<2 s)" : "stale",
                  (unsigned) g_host.hostVersion,
                  (unsigned) g_host.pingMs,
                  (unsigned) g_host.enginePid);
        if (g_host.countersValid)
        {
            ImGui::Text("produce %llu   done %llu   in flight %llu",
                        (unsigned long long) g_host.produceFrame,
                        (unsigned long long) g_host.doneFrame,
                        (unsigned long long) (g_host.produceFrame -
                                              g_host.doneFrame));
            MutedText("engineResult %u (1 = last delta computed)",
                      (unsigned) g_host.scalars->engineResult);
        }
        else
            MutedText("engine counters unavailable (no frame mapping — "
                      "the engine only exists while a session is armed)");
        ImGui::Separator();

        // event log: last ctl ops with ack state
        ImGui::Text("Event log (last %d):", g_eventCount);
        if (ImGui::BeginChild("events", ImVec2(0.0f, 0.0f), true))
        {
            for (int e = g_eventCount - 1; e >= 0; --e)
            {
                const LogEvent& ev = g_events[e];
                ImGui::PushStyleColor(ImGuiCol_Text, ev.bad
                    ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                    : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ImGui::Text("[%7llu.%03llu] %s",
                            (unsigned long long) (ev.tick / 1000u),
                            (unsigned long long) (ev.tick % 1000u), ev.text);
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
}

void DrawAboutTab()
{
    if (ImGui::BeginTabItem("About"))
    {
        ImGui::Spacing();
        ImGui::Text("%s %s", kPanelName, kPanelVersion);
        ImGui::Text("ctl protocol v%u (magic SLICTLv3), ABI v%u",
                    (unsigned) sli::CTL_VERSION,
                    (unsigned) sli::ABI_VERSION);
        ImGui::Text("Dear ImGui " IMGUI_VERSION " (Win32 + DX11 backends)");
        ImGui::Text("Build " __DATE__ " " __TIME__);
        ImGui::Spacing();
        ImGui::Text("Repository: link lands at publication.");
        ImGui::Text("License: MIT. Not affiliated with NVIDIA, AMD or "
                    "Intel.");
        MutedText("Credits: Dear ImGui and Detours vendored under their "
                  "own MIT licenses; the vendor NGX/DLSS-NR runtime is "
                  "user-provided and never distributed here. Design "
                  "derives from the POC's coproc_panel (681 lines of raw "
                  "Win32) via the analysis/03 review.");
        ImGui::EndTabItem();
    }
}

// Always-visible header: live dots, divergence indicator, Apply.
// R89f: Apply IS Apply+Save — one button sends the live batch AND
// persists sli.ini (the host reloads it at (re)arm). The indicator
// spells out how the UI state diverges from what was applied.
void DrawHeader()
{
    HostView& h = g_host;
    ImGui::Text("%s Host", Dot(h.hostLive));
    ImGui::SameLine();
    ImGui::Text("  %s Engine", Dot(h.engineLive));
    ImGui::SameLine();
    MutedText("protocol v%u%s", (unsigned) h.hostVersion,
              h.incompatible ? "  read-only: host protocol not v3" : "");

    // divergence ladder, most urgent first
    const char* indicator;
    if (h.incompatible)
        indicator = "READ-ONLY — host protocol not v3";
    else if (!g_st.appliedValid)
        indicator = "Not applied this session — Apply sends live + saves";
    else if (g_st.DiffersFrom(g_st.applied))
        indicator = "Unapplied changes — Apply to send live + save";
    else
        indicator = "Applied and saved — in sync";

    // single button, right-aligned on the indicator line
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("%s", indicator);
    ImGui::SameLine(w > 110.0f ? w - 110.0f : 0.0f);
    if (ImGui::Button("Apply", ImVec2(100.0f, 0.0f)))
        ApplyAll();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sends every control live (ctl batch) AND "
                          "persists sli.ini —\nthe host reloads the file "
                          "when the offload (re)arms.");
    ImGui::Separator();
}
