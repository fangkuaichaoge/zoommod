// ===================== System Header Files =====================
#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <fstream>
#include <algorithm>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "ZoomMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ===================== Transition Animation =====================
class Transition {
private:
    uint64_t m_startValue = 0;
    uint64_t m_endValue = 0;
    int m_duration = 0;
    int m_currentTime = 0;
    bool m_inProgress = false;

public:
    void startTransition(uint64_t from, uint64_t to, int duration) {
        m_startValue = from;
        m_endValue = to;
        m_duration = duration;
        m_currentTime = 0;
        m_inProgress = true;
    }

    void tick() {
        if (!m_inProgress) return;
        m_currentTime++;
        if (m_currentTime >= m_duration) {
            m_inProgress = false;
        }
    }

    uint64_t getCurrent() {
        if (!m_inProgress) return m_endValue;
        float progress = (float)m_currentTime / (float)m_duration;
        // Ease-out cubic
        float eased = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
        return m_startValue + (uint64_t)((float)(m_endValue - m_startValue) * eased);
    }

    bool inProgress() const {
        return m_inProgress;
    }
};

// ===================== Zoom State =====================
struct ZoomState {
    bool enabled = true;
    bool animated = false;
    bool zooming = false;
    uint64_t zoomLevel = 5345000000ULL;
    uint64_t lastClientZoom = 0;
    uint64_t minZoom = 5310000000ULL;
    uint64_t maxZoom = 5360000000ULL;
};
static ZoomState g_zoomState;
static Transition g_transition;
static std::mutex g_zoomMutex;

// ===================== Hook Function Pointers =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static uint64_t (*g_CameraAPI_tryGetFOV_orig)(void*) = nullptr;

// ===================== Utility Functions =====================
static uint64_t unsignedDiff(uint64_t a, uint64_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static int clamp(int minVal, int v, int maxVal) {
    if (v < minVal) return minVal;
    if (v > maxVal) return maxVal;
    return v;
}

// ===================== CameraAPI Hook =====================
static uint64_t CameraAPI_tryGetFOV_hook(void* thisPtr) {
    if (!g_CameraAPI_tryGetFOV_orig) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_zoomMutex);

    g_zoomState.lastClientZoom = g_CameraAPI_tryGetFOV_orig(thisPtr);

    if (!g_zoomState.enabled) {
        return g_zoomState.lastClientZoom;
    }

    if (!g_zoomState.animated) {
        return g_zoomState.zooming ? g_zoomState.zoomLevel : g_zoomState.lastClientZoom;
    }

    if (g_transition.inProgress() || g_zoomState.zooming) {
        g_transition.tick();
        uint64_t current = g_transition.getCurrent();
        if (current == 0) {
            return g_zoomState.lastClientZoom;
        }
        return current;
    }

    return g_zoomState.lastClientZoom;
}

// ===================== Find and Hook CameraAPI =====================
static bool findAndHookCameraAPI() {
    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib) {
        mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    }
    if (!mcLib) {
        LOGE("Failed to open libminecraftpe.so");
        return false;
    }
    
    uintptr_t libBase = 0;
    
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (libBase == 0) libBase = start;
            }
        }
    }
    
    if (libBase == 0) {
        LOGE("Failed to find libminecraftpe.so base address");
        return false;
    }
    
    LOGI("libminecraftpe.so base: 0x%lx", libBase);
    
    const char* typeinfoName = "9CameraAPI";
    size_t nameLen = strlen(typeinfoName);
    
    uintptr_t typeinfoNameAddr = 0;
    
    std::ifstream maps2("/proc/self/maps");
    while (std::getline(maps2, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos && line.find("r-xp") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - nameLen; addr++) {
            if (memcmp((void*)addr, typeinfoName, nameLen) == 0) {
                typeinfoNameAddr = addr;
                LOGI("Found typeinfo name at 0x%lx", typeinfoNameAddr);
                break;
            }
        }
        if (typeinfoNameAddr != 0) break;
    }
    
    if (typeinfoNameAddr == 0) {
        LOGE("Failed to find CameraAPI typeinfo name");
        return false;
    }
    
    uintptr_t typeinfoAddr = 0;
    
    std::ifstream maps3("/proc/self/maps");
    while (std::getline(maps3, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoNameAddr) {
                typeinfoAddr = addr - sizeof(void*);
                LOGI("Found typeinfo at 0x%lx", typeinfoAddr);
                break;
            }
        }
        if (typeinfoAddr != 0) break;
    }
    
    if (typeinfoAddr == 0) {
        LOGE("Failed to find CameraAPI typeinfo");
        return false;
    }
    
    uintptr_t vtableAddr = 0;
    
    std::ifstream maps4("/proc/self/maps");
    while (std::getline(maps4, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoAddr) {
                vtableAddr = addr + sizeof(void*);
                LOGI("Found vtable at 0x%lx", vtableAddr);
                break;
            }
        }
        if (vtableAddr != 0) break;
    }
    
    if (vtableAddr == 0) {
        LOGE("Failed to find CameraAPI vtable");
        return false;
    }
    
    uint64_t* tryGetFOVSlot = (uint64_t*)(vtableAddr + 7 * sizeof(void*));
    g_CameraAPI_tryGetFOV_orig = (uint64_t(*)(void*))(*tryGetFOVSlot);
    
    LOGI("Original tryGetFOV at 0x%lx", (uintptr_t)g_CameraAPI_tryGetFOV_orig);
    
    uintptr_t pageStart = (uintptr_t)tryGetFOVSlot & ~(4095UL);
    if (mprotect((void*)pageStart, 4096, PROT_READ | PROT_WRITE) != 0) {
        LOGE("Failed to make vtable writable");
        return false;
    }
    
    *tryGetFOVSlot = (uint64_t)CameraAPI_tryGetFOV_hook;
    
    mprotect((void*)pageStart, 4096, PROT_READ);
    
    LOGI("Successfully hooked CameraAPI::tryGetFOV");
    return true;
}

