#include "game.h"
#include "injury_protection.h"
#include "weapon_hooks.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static HMODULE selfModule;
static std::atomic_bool running{true}, menu{true};
static HWND gameWindow{}, overlayWindow{};
static ID3D11Device* device{}; static ID3D11DeviceContext* context{}; static IDXGISwapChain1* swapChain{}; static ID3D11RenderTargetView* target{};
static IDCompositionDevice* compositionDevice{}; static IDCompositionTarget* compositionTarget{}; static IDCompositionVisual* compositionVisual{};
static GameReader reader; static Features features;
static InjuryProtection injuryProtection;
static WeaponHooks weaponHooks;
static bool captureKeyWasDown[256]{};
static bool captureTeleportKeyWasDown[256]{};
static bool teleportKeyWasDown{};

static void appendOverlayLog(const char* line, DWORD length);

static std::filesystem::path configPath() {
    if (!selfModule) return {};
    std::wstring modulePath(MAX_PATH, L'\0');
    for (;;) {
        const DWORD capacity = static_cast<DWORD>(modulePath.size());
        const DWORD length = GetModuleFileNameW(selfModule, modulePath.data(), capacity);
        if (!length) return {};
        if (length < capacity) {
            modulePath.resize(length);
            const std::filesystem::path dllPath(modulePath);
            if (!dllPath.is_absolute()) return {};
            return dllPath.parent_path() / L"PQOverlay.ini";
        }
        if (capacity >= 32768) return {};
        modulePath.resize(std::min<DWORD>(capacity * 2, 32768));
    }
}

static void saveConfig(const Features& f) {
    const auto path = configPath();
    if (path.empty()) return;
    std::ofstream file(path, std::ios::trunc);
    if (!file) return;
    file << "espBox=" << int(f.espBox) << '\n';
    file << "espSkeleton=" << int(f.espSkeleton) << '\n';
    for (int i = 0; i < 4; ++i) file << "skeletonColor" << i << '=' << f.skeletonColor[i] << '\n';
    file << "aimbot=" << int(f.aimbot) << '\n';
    file << "showAimFov=" << int(f.showAimFov) << '\n';
    file << "espDistance=" << f.espDistance << '\n';
    file << "aimBone=" << f.aimBone << '\n';
    file << "aimKey=" << f.aimKey << '\n';
    file << "aimDistance=" << f.aimDistance << '\n';
    file << "aimFov=" << f.aimFov << '\n';
    for (int i = 0; i < 4; ++i) file << "boxColor" << i << '=' << f.boxColor[i] << '\n';
    file << "espDistanceLabel=" << int(f.espDistanceLabel) << '\n';
    file << "espHealthBar=" << int(f.espHealthBar) << '\n';
    file << "espHealthNumber=" << int(f.espHealthNumber) << '\n';
    file << "espCorpse=" << int(f.espCorpse) << '\n';
    file << "espSafeBoxes=" << int(f.espSafeBoxes) << '\n';
    for (int i = 0; i < 4; ++i) file << "safeBoxColor" << i << '=' << f.safeBoxColor[i] << '\n';
    file << "safeBoxDistance=" << f.safeBoxDistance << '\n';
    file << "corpseDistance=" << f.corpseDistance << '\n';
    file << "infiniteHealth=" << int(f.infiniteHealth) << '\n';
    file << "infiniteBodyStamina=" << int(f.infiniteBodyStamina) << '\n';
    file << "infiniteArmStamina=" << int(f.infiniteArmStamina) << '\n';
    file << "preventBleeding=" << int(f.preventBleeding) << '\n';
    file << "preventFracture=" << int(f.preventFracture) << '\n';
    file << "keepInventoryOnDeath=" << int(f.keepInventoryOnDeath) << '\n';
    file << "playerSpeedMultiplier=" << f.playerSpeedMultiplier << '\n';
    file << "playerJumpHeightMultiplier=" << f.playerJumpHeightMultiplier << '\n';
    file << "teleportKey=" << f.teleportKey << '\n';
    file << "weaponInfiniteAmmo=" << int(f.weaponInfiniteAmmo) << '\n';
    file << "weaponInfiniteGrenades=" << int(f.weaponInfiniteGrenades) << '\n';
    file << "weaponInfiniteDurability=" << int(f.weaponInfiniteDurability) << '\n';
    file << "weaponNoRecoil=" << int(f.weaponNoRecoil) << '\n';
    file << "weaponNoSpread=" << int(f.weaponNoSpread) << '\n';
    file << "meleeAttackSpeedMultiplier=" << f.meleeAttackSpeedMultiplier << '\n';
    file << "weaponOneHitKill=" << int(f.weaponOneHitKill) << '\n';
    file << "instantSearch=" << int(f.instantSearch) << '\n';
    file << "addMoneyAmount=" << f.addMoneyAmount << '\n';
    file << "addCoinAmount=" << f.addCoinAmount << '\n';
    file << "addExperienceAmount=" << f.addExperienceAmount << '\n';
    file << "addMetalAmount=" << f.addMetalAmount << '\n';
    file << "addOilAmount=" << f.addOilAmount << '\n';
    file << "addConcreteAmount=" << f.addConcreteAmount << '\n';
    file << "addFoodAmount=" << f.addFoodAmount << '\n';
    file << "addUicsReputation=" << f.addUicsReputation << '\n';
    file << "addVlfReputation=" << f.addVlfReputation << '\n';
    file << "addIgcReputation=" << f.addIgcReputation << '\n';
    file << "gameSpeedMultiplier=" << f.gameSpeedMultiplier << '\n';
}

