// panel_state.cpp — panel state, event log, sli.ini persistence.
// The three snapshots (value / applied / saved) that drive the header's
// divergence indicator, the bounded event ring, and the ini read/write.
// The ini is THE host's file: Save writes key-by-key with
// WritePrivateProfileString, which rewrites the whole file per call — a
// key can never appear twice, and every control is always-value (what you
// see is what gets sent and saved).

#include "panel_internal.h"

#include <cstdlib>
#include <cstring>

namespace {

wchar_t g_iniPath[MAX_PATH] = {};

int ReadIniInt(const wchar_t* key, int def)
{
    wchar_t buf[64] = {}, defBuf[32] = {};
    _snwprintf_s(defBuf, 32, _TRUNCATE, L"%d", def);
    GetPrivateProfileStringW(L"sli", key, defBuf, buf, 64, g_iniPath);
    return _wtoi(buf);
}

float ReadIniFloat(const wchar_t* key, float def)
{
    wchar_t buf[64] = {}, defBuf[32] = {};
    _snwprintf_s(defBuf, 32, _TRUNCATE, L"%.3f", (double) def);
    GetPrivateProfileStringW(L"sli", key, defBuf, buf, 64, g_iniPath);
    return (float) _wtof(buf);
}

} // namespace

PanelState g_st;

void SetDefaults()
{
    for (int i = 0; i < kNumControls; ++i)
        g_st.value[i] = kSpecs[i].def;
}

float SendValue(int i)
{
    return g_st.value[i];
}

// ---------------------------------------------------------------------------
// Event log: bounded ring, newest first (kLogCap lives in the header).
// ---------------------------------------------------------------------------
LogEvent g_events[kLogCap];
int g_eventCount = 0;

void Event(const char* fmt, ...)
{
    LogEvent ev;
    ev.tick = GetTickCount64();
    ev.bad = fmt[0] == '!';
    if (ev.bad) ++fmt;  // render without the marker
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(ev.text, sizeof(ev.text), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_eventCount < kLogCap)
        g_events[g_eventCount++] = ev;
    else
    {
        memmove(g_events, g_events + 1, sizeof(LogEvent) * (kLogCap - 1));
        g_events[kLogCap - 1] = ev;
    }
    sli::Log("panel: %s", ev.text);
}

// ---------------------------------------------------------------------------
// ini persistence.
// ---------------------------------------------------------------------------
void BuildIniPath()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (slash != nullptr) *slash = 0;
    _snwprintf_s(g_iniPath, MAX_PATH, _TRUNCATE, L"%ls\\sli.ini", exe);
}

void LoadIni()
{
    SetDefaults();

    for (int i = 0; i < kNumControls; ++i)
    {
        const ControlSpec& s = kSpecs[i];
        wchar_t key[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, s.key, -1, key, 64);
        if (s.type == Ctrl::Checkbox || s.type == Ctrl::Combo)
        {
            // clamp to the row's range: a hand-edited sli.ini must not feed
            // an out-of-range combo index (empty preview) or a set verb the
            // host then clamps differently
            int v = ReadIniInt(key, (int) s.def);
            if (v < (int) s.lo) v = (int) s.lo;
            if (v > (int) s.hi) v = (int) s.hi;
            g_st.value[i] = (float) v;
        }
        else
        {
            float v = ReadIniFloat(key, s.def);
            if (v < s.lo) v = s.lo;
            if (v > s.hi) v = s.hi;
            g_st.value[i] = v;
        }
    }
    // (R95 audit: the saved-snapshot copy is gone with saved[] — nothing
    //  reads it; the divergence indicator compares value vs applied only.)
    // Until the first Apply nothing was sent from THIS session; the
    // indicator says so rather than pretending parity with the host.
    g_st.appliedValid = false;
}

// Returns false when any write failed (disk/permissions) — the caller
// surfaces it in the event log (R89c: the return used to be ignored, a
// failed Save looked identical to a good one).
bool SaveIni()
{
    bool ok = true;
    for (int i = 0; i < kNumControls; ++i)
    {
        const ControlSpec& s = kSpecs[i];
        wchar_t key[64] = {}, val[48] = {};
        MultiByteToWideChar(CP_UTF8, 0, s.key, -1, key, 64);
        if (s.type == Ctrl::Checkbox || s.type == Ctrl::Combo)
            _snwprintf_s(val, 48, _TRUNCATE, L"%d", (int) SendValue(i));
        else
            _snwprintf_s(val, 48, _TRUNCATE, L"%.3f", (double) SendValue(i));
        // WritePrivateProfileString returns FALSE only on real failures
        // (locked file, full disk); a same-value write still succeeds.
        if (!WritePrivateProfileStringW(L"sli", key, val, g_iniPath))
            ok = false;
    }
    return ok;
}

// R89e: called from WndProc(WM_ACTIVATE) — the panel reloads sli.ini every
// time its window comes to the foreground, so external edits (ctl.exe,
// hand edits, another session) never leave a stale UI. Unsent in-session
// edits are discarded (the file wins) and the event log says so.
void PanelReloadIniOnActivate()
{
    LoadIni();
    Event("config reloaded from sli.ini (window activated — external "
          "edits picked up, unsent changes discarded)");
}