// ===================== ImGui Render Global State =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;
static ImFont* g_UIFont = nullptr;

// ===================== Theme Style =====================
static float g_FontScale = 1.0f;

static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;

    // 淡紫色主题
    const ImVec4 purplePrimary(0.6f, 0.4f, 0.85f, 1.0f);
    const ImVec4 purpleDark(0.45f, 0.3f, 0.7f, 1.0f);
    const ImVec4 purpleLight(0.75f, 0.55f, 0.95f, 1.0f);
    const ImVec4 purpleMuted(0.55f, 0.45f, 0.75f, 1.0f);
    const ImVec4 bgBase(0.94f, 0.90f, 0.98f, 0.96f);
    const ImVec4 bgSecondary(0.97f, 0.94f, 1.0f, 1.0f);
    const ImVec4 textMain(0.28f, 0.22f, 0.38f, 1.0f);
    const ImVec4 textMuted(0.55f, 0.48f, 0.62f, 1.0f);

    // 基础颜色
    c[ImGuiCol_WindowBg] = bgBase;
    c[ImGuiCol_ChildBg] = bgSecondary;
    c[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.93f, 0.99f, 0.98f);
    c[ImGuiCol_FrameBg] = bgSecondary;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.99f, 0.96f, 1.0f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 0.98f, 1.0f, 1.0f);
    c[ImGuiCol_TitleBg] = bgSecondary;
    c[ImGuiCol_TitleBgActive] = purpleLight;
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.95f, 0.91f, 0.99f, 1.0f);
    c[ImGuiCol_MenuBarBg] = bgSecondary;

    // 滚动条
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.92f, 0.88f, 0.96f, 0.5f);
    c[ImGuiCol_ScrollbarGrab] = purpleMuted;
    c[ImGuiCol_ScrollbarGrabHovered] = purplePrimary;
    c[ImGuiCol_ScrollbarGrabActive] = purpleDark;

    // 控件
    c[ImGuiCol_CheckMark] = purplePrimary;
    c[ImGuiCol_SliderGrab] = purplePrimary;
    c[ImGuiCol_SliderGrabActive] = purpleLight;
    c[ImGuiCol_Button] = purplePrimary;
    c[ImGuiCol_ButtonHovered] = purpleLight;
    c[ImGuiCol_ButtonActive] = purpleDark;

    // Header
    c[ImGuiCol_Header] = ImVec4(0.96f, 0.92f, 1.0f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.98f, 0.95f, 1.0f, 0.8f);
    c[ImGuiCol_HeaderActive] = purplePrimary;

    // 拖拽 grip
    c[ImGuiCol_ResizeGrip] = ImVec4(0.6f, 0.5f, 0.7f, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = purplePrimary;
    c[ImGuiCol_ResizeGripActive] = purpleDark;

    // Tab
    c[ImGuiCol_Tab] = bgSecondary;
    c[ImGuiCol_TabHovered] = purpleLight;
    c[ImGuiCol_TabActive] = purplePrimary;
    c[ImGuiCol_TabUnfocused] = bgSecondary;
    c[ImGuiCol_TabUnfocusedActive] = purpleMuted;

    // 文本
    c[ImGuiCol_Text] = textMain;
    c[ImGuiCol_TextDisabled] = textMuted;

    // 分隔线
    c[ImGuiCol_Separator] = ImVec4(0.7f, 0.62f, 0.82f, 0.4f);
    c[ImGuiCol_SeparatorHovered] = purplePrimary;
    c[ImGuiCol_SeparatorActive] = purpleDark;
    c[ImGuiCol_Border] = ImVec4(0.75f, 0.68f, 0.85f, 0.3f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // 导航
    c[ImGuiCol_NavHighlight] = purpleLight;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.5f, 0.7f, 1.0f, 0.5f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);

    // 圆角 (基于字体缩放)
    float roundScale = g_FontScale;
    s.WindowRounding = (float)(int)(10.0f * roundScale);
    s.ChildRounding = (float)(int)(8.0f * roundScale);
    s.FrameRounding = (float)(int)(6.0f * roundScale);
    s.GrabRounding = (float)(int)(6.0f * roundScale);
    s.ScrollbarRounding = (float)(int)(6.0f * roundScale);
    s.PopupRounding = (float)(int)(8.0f * roundScale);
    s.TabRounding = (float)(int)(6.0f * roundScale);

    // 间距 (基于字体缩放)
    s.WindowPadding = ImVec2(14.0f * roundScale, 12.0f * roundScale);
    s.FramePadding = ImVec2(10.0f * roundScale, 8.0f * roundScale);
    s.ItemSpacing = ImVec2(10.0f * roundScale, 8.0f * roundScale);
    s.ItemInnerSpacing = ImVec2(8.0f * roundScale, 6.0f * roundScale);
    s.TouchExtraPadding = ImVec2(4.0f * roundScale, 4.0f * roundScale);
    s.IndentSpacing = 22.0f * roundScale;
    s.ColumnsMinSpacing = 8.0f * roundScale;

    // 大小
    s.ScrollbarSize = (float)(int)(14.0f * roundScale);
    s.GrabMinSize = (float)(int)(12.0f * roundScale);
}