static bool checkboxConfig(const char* label, bool& value) {
    if (!ImGui::Checkbox(label, &value)) return false;
    saveConfig(features);
    return true;
}

static bool sliderConfig(const char* label, float& value, float minimum, float maximum,
    const char* format, ImGuiSliderFlags flags = 0) {
    if (!ImGui::SliderFloat(label, &value, minimum, maximum, format, flags)) return false;
    saveConfig(features);
    return true;
}

static bool colorConfig(const char* label, float (&value)[4]) {
    if (!ImGui::ColorEdit4(label, value,
        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) return false;
    saveConfig(features);
    return true;
}

static bool inventoryAmountButton(const char* inputId, const char* buttonLabel, int& amount) {
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputInt(inputId, &amount, 0, 0);
    const bool edited = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    const bool pressed = ImGui::Button(buttonLabel);
    if (edited || pressed) {
        amount = std::clamp(amount, 1, 100000000);
        saveConfig(features);
    }
    return pressed;
}

static bool inventoryAmountButton(const char* inputId, const char* buttonLabel, float& points) {
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputFloat(inputId, &points, 0.f, 0.f, "%.2f", ImGuiInputTextFlags_CharsDecimal);
    const bool edited = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::BeginDisabled(!std::isfinite(points) || points <= 0.f);
    const bool pressed = ImGui::Button(buttonLabel);
    ImGui::EndDisabled();
    if (edited || pressed) {
        points = std::isfinite(points) ? std::clamp(points, 0.f, 100.f) : 0.f;
        saveConfig(features);
    }
    return pressed;
}

static void loadConfig(Features& f) {
    const auto path = configPath();
    if (path.empty()) return;
    std::ifstream file(path);
    if (!file) return;
    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos)
            values[line.substr(0, separator)] = line.substr(separator + 1);
    }
    auto boolean = [&](const char* key, bool& out) {
        if (auto it = values.find(key); it != values.end()) out = std::strtol(it->second.c_str(), nullptr, 10) != 0;
    };
    auto integer = [&](const char* key, int& out) {
        if (auto it = values.find(key); it != values.end()) {
            char* end = nullptr; const long value = std::strtol(it->second.c_str(), &end, 10);
            if (end != it->second.c_str() && *end == '\0' && value >= 0 && value <= 255) out = int(value);
        }
    };
    auto amount = [&](const char* key, int& out) {
        if (auto it = values.find(key); it != values.end()) {
            char* end = nullptr; const long value = std::strtol(it->second.c_str(), &end, 10);
            if (end != it->second.c_str() && *end == '\0' && value >= 1 && value <= 100000000)
                out = int(value);
        }
    };
    auto real = [&](const std::string& key, float& out) {
        if (auto it = values.find(key); it != values.end()) {
            char* end = nullptr; const float value = std::strtof(it->second.c_str(), &end);
            if (end != it->second.c_str() && *end == '\0' && std::isfinite(value)) out = value;
        }
    };
    boolean("espBox", f.espBox); boolean("espSkeleton", f.espSkeleton);
    boolean("aimbot", f.aimbot); boolean("showAimFov", f.showAimFov);
    boolean("espDistanceLabel", f.espDistanceLabel); boolean("espHealthBar", f.espHealthBar);
    boolean("espHealthNumber", f.espHealthNumber); boolean("espCorpse", f.espCorpse);
    boolean("espSafeBoxes", f.espSafeBoxes);
    boolean("infiniteHealth", f.infiniteHealth); boolean("infiniteBodyStamina", f.infiniteBodyStamina);
    boolean("infiniteArmStamina", f.infiniteArmStamina); boolean("preventBleeding", f.preventBleeding);
    boolean("preventFracture", f.preventFracture); boolean("keepInventoryOnDeath", f.keepInventoryOnDeath);
    boolean("weaponInfiniteAmmo", f.weaponInfiniteAmmo);
    boolean("weaponInfiniteGrenades", f.weaponInfiniteGrenades);
    boolean("weaponInfiniteDurability", f.weaponInfiniteDurability);
    boolean("weaponNoRecoil", f.weaponNoRecoil); boolean("weaponNoSpread", f.weaponNoSpread);
    boolean("weaponOneHitKill", f.weaponOneHitKill);
    boolean("instantSearch", f.instantSearch);
    integer("aimBone", f.aimBone); integer("aimKey", f.aimKey); integer("teleportKey", f.teleportKey);
    real("espDistance", f.espDistance); real("aimDistance", f.aimDistance); real("aimFov", f.aimFov);
    real("corpseDistance", f.corpseDistance); real("playerSpeedMultiplier", f.playerSpeedMultiplier);
    real("playerJumpHeightMultiplier", f.playerJumpHeightMultiplier);
    real("meleeAttackSpeedMultiplier", f.meleeAttackSpeedMultiplier);
    real("gameSpeedMultiplier", f.gameSpeedMultiplier);
    amount("addMoneyAmount", f.addMoneyAmount);
    amount("addCoinAmount", f.addCoinAmount);
    amount("addExperienceAmount", f.addExperienceAmount);
    amount("addMetalAmount", f.addMetalAmount);
    amount("addOilAmount", f.addOilAmount);
    amount("addConcreteAmount", f.addConcreteAmount);
    amount("addFoodAmount", f.addFoodAmount);
    real("addUicsReputation", f.addUicsReputation);
    real("addVlfReputation", f.addVlfReputation);
    real("addIgcReputation", f.addIgcReputation);
    f.addUicsReputation = std::clamp(f.addUicsReputation, 0.f, 100.f);
    f.addVlfReputation = std::clamp(f.addVlfReputation, 0.f, 100.f);
    f.addIgcReputation = std::clamp(f.addIgcReputation, 0.f, 100.f);
    for (int i = 0; i < 4; ++i) {
        real("skeletonColor" + std::to_string(i), f.skeletonColor[i]);
        real("boxColor" + std::to_string(i), f.boxColor[i]);
        real("safeBoxColor" + std::to_string(i), f.safeBoxColor[i]);
    }
    f.espDistance = std::clamp(f.espDistance, 1.f, 1000.f);
    f.aimDistance = std::clamp(f.aimDistance, 1.f, 1000.f);
    f.aimFov = std::clamp(f.aimFov, 1.f, 30.f);
    f.corpseDistance = std::clamp(f.corpseDistance, 1.f, 1000.f);
    f.safeBoxDistance = std::clamp(f.safeBoxDistance, 1.f, 2000.f);
    f.playerSpeedMultiplier = std::clamp(f.playerSpeedMultiplier, 0.5f, 5.f);
    f.playerJumpHeightMultiplier = std::clamp(f.playerJumpHeightMultiplier, 0.5f, 5.f);
    f.meleeAttackSpeedMultiplier = std::clamp(f.meleeAttackSpeedMultiplier, 0.5f, 5.f);
    f.gameSpeedMultiplier = std::clamp(f.gameSpeedMultiplier, 0.1f, 5.f);
    for (float& value : f.skeletonColor) value = std::clamp(value, 0.f, 1.f);
    for (float& value : f.boxColor) value = std::clamp(value, 0.f, 1.f);
    const auto message = std::string("config loaded path=") + path.string() +
        " entries=" + std::to_string(values.size()) +
        " weaponOneHitKill=" + std::to_string(int(f.weaponOneHitKill)) +
        " meleeAttackSpeedMultiplier=" + std::to_string(f.meleeAttackSpeedMultiplier) + "\r\n";
    appendOverlayLog(message.c_str(), DWORD(message.size()));
}

