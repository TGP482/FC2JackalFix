module;

#include <common.hxx>
#include <atomic>
#include <cstdint>
#include <d3d9.h>
#include <d3d10.h>
#include <dxgi.h>
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_dx10.h>

export module settingspanel;

import common;
import borderless;
import inputdevice;
import jackalfixmenu;

static constexpr int nToggleKey = VK_F10;

static bool bOpen = false;
static bool bToggleWasDown = false;
static bool bReleasePending = false;

enum class Backend
{
    None,
    DX9,
    DX10,
};

static Backend eBackend = Backend::None;
static void* pBackendDevice = nullptr;

static ImVec2 MousePos{};
static bool bCentreMouse = false;
static LARGE_INTEGER LastFrame{};

// Far Cry 2's rust orange on charcoal.
static constexpr ImVec4 PanelBg{ 0.06f, 0.055f, 0.05f, 0.94f };
static constexpr ImVec4 Border{ 0.80f, 0.42f, 0.12f, 0.85f };
static constexpr ImVec4 Text{ 0.92f, 0.90f, 0.86f, 1.0f };
static constexpr ImVec4 Highlight{ 0.34f, 0.16f, 0.05f, 1.0f };
static constexpr ImVec4 Heading{ 0.90f, 0.50f, 0.16f, 1.0f };
static constexpr ImVec4 Dim{ 0.62f, 0.58f, 0.52f, 1.0f };
static constexpr ImVec4 Frame{ 0.13f, 0.11f, 0.09f, 1.0f };
static constexpr ImVec4 FrameHovered{ 0.21f, 0.15f, 0.10f, 1.0f };
static constexpr ImVec4 Button{ 0.18f, 0.12f, 0.07f, 1.0f };
// Lighter than the highlight, so it shows on a selected row.
static constexpr ImVec4 ButtonHovered{ 0.46f, 0.23f, 0.07f, 1.0f };
static constexpr ImVec4 Line{ 0.90f, 0.50f, 0.16f, 0.35f };

static constexpr std::pair<ImGuiCol, ImVec4> PanelColors[]
{
    { ImGuiCol_WindowBg, PanelBg }, { ImGuiCol_Border, Border }, { ImGuiCol_Text, Text },
    { ImGuiCol_Header, Highlight }, { ImGuiCol_HeaderHovered, Highlight }, { ImGuiCol_HeaderActive, Highlight },
    { ImGuiCol_Button, Button }, { ImGuiCol_ButtonHovered, ButtonHovered }, { ImGuiCol_ButtonActive, ButtonHovered },
    { ImGuiCol_FrameBg, Frame }, { ImGuiCol_FrameBgHovered, FrameHovered }, { ImGuiCol_FrameBgActive, Highlight },
    { ImGuiCol_ScrollbarGrab, Highlight }, { ImGuiCol_ScrollbarGrabHovered, ButtonHovered },
    { ImGuiCol_ScrollbarGrabActive, Heading }, { ImGuiCol_Separator, Line }, { ImGuiCol_NavHighlight, Heading },
};

// Keyboard keys the panel reads, polled rather than taken from window messages: the game owns those.
static constexpr std::pair<int, ImGuiKey> PanelKeys[]
{
    { VK_UP, ImGuiKey_UpArrow }, { VK_DOWN, ImGuiKey_DownArrow },
    { VK_LEFT, ImGuiKey_LeftArrow }, { VK_RIGHT, ImGuiKey_RightArrow },
    { VK_PRIOR, ImGuiKey_PageUp }, { VK_NEXT, ImGuiKey_PageDown },
    { VK_HOME, ImGuiKey_Home }, { VK_END, ImGuiKey_End },
    { VK_RETURN, ImGuiKey_Enter }, { VK_SPACE, ImGuiKey_Space },
    { VK_ESCAPE, ImGuiKey_Escape }, { VK_TAB, ImGuiKey_Tab },
};

// XINPUT_GAMEPAD button bits.
static constexpr std::pair<uint16_t, ImGuiKey> PadButtons[]
{
    { 0x0001, ImGuiKey_GamepadDpadUp }, { 0x0002, ImGuiKey_GamepadDpadDown },
    { 0x0004, ImGuiKey_GamepadDpadLeft }, { 0x0008, ImGuiKey_GamepadDpadRight },
    { 0x1000, ImGuiKey_GamepadFaceDown }, { 0x2000, ImGuiKey_GamepadFaceRight },
};

static bool IsKeyHeld(int nKey)
{
    return (GetAsyncKeyState(nKey) & 0x8000) != 0;
}