// ===================== UI Interface =====================
static bool g_ShowUI = true;
static bool g_Expanded = true;

// 辅助函数：带悬停提示的按钮
static inline bool IconButton(const char* label, const ImVec4& color, const ImVec4& hoverColor, const ImVec2& size, const char* tooltip = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

// 辅助函数：带颜色状态的缩放按钮
static bool ZoomButton(const char* label, bool isActive, const ImVec2& size) {
    if (isActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.3f, 0.7f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.4f, 0.8f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.4f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.55f, 0.95f, 1.0f));
    }
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    return clicked;
}

static void DrawUI() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);

    ImGuiIO& io = ImGui::GetIO();

    // ==================== Collapsed State: Small Button ====================
    if (!g_ShowUI) {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::Begin("##ReopenZoom", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBackground);

        if (ZoomButton("[Z]", false, ImVec2(48, 48))) {
            g_ShowUI = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open Zoom Menu");
        }
        ImGui::End();
        if (g_UIFont) ImGui::PopFont();
        return;
    }

    // ==================== Main Window ====================
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 0), ImVec2(io.DisplaySize.x - 32, io.DisplaySize.y * 0.5f));

    ImGui::Begin("Zoom Mod", &g_ShowUI,
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing);

    // Get state (thread-safe)
    ZoomState state;
    {
        std::lock_guard<std::mutex> lock(g_zoomMutex);
        state = g_zoomState;
    }

    // ----- Title Bar -----
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 0));

    // Collapse/Expand button
    if (IconButton(g_Expanded ? "−" : "+",
            ImVec4(0.92f, 0.86f, 0.98f, 1.0f),
            ImVec4(0.96f, 0.92f, 1.0f, 1.0f),
            ImVec2(32, 32),
            g_Expanded ? "Collapse" : "Expand")) {
        g_Expanded = !g_Expanded;
    }
    ImGui::SameLine();

    // Title text
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.4f, 0.85f, 1.0f));
    ImGui::Text("Zoom Mod");
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // ----- Main Zoom Button -----
    ImGui::Dummy(ImVec2(0, 6));

    float windowWidth = ImGui::GetContentRegionAvail().x;
    float buttonWidth = windowWidth;
    if (buttonWidth > 280) buttonWidth = 280;

    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f);

    const char* buttonLabel = state.zooming ? "Zooming" : "Zoom";
    if (ZoomButton(buttonLabel, state.zooming, ImVec2(buttonWidth, 52))) {
        std::lock_guard<std::mutex> lock(g_zoomMutex);
        g_zoomState.zooming = !g_zoomState.zooming;

        if (g_zoomState.animated) {
            uint64_t diff = unsignedDiff(g_zoomState.lastClientZoom, g_zoomState.zoomLevel);
            int duration = clamp(100, (int)(diff / 150000), 250);

            if (g_zoomState.zooming) {
                g_transition.startTransition(g_zoomState.lastClientZoom, g_zoomState.zoomLevel, duration);
            } else {
                g_transition.startTransition(g_zoomState.zoomLevel, g_zoomState.lastClientZoom, duration);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle zoom state");
    }

    // ----- Settings Panel (when expanded) -----
    if (g_Expanded) {
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        // Row 1: Enabled and Animated checkboxes
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(24, 8));

        ImGui::AlignTextToFramePadding();
        if (ImGui::Checkbox("##Enabled", &state.enabled)) {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            g_zoomState.enabled = state.enabled;
            if (!state.enabled) {
                g_zoomState.zooming = false;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable/Disable zoom");
        ImGui::SameLine();
        ImGui::Text("Enabled");

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (ImGui::Checkbox("##Animated", &state.animated)) {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            g_zoomState.animated = state.animated;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable/Disable smooth animation");
        ImGui::SameLine();
        ImGui::Text("Animated");

        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, 10));

        // Row 2: Zoom slider
        float zoomPercent = ((float)(state.maxZoom - state.zoomLevel) / (float)(state.maxZoom - state.minZoom)) * 100.0f;
        
        ImGui::PushItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.6f, 0.4f, 0.85f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.75f, 0.55f, 0.95f, 1.0f));

        if (ImGui::SliderFloat("##ZoomLevel", &zoomPercent, 0.0f, 100.0f, "%.0f%%")) {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            g_zoomState.zoomLevel = state.maxZoom - (uint64_t)((zoomPercent / 100.0f) * (float)(state.maxZoom - state.minZoom));
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Adjust zoom level");
        }

        ImGui::PopStyleColor(2);
        ImGui::PopItemWidth();
        ImGui::Dummy(ImVec2(0, 6));

        // Row 3: Status
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Status").x - ImGui::CalcTextSize("Normal").x - 8) * 0.5f);
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Status: ");
        ImGui::SameLine();
        if (state.zooming) {
            ImGui::TextColored(ImVec4(0.6f, 0.4f, 0.85f, 1.0f), "Zooming");
        } else {
            ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Normal");
        }

        // Sync state back to global
        {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            g_zoomState.enabled = state.enabled;
            g_zoomState.animated = state.animated;
        }
    }

    ImGui::End();

    // ==================== Independent Zoom Button (Bottom Right) ====================
    if (g_ShowUI) {
        ZoomState btnState;
        {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            btnState = g_zoomState;
        }

        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 92, io.DisplaySize.y - 92), ImGuiCond_Always);
        ImGui::Begin("##IndependentZoom", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBackground);

        if (ZoomButton("Z", btnState.zooming, ImVec2(72, 72))) {
            std::lock_guard<std::mutex> lock(g_zoomMutex);
            g_zoomState.zooming = !g_zoomState.zooming;

            if (g_zoomState.animated) {
                uint64_t diff = unsignedDiff(g_zoomState.lastClientZoom, g_zoomState.zoomLevel);
                int duration = clamp(100, (int)(diff / 150000), 250);

                if (g_zoomState.zooming) {
                    g_transition.startTransition(g_zoomState.lastClientZoom, g_zoomState.zoomLevel, duration);
                } else {
                    g_transition.startTransition(g_zoomState.zoomLevel, g_zoomState.lastClientZoom, duration);
                }
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Toggle Zoom (Click)");
        }

        ImGui::End();
    }

    if (g_UIFont) ImGui::PopFont();
}

// ===================== GL State Protection =====================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst, bSrcA, bDstA;
    GLboolean blend, cull, depth, scissor, stencil, dither;
    GLint frontFace, activeTexture;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.activeTexture);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFuncSeparate(s.bSrc, s.bDst, s.bSrcA, s.bDstA);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glFrontFace(s.frontFace);
}

// ===================== ImGui Initialization =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    // 根据屏幕高度计算字体缩放比例 (基准 720p)
    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    // 设置字体大小 (基准 20px，根据缩放调整)
    ImFontConfig cfg;
    cfg.SizePixels = (float)(int)(20.0f * g_FontScale);
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    g_UIFont = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    SetupStyle();
    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s; SaveGL(s);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawUI();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ===================== EGL Render Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
        }
    }

    if (ctx != g_TargetContext || s != g_TargetSurface)
        return orig_eglSwapBuffers(d, s);

    g_Width = w; g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(d, s);
}

// ===================== Input Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}

static void HookInput() {
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    
    GHandle egl = GlossOpen("libEGL.so");
    if (!egl) return nullptr;
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    
    HookInput();
    
    if (!findAndHookCameraAPI()) {
        LOGE("Failed to hook CameraAPI");
    }
    
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