static void appendOverlayLog(const char* line, DWORD length) {
    OutputDebugStringA(line);
    wchar_t path[MAX_PATH]{};
    const DWORD pathLength = GetTempPathW(MAX_PATH, path);
    if (pathLength && pathLength < MAX_PATH) {
        wcscat_s(path, L"PQOverlay.log");
        HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written{};
            WriteFile(file, line, length, &written, nullptr);
            CloseHandle(file);
        }
    }
}

static bool initFailure(const char* stage, HRESULT hr) {
    char line[256]{};
    const int length = std::snprintf(line, sizeof(line), "%s: 0x%08lX\r\n", stage, static_cast<unsigned long>(hr));
    if (length > 0) appendOverlayLog(line, static_cast<DWORD>(length));
    return false;
}

static void updateAimKeyCapture() {
    if (!features.captureAimKey) return;
    for (int key = 1; key < 256; ++key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !captureKeyWasDown[key]) {
            if (key == VK_ESCAPE) features.captureAimKey = false;
            else {
                features.aimKey = key;
                features.captureAimKey = false;
                saveConfig(features);
            }
            break;
        }
        captureKeyWasDown[key] = down;
    }
}

static void beginAimKeyCapture() {
    for (int key = 1; key < 256; ++key) captureKeyWasDown[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    features.captureAimKey = true;
}

static void updateTeleportKeyCapture() {
    if (!features.captureTeleportKey) return;
    for (int key = 1; key < 256; ++key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !captureTeleportKeyWasDown[key]) {
            if (key == VK_ESCAPE) features.captureTeleportKey = false;
            else {
                features.teleportKey = key;
                features.captureTeleportKey = false;
                teleportKeyWasDown = true;
                saveConfig(features);
            }
            break;
        }
        captureTeleportKeyWasDown[key] = down;
    }
}