static bool IsGameForeground()
{
    auto hWnd = JackalFixGameWindow();
    return hWnd != nullptr && GetForegroundWindow() == hWnd;
}

static void EnsureContext()
{
    if (ImGui::GetCurrentContext() != nullptr)
        return;

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // Held arrows step ten times a second, ImGui's twenty is too fast for settings.
    io.KeyRepeatRate = 0.1f;
}

static void ShutdownBackend()
{
    if (eBackend == Backend::DX9)
        ImGui_ImplDX9_Shutdown();
    else if (eBackend == Backend::DX10)
        ImGui_ImplDX10_Shutdown();

    eBackend = Backend::None;
    pBackendDevice = nullptr;
}

// A new device, after a renderer restart, gets the backend again.
static bool EnsureBackend(Backend eWanted, void* pDevice)
{
    if (eBackend == eWanted && pBackendDevice == pDevice)
        return true;

    ShutdownBackend();

    auto bReady = eWanted == Backend::DX9
        ? ImGui_ImplDX9_Init(static_cast<IDirect3DDevice9*>(pDevice))
        : ImGui_ImplDX10_Init(static_cast<ID3D10Device*>(pDevice));

    if (!bReady)
        return false;

    eBackend = eWanted;
    pBackendDevice = pDevice;
    return true;
}

static void Toggle()
{
    bOpen = !bOpen;
    if (!bOpen)
        return;

    JackalFixPanelOpen();
    // Motion from before the open belongs to the game.
    TakeBlockedMouse();
    bCentreMouse = true;
}

static bool IsClosingKeyHeld()
{
    return IsKeyHeld(VK_ESCAPE) || IsKeyHeld(nToggleKey) || IsKeyHeld(VK_RETURN) || IsKeyHeld(VK_SPACE);
}

static void UpdateToggle()
{
    auto bDown = IsGameForeground() && IsKeyHeld(nToggleKey);
    if (bDown && !bToggleWasDown)
        Toggle();

    bToggleWasDown = bDown;
}

// Kept blocked after closing until every key, pad button and mouse button is up.
static void UpdateInputBlock()
{
    if (bOpen)
        bReleasePending = true;
    else if (bReleasePending && !IsClosingKeyHeld() && GetPadState().nButtons == 0 && TakeBlockedMouse().nButtons == 0)
        bReleasePending = false;

    SetGameInputBlocked(bOpen || bReleasePending);
}

static float FrameSeconds()
{
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);

    auto fSeconds = LastFrame.QuadPart != 0
        ? static_cast<float>(now.QuadPart - LastFrame.QuadPart) / static_cast<float>(frequency.QuadPart)
        : 1.0f / 60.0f;

    LastFrame = now;
    return std::clamp(fSeconds, 0.0001f, 0.25f);
}

static void FeedKeyboard(ImGuiIO& io, bool bForeground)
{
    for (const auto& [nVirtualKey, key] : PanelKeys)
        io.AddKeyEvent(key, bForeground && IsKeyHeld(nVirtualKey));
}

static void FeedPad(ImGuiIO& io)
{
    const auto& pad = GetPadState();
    for (const auto& [nBit, key] : PadButtons)
        io.AddKeyEvent(key, (pad.nButtons & nBit) != 0);

    const auto fX = PadThumbAxis(pad.sThumbLX);
    const auto fY = PadThumbAxis(pad.sThumbLY);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, fX < 0.0f, (std::max)(-fX, 0.0f));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, fX > 0.0f, (std::max)(fX, 0.0f));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, fY > 0.0f, (std::max)(fY, 0.0f));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, fY < 0.0f, (std::max)(-fY, 0.0f));
}

// The game reads the mouse through DirectInput, so the pointer is moved by its counts.
static void FeedMouse(ImGuiIO& io)
{
    const auto mouse = TakeBlockedMouse();
    if (bCentreMouse)
    {
        MousePos = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        bCentreMouse = false;
    }

    MousePos.x = std::clamp(MousePos.x + static_cast<float>(mouse.nX), 0.0f, io.DisplaySize.x - 1.0f);
    MousePos.y = std::clamp(MousePos.y + static_cast<float>(mouse.nY), 0.0f, io.DisplaySize.y - 1.0f);
    io.AddMousePosEvent(MousePos.x, MousePos.y);

    for (int nButton = 0; nButton < 3; nButton++)
        io.AddMouseButtonEvent(nButton, (mouse.nButtons & (1u << nButton)) != 0);

    // DirectInput reports 120 a notch.
    if (mouse.nWheel != 0)
        io.AddMouseWheelEvent(0.0f, static_cast<float>(mouse.nWheel) / 120.0f);
}

