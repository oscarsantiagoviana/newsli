// panel.cpp — the new-sli control panel entry point: Win32 + DX11
// bootstrap (standard Dear ImGui pattern) and the message loop that ties
// the 500 ms host poll to the frame. Everything else lives in
// panel_internal.h / panel_state.cpp / panel_host.cpp / panel_ui.cpp
// (R82d split: one concern per TU).

#include "panel_internal.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// The Win32 backend intentionally ships this declaration inside '#if 0'
// (to keep <windows.h> out of its header) — copy it here per its docs.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <d3d11.h>
#include <dxgi.h>

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

void CreateRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return;
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

void CleanupRenderTarget()
{
    if (g_rtv != nullptr) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL levels[] =
        { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = {};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, &got, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(  // headless: software raster
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, &got,
            &g_context);
    if (FAILED(hr))
        return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swapChain != nullptr) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context != nullptr) { g_context->Release(); g_context = nullptr; }
    if (g_device != nullptr) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return 0;
    switch (msg)
    {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                return 0;
            CleanupRenderTarget();
            if (g_device != nullptr && g_swapChain != nullptr)
            {
                const HRESULT hr = g_swapChain->ResizeBuffers(
                    0, (UINT) LOWORD(lParam), (UINT) HIWORD(lParam),
                    DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED(hr))
                    sli::Log("panel: !ResizeBuffers failed 0x%08X "
                             "(render skipped until it succeeds)",
                             (unsigned) hr);
            }
            CreateRenderTarget();
            return 0;
        case WM_ACTIVATE:
            // R89e (user report): the panel read sli.ini ONCE at process
            // start and lived in the tray — reopening the window showed a
            // STALE config whenever the ini was edited outside (ctl.exe,
            // hand edits, another panel session). Reload on every
            // activation: what you see is what the file says. Unsent edits
            // in THIS session are discarded (the file wins) — the event
            // log says so, so it is never a silent surprise.
            if (LOWORD(wParam) != WA_INACTIVE)
                PanelReloadIniOnActivate();
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Extra glyph ranges for the status marks: geometric shapes (dots), misc
// symbols (warning sign), general punctuation (dashes). Falls back to the
// embedded default font (the ASCII spellings still read fine) if the file
// is missing.
void LoadPanelFont()
{
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar ranges[] =
    {
        0x0020, 0x00FF,  // Basic Latin + Latin Supplement
        0x2010, 0x2027,  // dashes, quotes
        0x25A0, 0x25FF,  // geometric shapes (filled/hollow dot)
        0x2600, 0x26FF,  // misc symbols (warning sign)
        0,
    };
    ImFont* f = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", 0.0f, nullptr, ranges);
    sli::Log("panel: font %s", f != nullptr ? "segoeui" : "default");
}

} // namespace

// ---------------------------------------------------------------------------
// Entry: windowed GUI app (WIN32 subsystem), single instance (a second
// writer on the ctl seq/ack would interleave — mutex, POC parity).
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\sli_panel_once");
    if (once == nullptr)
        sli::Log("panel: !CreateMutex failed %u (single-instance guard "
                 "inactive)", (unsigned) GetLastError());
    else if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr,
                    L"new-sli panel is already running.",
                    L"new-sli control panel", MB_ICONINFORMATION);
        CloseHandle(once);
        return 0;
    }

    wchar_t exeDir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    if (wchar_t* s = wcsrchr(exeDir, L'\\')) *s = 0;
    wchar_t logPath[MAX_PATH] = {};
    _snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%ls\\sli_panel.log",
                 exeDir);
    char logA[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, logPath, -1, logA, MAX_PATH,
                        nullptr, nullptr);
    sli::LogInit(logA, 2 * 1024 * 1024, 2);
    sli::Log("panel: %s %s starting (ctl v%u, ImGui " IMGUI_VERSION ")",
             kPanelName, kPanelVersion, (unsigned) sli::CTL_VERSION);

    BuildIniPath();
    LoadIni();
    EnumGpus();

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"sli_panel", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName,
                                L"new-sli control panel",
                                WS_OVERLAPPEDWINDOW, 100, 100,
                                (int) (780 * scale), (int) (940 * scale),
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr || !CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        sli::Log("panel: !D3D11 init failed");
        if (once != nullptr) CloseHandle(once);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window layout is not config; sli.ini is
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 18.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    LoadPanelFont();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Event("panel started (%s %s)", kPanelName, kPanelVersion);

    bool done = false;
    ULONGLONG nextPoll = 0;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        PollHostIfDue(nextPoll);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##panel", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        DrawHeader();
        if (ImGui::BeginTabBar("tabs"))
        {
            DrawUpscalingTab();
            DrawFgTab();
            DrawNrTab();
            DrawDebugTab();
            DrawAboutTab();
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
        // a failed ResizeBuffers (device-removed) can leave g_rtv null:
        // skip the draw rather than feed a null RTV to the D3D11 calls
        if (g_rtv != nullptr)
        {
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_context->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        g_swapChain->Present(1, 0);
    }

    // teardown: backends -> context -> device (reverse init order)
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CloseCtl();
    sli::Log("panel: exit clean");
    if (once != nullptr) CloseHandle(once);
    return 0;
}