static void beginTeleportKeyCapture() {
    for (int key = 1; key < 256; ++key)
        captureTeleportKeyWasDown[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    features.captureTeleportKey = true;
}

static const char* aimKeyLabel(int key) {
    switch (key) {
    case VK_LBUTTON: return "鼠标左键";
    case VK_RBUTTON: return "鼠标右键";
    case VK_MBUTTON: return "鼠标中键";
    case VK_XBUTTON1: return "鼠标侧键 1";
    case VK_XBUTTON2: return "鼠标侧键 2";
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_SPACE: return "空格";
    case VK_INSERT: return "Insert";
    case VK_CAPITAL: return "Caps Lock";
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
    case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12: {
        static char functionKey[8];
        std::snprintf(functionKey, sizeof(functionKey), "F%d", key - VK_F1 + 1);
        return functionKey;
    }
    default: {
        static char keyName[64];
        LONG scan = MapVirtualKeyA(static_cast<UINT>(key), MAPVK_VK_TO_VSC) << 16;
        if (GetKeyNameTextA(scan, keyName, static_cast<int>(sizeof(keyName)))) return keyName;
        return "未知按键";
    }
    }
}

static void releaseGraphics() {
    if (target) { target->Release(); target = nullptr; }
    if (swapChain) { swapChain->Release(); swapChain = nullptr; }
    if (compositionVisual) { compositionVisual->Release(); compositionVisual = nullptr; }
    if (compositionTarget) { compositionTarget->Release(); compositionTarget = nullptr; }
    if (compositionDevice) { compositionDevice->Release(); compositionDevice = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }
}

static BOOL CALLBACK findGameWindow(HWND h, LPARAM data) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
        *reinterpret_cast<HWND*>(data) = h; return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((menu || m == WM_KILLFOCUS) && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) { running = false; return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void setInputMode() {
    constexpr LONG passiveStyles = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    const bool menuOpen = menu.load();
    const LONG ex = GetWindowLongW(overlayWindow, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TOPMOST;
    SetWindowLongW(overlayWindow, GWL_EXSTYLE,
        menuOpen ? ex & ~passiveStyles : ex | passiveStyles);
    SetWindowPos(overlayWindow, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!menuOpen && GetForegroundWindow() == overlayWindow) SetForegroundWindow(gameWindow);
}
static bool createRenderTarget() {
    ID3D11Texture2D* back{};
    HRESULT hr = swapChain ? swapChain->GetBuffer(0, IID_PPV_ARGS(&back)) : E_POINTER;
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(back, nullptr, &target);
    if (back) back->Release();
    if (FAILED(hr)) return initFailure("CreateRenderTargetView", hr);
    return true;
}
static bool initD3D() {
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) return initFailure("D3D11CreateDevice", hr);
    IDXGIDevice* dxgiDevice{}; IDXGIAdapter* adapter{}; IDXGIFactory2* factory{};
    const char* failedStage = nullptr;
    hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) failedStage = "QueryInterface(IDXGIDevice)";
    if (SUCCEEDED(hr)) {
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) failedStage = "IDXGIDevice::GetAdapter";
    }
    if (SUCCEEDED(hr)) {
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) failedStage = "IDXGIAdapter::GetParent";
    }
    RECT clientRect{};
    GetClientRect(gameWindow, &clientRect);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = clientRect.right > 0 ? static_cast<UINT>(clientRect.right) : 1;
    sd.Height = clientRect.bottom > 0 ? static_cast<UINT>(clientRect.bottom) : 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferCount = 2; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (SUCCEEDED(hr)) { hr = factory->CreateSwapChainForComposition(device, &sd, nullptr, &swapChain); if (FAILED(hr)) failedStage = "CreateSwapChainForComposition"; }
    if (SUCCEEDED(hr)) { hr = DCompositionCreateDevice(dxgiDevice, IID_PPV_ARGS(&compositionDevice)); if (FAILED(hr)) failedStage = "DCompositionCreateDevice"; }
    if (SUCCEEDED(hr)) { hr = compositionDevice->CreateTargetForHwnd(overlayWindow, TRUE, &compositionTarget); if (FAILED(hr)) failedStage = "CreateTargetForHwnd"; }
    if (SUCCEEDED(hr)) { hr = compositionDevice->CreateVisual(&compositionVisual); if (FAILED(hr)) failedStage = "CreateVisual"; }
    if (SUCCEEDED(hr)) { hr = compositionVisual->SetContent(swapChain); if (FAILED(hr)) failedStage = "SetContent"; }
    if (SUCCEEDED(hr)) { hr = compositionTarget->SetRoot(compositionVisual); if (FAILED(hr)) failedStage = "SetRoot"; }
    if (SUCCEEDED(hr)) { hr = compositionDevice->Commit(); if (FAILED(hr)) failedStage = "Commit"; }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (FAILED(hr)) return initFailure(failedStage ? failedStage : "DirectComposition", hr);
    return true;
}
struct DrawCounts { int boxes{}, skeletonSegments{}, headRings{}, healthBars{}, healthNumbers{}, distanceLabels{}, corpseLabels{}, safeBoxes{}; };

static int drawBoneSegment(ImDrawList* draw, ImVec2 from, ImVec2 to,
                           ImVec2 head, float headRadius, ImU32 color, float lineWidth) {
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared == 0.f) return 0;
    if (headRadius > 0.f) {
        const float ox = from.x - head.x, oy = from.y - head.y;
        const float dot = ox * dx + oy * dy;
        const float discriminant = dot * dot - lengthSquared * (ox * ox + oy * oy - headRadius * headRadius);
        if (discriminant > 0.f) {
            const float root = std::sqrt(discriminant);
            const float enter = (-dot - root) / lengthSquared;
            const float exit = (-dot + root) / lengthSquared;
            if (enter < 1.f && exit > 0.f) {
                int pieces = 0;
                if (enter > 0.f) {
                    draw->AddLine(from, ImVec2(from.x + enter * dx, from.y + enter * dy), color, lineWidth);
                    ++pieces;
                }
                if (exit < 1.f) {
                    draw->AddLine(ImVec2(from.x + exit * dx, from.y + exit * dy), to, color, lineWidth);
                    ++pieces;
                }
                return pieces;
            }
        }
    }
    draw->AddLine(from, to, color, lineWidth);
    return 1;
}