static void Section(const char* pTitle)
{
    const auto fEm = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, fEm * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, Heading);
    ImGui::TextUnformatted(pTitle);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, fEm * 0.2f));
}

// The value column's width.
static constexpr float fValueEms = 14.0f;

// A row's place and size: the label left, the value block (an arrow each side of the value) right.
struct RowBox
{
    float fRowW;
    float fRowH;
    float fValueW;
    float fBlockW;
    float fRowX;
    float fRowY;
};

static RowBox MeasureRow()
{
    const auto& style = ImGui::GetStyle();
    RowBox box{};
    box.fRowW = ImGui::GetContentRegionAvail().x;
    box.fRowH = ImGui::GetFrameHeight();
    box.fValueW = ImGui::GetFontSize() * fValueEms;
    box.fBlockW = box.fRowH * 2 + box.fValueW + style.ItemSpacing.x * 2;
    box.fRowX = ImGui::GetCursorPosX();
    box.fRowY = ImGui::GetCursorPosY();
    return box;
}

static int ArrowsWhenFocused()
{
    if (!ImGui::IsItemFocused())
        return 0;

    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Header));

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight))
        return 1;
    return 0;
}

// Label left, arrows and value right; the row is the only nav stop. Returns the step asked for.
static int OptionRow(const char* pLabel, const char* pValue)
{
    const auto& style = ImGui::GetStyle();
    const auto box = MeasureRow();
    ImGui::PushID(pLabel);

    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.fRowW, box.fRowH));
    auto nStep = ArrowsWhenFocused();

    ImGui::SetCursorPos(ImVec2(box.fRowX + style.ItemInnerSpacing.x, box.fRowY));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(pLabel);

    // Held arrows keep stepping.
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    const auto fBlockX = box.fRowX + box.fRowW - box.fBlockW;
    ImGui::SetCursorPos(ImVec2(fBlockX, box.fRowY));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left))
        nStep = -1;

    const auto fValueX = fBlockX + box.fRowH + style.ItemSpacing.x;
    const auto fPad = (box.fValueW - ImGui::CalcTextSize(pValue).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(fValueX + (std::max)(fPad, 0.0f), box.fRowY));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(pValue);

    ImGui::SetCursorPos(ImVec2(fValueX + box.fValueW + style.ItemSpacing.x, box.fRowY));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right))
        nStep = 1;
    ImGui::PopItemFlag();

    ImGui::PopID();
    ImGui::SetCursorPos(ImVec2(box.fRowX, box.fRowY));
    ImGui::Dummy(ImVec2(box.fRowW, box.fRowH));
    return nStep;
}

static void DrawRows()
{
    for (size_t i = 0; i < JackalFixPanelRowCount(); i++)
    {
        const auto row = JackalFixPanelRowAt(i);
        if (row.bHeading)
        {
            Section(row.label.c_str());
            continue;
        }

        ImGui::BeginDisabled(!row.bAvailable);
        const auto nStep = OptionRow(row.label.c_str(), row.value.c_str());
        ImGui::EndDisabled();

        if (nStep != 0)
            JackalFixPanelStep(i, nStep);
    }
}

static void DrawTitle()
{
    static constexpr auto szTitle = "JACKAL FIX";
    const auto fPad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(szTitle).x) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(fPad, 0.0f));
    ImGui::TextUnformatted(szTitle);
}

// Back and Defaults on the right.
static void DrawFooter(float fEm)
{
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, fEm * 0.2f));

    const auto fButtonW = fEm * 8;
    const auto fPad = ImGui::GetContentRegionAvail().x - fButtonW * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(fPad, 0.0f));

    if (ImGui::Button("Back", ImVec2(fButtonW, 0)))
        bOpen = false;

    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(fButtonW, 0)))
        JackalFixPanelDefaults();
}

static void DrawPanel()
{
    const auto* pViewport = ImGui::GetMainViewport();
    const auto fEm = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(pViewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fEm * 50, fEm * 32), ImGuiCond_Always);

    for (const auto& [col, color] : PanelColors)
        ImGui::PushStyleColor(col, color);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fEm * 1.6f, fEm * 1.2f));

    constexpr ImGuiWindowFlags nFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##jackalfix", nullptr, nFlags))
    {
        DrawTitle();
        const auto fFooter = ImGui::GetFrameHeightWithSpacing() + fEm;
        ImGui::BeginChild("##rows", ImVec2(0, -fFooter), ImGuiChildFlags_NavFlattened);
        DrawRows();
        ImGui::EndChild();
        DrawFooter(fEm);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(static_cast<int>(std::size(PanelColors)));
}

// Builds the frame's draw data; false when the panel is closed.
static bool BuildFrame(float fWidth, float fHeight)
{
    UpdateToggle();
    UpdateInputBlock();
    if (!bOpen)
        return false;

    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(fWidth, fHeight);
    io.DeltaTime = FrameSeconds();
    io.MouseDrawCursor = true;
    // Scale the font with the output height.
    io.FontGlobalScale = (std::max)(1.0f, fHeight / 720.0f);

    const auto bForeground = IsGameForeground();
    FeedKeyboard(io, bForeground);
    FeedPad(io);
    FeedMouse(io);

    ImGui::NewFrame();
    DrawPanel();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
        bOpen = false;

    ImGui::Render();
    return true;
}

static void RenderDX9(IDirect3DDevice9* pDevice)
{
    EnsureContext();
    if (!EnsureBackend(Backend::DX9, pDevice))
        return;

    IDirect3DSurface9* pBackBuffer = nullptr;
    if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
        return;

    D3DSURFACE_DESC desc{};
    pBackBuffer->GetDesc(&desc);

    ImGui_ImplDX9_NewFrame();
    if (BuildFrame(static_cast<float>(desc.Width), static_cast<float>(desc.Height)))
    {
        // A state block does not cover render targets, so those are saved by hand.
        IDirect3DSurface9* pOldTarget = nullptr;
        IDirect3DSurface9* pOldDepth = nullptr;
        pDevice->GetRenderTarget(0, &pOldTarget);
        pDevice->GetDepthStencilSurface(&pOldDepth);
        pDevice->SetRenderTarget(0, pBackBuffer);
        pDevice->SetDepthStencilSurface(nullptr);

        const auto bOpenedScene = SUCCEEDED(pDevice->BeginScene());
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        if (bOpenedScene)
            pDevice->EndScene();

        if (pOldTarget)
        {
            pDevice->SetRenderTarget(0, pOldTarget);
            pOldTarget->Release();
        }
        pDevice->SetDepthStencilSurface(pOldDepth);
        if (pOldDepth)
            pOldDepth->Release();
    }

    pBackBuffer->Release();
}

static void DrawToBackBuffer(ID3D10Device* pDevice, ID3D10Texture2D* pBackBuffer)
{
    ID3D10RenderTargetView* pView = nullptr;
    if (FAILED(pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pView)) || !pView)
        return;

    ID3D10RenderTargetView* pOldView = nullptr;
    ID3D10DepthStencilView* pOldDepth = nullptr;
    pDevice->OMGetRenderTargets(1, &pOldView, &pOldDepth);
    pDevice->OMSetRenderTargets(1, &pView, nullptr);

    ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());

    pDevice->OMSetRenderTargets(1, &pOldView, pOldDepth);
    if (pOldView)
        pOldView->Release();
    if (pOldDepth)
        pOldDepth->Release();
    pView->Release();
}

static void RenderDX10(ID3D10Device* pDevice, IDXGISwapChain* pSwapChain)
{
    EnsureContext();
    if (!pSwapChain || !EnsureBackend(Backend::DX10, pDevice))
        return;

    // Fetched each frame and released after: a held reference would fail the next ResizeBuffers.
    ID3D10Texture2D* pBackBuffer = nullptr;
    if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void**>(&pBackBuffer))))
        return;

    D3D10_TEXTURE2D_DESC desc{};
    pBackBuffer->GetDesc(&desc);

    ImGui_ImplDX10_NewFrame();
    if (BuildFrame(static_cast<float>(desc.Width), static_cast<float>(desc.Height)))
        DrawToBackBuffer(pDevice, pBackBuffer);

    pBackBuffer->Release();
}

class SettingsPanel
{
public:
    SettingsPanel()
    {
        JackalFix::onFrameEndDX9Event() += [](void* pDevice)
        {
            if (pDevice)
                RenderDX9(static_cast<IDirect3DDevice9*>(pDevice));
        };

        JackalFix::onFrameEndDX10Event() += [](void* pDevice, void* pSwapChain)
        {
            if (pDevice)
                RenderDX10(static_cast<ID3D10Device*>(pDevice), static_cast<IDXGISwapChain*>(pSwapChain));
        };

        // Its vertex and index buffers live in the default pool.
        JackalFix::onDeviceResetEvent() += []()
        {
            if (eBackend == Backend::DX9)
                ImGui_ImplDX9_InvalidateDeviceObjects();
        };
    }
} SettingsPanel;