static DrawCounts drawEsp(ImDrawList* draw, int w, int h) {
    DrawCounts counts;
    if (features.showAimFov && w > 0 && h > 0) {
        const float radius = reader.aimRadiusPixels(w, features);
        draw->AddCircle(ImVec2(w * .5f, h * .5f), radius, IM_COL32(255, 220, 80, 180), 96, 1.25f);
    }
    if (!features.espBox && !features.espSkeleton && !features.espHealthBar &&
        !features.espHealthNumber && !features.espDistanceLabel && !features.espCorpse &&
        !features.espSafeBoxes) return counts;
    if (!reader.camera().valid || w <= 0 || h <= 0) return counts;
    const float focal = w * .5f / std::tan(reader.camera().fov * 3.14159265358979323846f / 360.f);
    if (features.espSafeBoxes) {
        auto* font = ImGui::GetFont();
        for (const auto& safe : reader.safeBoxes()) {
            if (!safe.visible || safe.distance > features.safeBoxDistance) continue;
            const float pixelsPerMeter = focal / (std::max)(safe.distance, 0.5f);
            const float scale = std::clamp(pixelsPerMeter / 50.f, .35f, 2.f);
            const float size = std::clamp(16.f * scale, 14.f, 24.f);
            char label[64]{};
            std::snprintf(label, sizeof(label), "%s保险箱 %.0f m", safe.large ? "大" : "小", safe.distance);
            const ImVec2 textSize = font->CalcTextSizeA(size, 1e6f, 0.f, label);
            const ImVec2 position(float(safe.screen.x) - textSize.x * .5f,
                float(safe.screen.y) - textSize.y * .5f);
            draw->AddText(font, size, ImVec2(position.x + scale, position.y + scale),
                IM_COL32(0, 0, 0, 220), label);
            const ImU32 safeColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                features.safeBoxColor[0], features.safeBoxColor[1],
                features.safeBoxColor[2], features.safeBoxColor[3]));
            draw->AddText(font, size, position, safeColor, label);
            ++counts.safeBoxes;
        }
    }
    for (const auto& t : reader.targets()) {
        const float pixelsPerMeter = focal / (std::max)(t.distance, 0.5f);
        const float scale = std::clamp(pixelsPerMeter / 50.f, .35f, 2.f);
        const float boxThickness = std::clamp(1.5f * scale, .75f, 3.f);
        const float labelSize = std::clamp(16.f * scale, t.dead ? 11.f : 8.f, 32.f);
        const float labelGap = 2.f * scale;
        auto* font = ImGui::GetFont();
        const auto addLabel = [&](const char* text, ImVec2 position) {
            draw->AddText(font, labelSize, ImVec2(position.x + scale, position.y + scale),
                IM_COL32(0, 0, 0, 220), text);
            draw->AddText(font, labelSize, position, IM_COL32(240, 240, 240, 240), text);
        };
        if (t.dead) {
            if (features.espCorpse && t.distance <= features.corpseDistance && t.visible) {
                char label[48]{};
                std::snprintf(label, sizeof(label), "尸体 %.0f m", t.distance);
                const float textWidth = font->CalcTextSizeA(labelSize, 1e6f, 0.f, label).x;
                addLabel(label, ImVec2(float(t.rootScreen.x) - textWidth * .5f, float(t.rootScreen.y)));
                ++counts.corpseLabels;
            }
            continue;
        }
        if (t.distance > features.espDistance) continue;
        if (t.boxValid) {
            const float left = float(t.boxMin.x), right = float(t.boxMax.x);
            const float top = float(t.boxMin.y), bottom = float(t.boxMax.y);
            if (features.espBox) {
                const ImU32 boxColor = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    features.boxColor[0], features.boxColor[1], features.boxColor[2], features.boxColor[3]));
                draw->AddRect(ImVec2(left, top), ImVec2(right, bottom), boxColor, 0, 0, boxThickness);
                ++counts.boxes;
            }
            if (features.espHealthBar && t.healthValid && bottom - top >= 2.f) {
                const float barWidth = std::clamp(5.f * scale, 2.f, 10.f);
                const float border = std::clamp(scale, .5f, 1.5f);
                const float barRight = left - boxThickness * .5f - .5f * scale;
                const float barLeft = barRight - barWidth;
                const float ratio = std::clamp(t.health / t.maxHealth, 0.f, 1.f);
                draw->AddRectFilled(ImVec2(barLeft, top), ImVec2(barRight, bottom), IM_COL32(12, 16, 20, 210));
                const float innerTop = top + border, innerBottom = bottom - border;
                if (ratio > 0.f && innerBottom > innerTop) {
                    const float filledTop = innerBottom - (innerBottom - innerTop) * ratio;
                    draw->AddRectFilled(ImVec2(barLeft + border, filledTop),
                        ImVec2(barRight - border, innerBottom), IM_COL32(0, 190, 255, 240));
                }
                draw->AddRect(ImVec2(barLeft, top), ImVec2(barRight, bottom),
                    IM_COL32(0, 190, 255, 240), 0, 0, border);
                ++counts.healthBars;
            }
            if (features.espHealthNumber && t.healthValid) {
                char healthLabel[48]{};
                std::snprintf(healthLabel, sizeof(healthLabel), "%.0f", t.health);
                addLabel(healthLabel, ImVec2(right + boxThickness * .5f + labelGap, top));
                ++counts.healthNumbers;
            }
            if (features.espDistanceLabel) {
                char distanceLabel[48]{};
                std::snprintf(distanceLabel, sizeof(distanceLabel), "%.0f m", t.distance);
                const ImVec2 textSize = font->CalcTextSizeA(labelSize, 1e6f, 0.f, distanceLabel);
                addLabel(distanceLabel, ImVec2((left + right - textSize.x) * .5f,
                    bottom + boxThickness * .5f + labelGap));
                ++counts.distanceLabels;
            }
        }
        if (features.espSkeleton) {
            const float lineWidth = std::clamp(pixelsPerMeter * .016f, 1.f, 4.f);
            const float headRadius = t.aimValid[9] ? std::clamp(pixelsPerMeter * .10f, 2.f, 80.f) : 0.f;
            const ImVec2 head(float(t.headScreen.x), float(t.headScreen.y));
            const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(
                features.skeletonColor[0], features.skeletonColor[1],
                features.skeletonColor[2], features.skeletonColor[3]));
            for (const auto& line : t.skeletonLines) {
                counts.skeletonSegments += drawBoneSegment(draw,
                    ImVec2(float(line.first.x), float(line.first.y)),
                    ImVec2(float(line.second.x), float(line.second.y)), head, headRadius, color, lineWidth);
            }
            if (headRadius > 0.f) {
                draw->AddCircle(head, headRadius, color, 0, lineWidth);
                ++counts.headRings;
            }
        }
    }
    return counts;
}
static void render() {
    int w=0,h=0; RECT gr{}; GetClientRect(gameWindow,&gr); w=gr.right; h=gr.bottom; POINT p{0,0}; ClientToScreen(gameWindow,&p); SetWindowPos(overlayWindow,HWND_TOPMOST,p.x,p.y,w,h,SWP_NOACTIVATE);
    reader.update(w,h,features);
    injuryProtection.update(reader.localAbilityComponent(), features.preventBleeding, features.preventFracture);
    updateTeleportKeyCapture();
    const bool menuOpen=menu.load();
    static bool lastMenuOpen=false;
    if (menuOpen && !lastMenuOpen) weaponHooks.refreshWorldTime();
    lastMenuOpen=menuOpen;
    weaponHooks.update(features);
    if (!features.captureTeleportKey) {
        const bool down = (GetAsyncKeyState(features.teleportKey) & 0x8000) != 0;
        if (down && !teleportKeyWasDown) reader.teleportToMarker();
        teleportKeyWasDown = down;
    }
    reader.aim(features, gameWindow, menu);
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    const auto drawCounts = drawEsp(ImGui::GetBackgroundDrawList(),w,h);
    static bool timeDragging=false;
    bool worldTabVisible=false;
    if (menuOpen) {
        ImGui::SetNextWindowSize(ImVec2(440,560),ImGuiCond_FirstUseEver); ImGui::Begin("PROJECT QUARANTINE",nullptr,ImGuiWindowFlags_NoCollapse);
        ImGui::Text("状态：%s", reader.status()); ImGui::Separator();
        if (ImGui::BeginTabBar("FeatureTabs")) {
            if (ImGui::BeginTabItem("透视")) {
                checkboxConfig("方框透视", features.espBox);
                colorConfig("方框颜色", features.boxColor);
                checkboxConfig("骨骼透视", features.espSkeleton);
                colorConfig("骨骼颜色", features.skeletonColor);
                checkboxConfig("左侧血条", features.espHealthBar);
                checkboxConfig("血量数值", features.espHealthNumber);
                checkboxConfig("距离显示", features.espDistanceLabel);
                sliderConfig("透视距离", features.espDistance, 1.f, 1000.f, "%.0f m");
                checkboxConfig("尸体显示", features.espCorpse);
                sliderConfig("尸体显示距离", features.corpseDistance, 1.f, 1000.f, "%.0f m");
                ImGui::Separator();
                checkboxConfig("保险箱透视", features.espSafeBoxes);
                colorConfig("保险箱颜色", features.safeBoxColor);
                sliderConfig("保险箱显示距离", features.safeBoxDistance, 1.f, 2000.f, "%.0f m");
                ImGui::Text("目标数量：%d", static_cast<int>(reader.targets().size()));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("自瞄")) {
                checkboxConfig("启用自瞄", features.aimbot);
                checkboxConfig("显示自瞄范围", features.showAimFov);
                ImGui::SameLine();
                if (ImGui::Button(features.captureAimKey ? "按任意键..." : aimKeyLabel(features.aimKey))) beginAimKeyCapture();
                const char* boneLabels[] = {"头部", "颈部", "脊柱", "骨盆"};
                constexpr int boneIds[] = {9, 8, 4, 1};
                int selectedBone = 0;
                for (int i = 0; i < IM_ARRAYSIZE(boneIds); ++i)
                    if (features.aimBone == boneIds[i]) selectedBone = i;
                if (ImGui::Combo("自瞄部位", &selectedBone, boneLabels, IM_ARRAYSIZE(boneLabels))) {
                    features.aimBone = boneIds[selectedBone];
                    saveConfig(features);
                }
                sliderConfig("自瞄距离", features.aimDistance, 1.f, 1000.f, "%.0f m");
                sliderConfig("自瞄范围", features.aimFov, 1.f, 30.f, "%.1f deg");
                ImGui::Text("按住热键：持续锁定当前目标的指定部位");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("玩家")) {
                checkboxConfig("无限血量", features.infiniteHealth);
                checkboxConfig("无限身体耐力", features.infiniteBodyStamina);
                checkboxConfig("无限手臂耐力", features.infiniteArmStamina);
                checkboxConfig("不会流血", features.preventBleeding);
                checkboxConfig("不会骨折", features.preventFracture);
                checkboxConfig("死亡不掉落物品", features.keepInventoryOnDeath);
                sliderConfig("玩家移速", features.playerSpeedMultiplier, 0.5f, 5.f, "%.2f×",
                    ImGuiSliderFlags_AlwaysClamp);
                sliderConfig("跳跃高度", features.playerJumpHeightMultiplier, 0.5f, 5.f, "%.2f×",
                    ImGuiSliderFlags_AlwaysClamp);
                if (ImGui::Button("传送到标记点")) reader.teleportToMarker();
                ImGui::SameLine();
                if (ImGui::Button(features.captureTeleportKey ? "按任意键..." :
                    aimKeyLabel(features.teleportKey))) beginTeleportKeyCapture();
                if (const char* status = injuryProtection.status()) ImGui::TextUnformatted(status);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("武器")) {
                checkboxConfig("无限弹药", features.weaponInfiniteAmmo);
                checkboxConfig("无限手雷", features.weaponInfiniteGrenades);
                checkboxConfig("无限装备耐久", features.weaponInfiniteDurability);
                checkboxConfig("无后坐力", features.weaponNoRecoil);
                checkboxConfig("无扩散", features.weaponNoSpread);
                sliderConfig("近战攻速", features.meleeAttackSpeedMultiplier, 0.5f, 5.f, "%.2f×",
                    ImGuiSliderFlags_AlwaysClamp);
                checkboxConfig("一击必杀", features.weaponOneHitKill);
                ImGui::Separator();
                if (ImGui::Button("杀死全部敌人")) weaponHooks.requestKillAll();
                const int killed = weaponHooks.lastKillCount();
                if (killed == -2) ImGui::TextUnformatted("等待游戏线程执行……");
                else if (killed == -3) ImGui::TextUnformatted("当前场景的 AI 管理器未就绪");
                else if (killed >= 0) ImGui::Text("本次处理：%d 名敌人", killed);
                const auto weaponStatus = weaponHooks.status();
                if (!weaponStatus.empty()) ImGui::TextUnformatted(weaponStatus.c_str());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("库存")) {
                checkboxConfig("即时搜索", features.instantSearch);
                if (inventoryAmountButton("##MoneyAmount", "添加金钱", features.addMoneyAmount))
                    weaponHooks.requestAddMoney(features.addMoneyAmount);
                if (inventoryAmountButton("##CoinAmount", "添加标记硬币", features.addCoinAmount))
                    weaponHooks.requestAddCoins(features.addCoinAmount);
                if (inventoryAmountButton("##ExperienceAmount", "添加经验", features.addExperienceAmount))
                    weaponHooks.requestAddExperience(features.addExperienceAmount);
                if (inventoryAmountButton("##MetalAmount", "添加金属", features.addMetalAmount))
                    weaponHooks.requestAddResource(HideoutResource::Metal, features.addMetalAmount);
                if (inventoryAmountButton("##OilAmount", "添加石油", features.addOilAmount))
                    weaponHooks.requestAddResource(HideoutResource::Oil, features.addOilAmount);
                if (inventoryAmountButton("##ConcreteAmount", "添加混凝土", features.addConcreteAmount))
                    weaponHooks.requestAddResource(HideoutResource::Concrete, features.addConcreteAmount);
                if (inventoryAmountButton("##FoodAmount", "添加食物", features.addFoodAmount))
                    weaponHooks.requestAddResource(HideoutResource::Food, features.addFoodAmount);
                if (inventoryAmountButton("##UicsReputation", "添加 UICS 声望", features.addUicsReputation))
                    weaponHooks.requestAddReputation(Faction::Uics, features.addUicsReputation);
                if (inventoryAmountButton("##VlfReputation", "添加 VLF 声望", features.addVlfReputation))
                    weaponHooks.requestAddReputation(Faction::Vlf, features.addVlfReputation);
                if (inventoryAmountButton("##IgcReputation", "添加 IGC 声望", features.addIgcReputation))
                    weaponHooks.requestAddReputation(Faction::Igc, features.addIgcReputation);
                if (ImGui::Button("免费升级当前据点设施")) weaponHooks.requestFreeHideoutUpgrade();
                const auto inventoryStatus = weaponHooks.status();
                if (!inventoryStatus.empty()) ImGui::TextUnformatted(inventoryStatus.c_str());
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("世界")) {
                worldTabVisible=true;
                sliderConfig("游戏速度", features.gameSpeedMultiplier, 0.1f, 5.f, "%.2f×",
                    ImGuiSliderFlags_AlwaysClamp);
                const int minute=weaponHooks.currentWorldMinute();
                static int selectedHour{};
                if (!timeDragging && minute>=0) selectedHour=minute/60;
                char display[8]="--:--";
                if (minute>=0)
                    std::snprintf(display,sizeof(display),"%02d:%02d",selectedHour,timeDragging ? 0 : minute%60);
                ImGui::BeginDisabled(minute<0);
                ImGui::SliderInt("时间",&selectedHour,0,23,display,ImGuiSliderFlags_NoInput);
                if (minute>=0 && ImGui::IsItemDeactivatedAfterEdit())
                    weaponHooks.requestSetTimeOfDay(selectedHour);
                timeDragging=minute>=0 && ImGui::IsItemActive();
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator(); ImGui::Text("INS：显示/隐藏菜单  |  END：卸载"); ImGui::End();
    }
    if (!worldTabVisible) timeDragging=false;
    updateAimKeyCapture();
    ImGui::Render();
    const float clear[4]{0,0,0,0};
    context->OMSetRenderTargets(1, &target, nullptr);
    context->ClearRenderTargetView(target, clear);
    auto* drawData = ImGui::GetDrawData();
    ImGui_ImplDX11_RenderDrawData(drawData);
    const auto presentResult = swapChain->Present(1, 0);
    static bool reportedDraw = false;
    static bool reportedCorpse = false;
    const bool hasLivingDraw = drawCounts.skeletonSegments && drawCounts.headRings;
    if (drawData->TotalIdxCount > 0 && ((!reportedDraw && hasLivingDraw) ||
        (!reportedCorpse && drawCounts.corpseLabels))) {
        char line[384]{};
        const int length = std::snprintf(line, sizeof(line),
            "pid=%lu size=%dx%d boxes=%d skeletonSegments=%d headRings=%d healthBars=%d healthNumbers=%d distanceLabels=%d corpseLabels=%d safeBoxes=%d indices=%d present=0x%08lX\r\n",
            static_cast<unsigned long>(GetCurrentProcessId()), w, h, drawCounts.boxes,
            drawCounts.skeletonSegments, drawCounts.headRings, drawCounts.healthBars,
            drawCounts.healthNumbers, drawCounts.distanceLabels, drawCounts.corpseLabels, drawCounts.safeBoxes,
            drawData->TotalIdxCount, static_cast<unsigned long>(presentResult));
        if (length > 0) appendOverlayLog(line, static_cast<DWORD>(length));
        reportedDraw |= hasLivingDraw;
        reportedCorpse |= drawCounts.corpseLabels > 0;
    }
}
static DWORD WINAPI MainThread(void*) {
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&gameWindow));
    if (!gameWindow) {
        initFailure("FindGameWindow", HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
        FreeLibraryAndExitThread(selfModule, 1);
    }
    WNDCLASSEXW wc{sizeof(wc),CS_CLASSDC,WndProc,0,0,selfModule,nullptr,nullptr,nullptr,nullptr,L"PQOverlayWindow",nullptr};
    if (!RegisterClassExW(&wc)) { initFailure("RegisterClassExW", HRESULT_FROM_WIN32(GetLastError())); FreeLibraryAndExitThread(selfModule, 1); }
    RECT r{}; GetClientRect(gameWindow,&r); POINT p{0,0}; ClientToScreen(gameWindow,&p);
    overlayWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE,wc.lpszClassName,L"",WS_POPUP,p.x,p.y,r.right,r.bottom,nullptr,nullptr,selfModule,nullptr);
    if (!overlayWindow) { initFailure("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError())); UnregisterClassW(wc.lpszClassName,selfModule); FreeLibraryAndExitThread(selfModule, 1); }
    ShowWindow(overlayWindow, SW_SHOWNOACTIVATE); setInputMode();
    if (!initD3D() || !createRenderTarget()) {
        releaseGraphics(); DestroyWindow(overlayWindow); UnregisterClassW(wc.lpszClassName,selfModule); FreeLibraryAndExitThread(selfModule,1);
    }
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc",18.f,nullptr,io.Fonts->GetGlyphRangesChineseFull()); ImGui::StyleColorsDark(); ImGui_ImplWin32_Init(overlayWindow); ImGui_ImplDX11_Init(device,context); loadConfig(features); reader.initialize(); weaponHooks.update(features); weaponHooks.initialize(reader.moduleBase());
    bool lastIns=false,lastEnd=false; while(running) { bool ins=(GetAsyncKeyState(VK_INSERT)&1)!=0; bool end=(GetAsyncKeyState(VK_END)&1)!=0; if(ins&&!lastIns){menu=!menu;setInputMode();} if(end&&!lastEnd)running=false; lastIns=ins;lastEnd=end; MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);} render(); Sleep(1); }
    if (menu.exchange(false) && IsWindow(overlayWindow)) setInputMode();
    weaponHooks.shutdown();
    injuryProtection.shutdown();
    reader.shutdown(); ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); releaseGraphics(); DestroyWindow(overlayWindow); UnregisterClassW(wc.lpszClassName,selfModule); FreeLibraryAndExitThread(selfModule,0); return 0;
}
BOOL APIENTRY DllMain(HMODULE h,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){selfModule=h;DisableThreadLibraryCalls(h);CreateThread(nullptr,0,MainThread,nullptr,0,nullptr);}return TRUE;}
