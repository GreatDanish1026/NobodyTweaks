// NobodyTweaks: a ReShade add-on for Nobody Wants to Die (Critical Hit Games, UE 5.3, project
// detnoir, DirectX 12) that switches on the game's hidden hardware ray tracing, raises its
// lighting, shadow and detail settings beyond the menu, live, from an in-game overlay, and turns
// on Unreal's own HDR output (the game has none). Built on TrueColorsTweaks (game-thread setting,
// HDR, measuring), with the options from NobodyTweaks' probe (docs/FINDINGS.md).
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, vtable layout read from the game's own call sites). Settings are kept in
// ReShade.ini [NobodyTweaks]. Values are set on the game thread, from the console manager's
// once-per-frame CallAllConsoleVariableSinks (GameTick), so they reach the renderer between
// frames like the console's own; they are set at console priority (the menu can't override
// them), and letting go puts the game's value and priority back. F8 switches all of it off and
// on.
//
// Threads: ReShade's present thread draws the overlay, measures frame times and only changes
// options (g_state). The game thread does all setting and the probe: it only tries g_state, to
// copy what's wanted, and skips a frame if the present thread holds it. It never holds g_state
// while it sets anything: a change callback can flush rendering, which waits for the render
// thread, which may be presenting and waiting for g_state. What the overlay shows of the game
// thread's state comes from a copy it publishes under g_viewMutex (also never held across a
// Set), and the cvar lookup cache has its own lock.
//
// Ray tracing: the exe starts with r.RayTracing=1 and r.RayTracing.EnableOnDemand=1 (ray tracing
// can be switched on while the game runs), but its menu never turns it on.
//
// The add-on never patches game code. It writes NobodyTweaks.log (and -probe.txt) next to the
// game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Ray tracing, lighting, detail and HDR settings beyond " DT_GAME "'s menu, live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

const uint8_t* GameBase() { return reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)); }

uint32_t PeTimestamp(const uint8_t* base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

// ReShade can load the add-on more than once per process (the game makes test devices at
// start), so the log is started fresh only by the first load in a process (marked in an
// environment variable).
void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    wchar_t pid[32] = {}, mine[32];
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"NOBODYTWEAKS_PID", pid, 32);
    const bool again = !wcscmp(pid, mine);
    SetEnvironmentVariableW(L"NOBODYTWEAKS_PID", mine);
    g_log = _wfopen((g_exeDir + DT_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded%s", again ? " (again in this process)" : "");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. The Game choice shows the game's current value (the probe writes them all).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

constexpr const char* kRayTracingTab = "Ray tracing";
constexpr const char* kHdrTab = "HDR";

std::vector<Option> g_options = {
    // ------------------------------------------------ ray tracing (hidden by the game's menu)
    {"Ray tracing", "Hardware ray tracing", "HwRt", "Hardware ray tracing",
     "The game has ray tracing built in but its menu doesn't show it. On: Lumen's lighting\n"
     "and reflections trace the real scene on the GPU's ray tracing hardware instead of a\n"
     "coarse distance field: accurate reflections, fewer light leaks, better bounce light.\n"
     "Costs frame rate.",
     L"r.Lumen.HardwareRayTracing",
     {{"Game (off)", 0, {}}, {"On", 1, {{L"r.RayTracing.Enable", L"1"}, {L"r.Lumen.HardwareRayTracing", L"1"}}}}},
    {"Ray tracing", "Hardware ray tracing", "HitLighting", "Hit lighting",
     "With hardware ray tracing on: reflections are lit with real lighting at the point the\n"
     "ray hits instead of Lumen's coarse surface cache. Mirrors, wet streets and glossy\n"
     "surfaces show correct, sharp lighting. The costliest option here.",
     L"r.Lumen.HardwareRayTracing.LightingMode",
     {{"Game (surface cache)", 0, {}},
      {"Reflections", 1,
       {{L"r.Lumen.HardwareRayTracing.LightingMode", L"2"}, {L"r.Lumen.Reflections.HardwareRayTracing.Retrace.HitLighting", L"1"}}}}},
    // UE's standalone ray traced shadows and translucency (r.RayTracing.Shadows/.Translucency)
    // were offered in 0.2.0-0.3.0; with them on, hardware ray tracing "looked really bad" (user,
    // 2026-11-01), so they're gone (docs/FINDINGS.md).

    // ------------------------------------------------ lighting (software Lumen and shadows)
    {"Lighting", "Global illumination", "GiDetail", "Global illumination detail",
     "Lumen's bounce lighting. The game uses UE's Epic values (probe budget 300, one probe\n"
     "per 16 pixels). High raises the budget to UE's Cinematic 1000; Cinematic also places\n"
     "4x as many probes with finer tracing: steadier, less blotchy indirect light.",
     L"r.Lumen.ScreenProbeGather.DownsampleFactor",
     {{"Game", 0, {}},
      {"High", 1, {{L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"1000"}}},
      {"Cinematic", 2,
       {{L"r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget", L"1000"},
        {L"r.Lumen.ScreenProbeGather.DownsampleFactor", L"8"},
        {L"r.Lumen.ScreenProbeGather.TracingOctahedronResolution", L"16"},
        {L"r.Lumen.ScreenProbeGather.FullResolutionJitterWidth", L"0.5"}}}}},
    {"Lighting", "Global illumination", "MeshSdf", "Detailed tracing",
     "Without hardware ray tracing, Lumen traces only a coarse distance field of the whole\n"
     "scene (the game turns per-object tracing off). On: it traces each object's own\n"
     "distance field: fewer light leaks and better shadowing of bounce light. No effect\n"
     "with hardware ray tracing on.",
     L"r.Lumen.TraceMeshSDFs",
     {{"Game (off)", 0, {}}, {"On", 1, {{L"r.Lumen.TraceMeshSDFs", L"1"}}}}},
    {"Lighting", "Global illumination", "ShortAo", "Contact shadows",
     "Small-scale ambient occlusion in corners, creases and under objects, which the game\n"
     "turns off. Screen: from the depth buffer. Ray traced: with hardware ray tracing,\n"
     "also correct at screen edges and behind objects.",
     L"r.Lumen.ScreenProbeGather.ShortRangeAO",
     {{"Game (off)", 0, {}},
      {"Screen", 1, {{L"r.Lumen.ScreenProbeGather.ShortRangeAO", L"1"}}},
      {"Ray traced", 2,
       {{L"r.Lumen.ScreenProbeGather.ShortRangeAO", L"1"}, {L"r.Lumen.ScreenProbeGather.ShortRangeAO.HardwareRayTracing", L"1"}}}}},
    {"Lighting", "Reflections", "Roughness", "Reflections on rougher surfaces",
     "Lumen's traced reflections stop at roughness 0.4 in this game; rougher surfaces (worn\n"
     "metal, wood, painted walls) fall back to a blurry approximation. Higher traces more.",
     L"r.Lumen.Reflections.MaxRoughnessToTrace",
     {{"Game", 0, {}},
      {"0.6", 6,
       {{L"r.Lumen.Reflections.MaxRoughnessToTrace", L"0.6"}}},
      {"0.8", 8,
       {{L"r.Lumen.Reflections.MaxRoughnessToTrace", L"0.8"}}},
      {"All (1.0)", 10,
       {{L"r.Lumen.Reflections.MaxRoughnessToTrace", L"1"}}}}},
    {"Lighting", "Reflections", "FrontLayer", "Glass reflections",
     "Lumen reflections on windows, glass and water surfaces, which the game leaves off.",
     L"r.Lumen.TranslucencyReflections.FrontLayer.Enable",
     {{"Game (off)", 0, {}},
      {"On", 1,
       {{L"r.Lumen.TranslucencyReflections.FrontLayer.EnableForProject", L"1"},
        {L"r.Lumen.TranslucencyReflections.FrontLayer.Enable", L"1"}}}}},
    {"Lighting", "Shadows", "Shadows", "Shadow quality",
     "Sharp: twice the shadow memory (game 4096 pages) and full-resolution shadows from\n"
     "moving lights (the game halves them): steadier shadows from flashlights and lamps.\n"
     "Cinematic: also 16 shadow rays (game 8) for more accurate soft shadows.",
     L"r.Shadow.Virtual.SMRT.RayCountLocal",
     {{"Game", 0, {}},
      {"Sharp", 1,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"}, {L"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", L"0"}}},
      {"Cinematic", 2,
       {{L"r.Shadow.Virtual.MaxPhysicalPages", L"8192"},
        {L"r.Shadow.Virtual.ResolutionLodBiasLocalMoving", L"0"},
        {L"r.Shadow.Virtual.SMRT.RayCountDirectional", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayDirectional", L"8"},
        {L"r.Shadow.Virtual.SMRT.RayCountLocal", L"16"},
        {L"r.Shadow.Virtual.SMRT.SamplesPerRayLocal", L"8"}}}}},
    {"Lighting", "Fog", "FogCells", "Volumetric fog detail",
     "Size of the fog's light grid in screen pixels (game 8 px): smaller cells give crisper\n"
     "light shafts and less blocky shadows in the rain and smoke. 4 px is about 4x the fog\n"
     "cost.",
     L"r.VolumetricFog.GridPixelSize",
     {{"Game", 0, {}},
      {"Fine (6 px)", 6, {{L"r.VolumetricFog.GridPixelSize", L"6"}, {L"r.VolumetricFog.HistoryMissSupersampleCount", L"8"}}},
      {"Finest (4 px)", 4, {{L"r.VolumetricFog.GridPixelSize", L"4"}, {L"r.VolumetricFog.HistoryMissSupersampleCount", L"16"}}}}},

    // ------------------------------------------------ detail
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The game uses 2.5 GB, so textures can go blurry\n"
     "and reload in larger areas. The RTX 5080 has 16 GB.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper streets and floors at shallow angles; the game uses 8x. Textures already\n"
     "loaded keep their filtering until they reload.",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}, {L"r.VT.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Textures", "MipBias", "Texture sharpness",
     "Uses sharper texture detail levels than the engine picks. Helps textures that look\n"
     "soft after upscaling; too sharp can shimmer on fine patterns.",
     L"r.MipMapLODBias",
     {{"Game", 0, {}},
      {"Sharper (-0.5)", 5, {{L"r.MipMapLODBias", L"-0.5"}}},
      {"Sharpest (-1)", 10, {{L"r.MipMapLODBias", L"-1"}}}}},
    {"Detail", "Geometry", "NaniteDetail", "Geometry detail",
     "Triangle size of the detailed (Nanite) models, in pixels (game 1). Smaller keeps more\n"
     "detail on distant buildings.",
     L"r.Nanite.MaxPixelsPerEdge",
     {{"Game", 0, {}}, {"Finer (0.5 px)", 1, {{L"r.Nanite.MaxPixelsPerEdge", L"0.5"}}}}},
    {"Detail", "Geometry", "ViewDistance", "Object draw distance",
     "Objects and their detail models switch out further away (game 1x).",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}}, {"x2", 2, {{L"r.ViewDistanceScale", L"2"}}}}},
    {"Detail", "Post-processing", "FilmGrain", "Film grain", "Unreal's film grain noise (no menu option).",
     L"r.FilmGrain", {{"Game", 0, {}}, {"Off", 1, {{L"r.FilmGrain", L"0"}}}}},
    {"Detail", "Post-processing", "LensFlares", "Lens flares", "Ghost reflections of bright lights across the screen.",
     L"r.LensFlareQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {"Detail", "Post-processing", "Bloom", "Bloom", "Glow around bright lights.",
     L"r.BloomQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.BloomQuality", L"0"}}}}},

    // Frame generation (the game's FSR 3 frame interpolation, 0.2.0-0.3.5) moved to NobodyDLSS,
    // which handles DLSS and FSR upscaling and frame generation together.

    // ------------------------------------------------ HDR (the game has no HDR setting)
    // As in TrueColorsTweaks: r.HDR.EnableHDROutput with the Rec. 2020 gamut makes the engine
    // switch its swap chain to HDR10. The output device and the brightness sliders below are
    // added in Wanted() while this is on.
    {kHdrTab, "HDR output", "Hdr", "HDR",
     "The game has no HDR setting. This turns on Unreal's own HDR output: HDR10 (PQ,\n"
     "Rec. 2020) tone-mapped for your display's peak brightness (set below). Windows HDR\n"
     "must be on.",
     L"r.HDR.EnableHDROutput",
     {{"Off (game)", 0, {}},
      {"HDR10", 1, {{L"r.HDR.EnableHDROutput", L"1"}, {L"r.HDR.Display.ColorGamut", L"2"}}}}},
};

// HDR brightness (used while the HDR option is on). Peak goes to r.HDR.Display.MaxLuminance (0 in
// the game = the output device's default) with ACES output device 3 (ST-2084). UI and mid-grey
// at 0 leave the game's values (300 and 15 nits).
int g_hdrPeak = 1000;
// Hardware ray tracing comes back on at launch only once it has been seen running (15 s of
// structure builds): 0.2.0 locked the game up switching it on, and a saved choice must not do
// that at every launch.
int g_hwRtConfirmed = 0;
int g_hdrUi = 0;
int g_hdrMid = 0;

// Guards the options, the measurement and the requests to the game thread (present thread;
// the game thread only tries it, briefly).
std::recursive_mutex g_state;

std::atomic<bool> g_enabled{true};  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrPeakNits", g_hdrPeak);
    reshade::get_config_value(nullptr, DT_PRODUCT, "HwRtConfirmed", g_hwRtConfirmed);
    for (Option& o : g_options)
        if (!strcmp(o.key, "HwRt") && o.value && !g_hwRtConfirmed) {
            o.value = 0;
            Log("hardware ray tracing was on but never confirmed running: left off at launch");
        }
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrUiNits", g_hdrUi);
    reshade::get_config_value(nullptr, DT_PRODUCT, "HdrMidNits", g_hdrMid);
    g_hdrPeak = std::clamp(g_hdrPeak, 400, 10000);
    if (g_hdrUi && (g_hdrUi < 80 || g_hdrUi > 1000)) g_hdrUi = 0;
    if (g_hdrMid && (g_hdrMid < 5 || g_hdrMid > 50)) g_hdrMid = 0;
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPeakNits", g_hdrPeak);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HwRtConfirmed", g_hwRtConfirmed);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrUiNits", g_hdrUi);
    reshade::set_config_value(nullptr, DT_PRODUCT, "HdrMidNits", g_hdrMid);
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    if (m.count(L"r.HDR.EnableHDROutput")) {  // HDR on: output device and brightness sliders
        m[L"r.HDR.Display.OutputDevice"] = L"3";
        m[L"r.HDR.Display.MaxLuminance"] = std::to_wstring(g_hdrPeak);
        if (g_hdrUi) m[L"r.HDR.UI.Luminance"] = std::to_wstring(g_hdrUi);
        if (g_hdrMid) m[L"r.HDR.Display.MidLuminance"] = std::to_wstring(g_hdrMid);
    }
    return m;
}

// ---------------------------------------------------------------- device

std::atomic<int> g_api{0};  // device_api of the presenting device (0 = not seen yet)

bool OnDirectX12() { return g_api.load() == static_cast<int>(device_api::d3d12); }

const char* ApiName() {
    switch (static_cast<device_api>(g_api.load())) {
        case device_api::d3d11: return "DirectX 11";
        case device_api::d3d12: return "DirectX 12";
        case device_api::vulkan: return "Vulkan";
        default: return "?";
    }
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
std::string g_liveError;

// Game thread only:
std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, uint32_t> g_origPriority;   // ...and the priority it was set at
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)

// Requests from the present thread (under g_state):
std::atomic<bool> g_dirty{true};  // options changed: apply at the next game tick
std::vector<std::pair<std::wstring, std::wstring>> g_pendingConsole;  // Advanced tab's "Set once"
std::string g_pendingProbe;                                           // Advanced tab's probe request

std::atomic<int> g_reapplied{0};    // values put back after they changed
std::atomic<bool> g_hooked{false};  // game-thread hook in place
std::atomic<uint64_t> g_ticks{0};   // game ticks seen by the hook
std::atomic<int> g_rayTracing{-1};  // r.RayTracing as the game thread last read it
ULONGLONG g_readyTick = 0;

// What the overlay shows of the game thread's state; the game thread publishes it.
struct View {
    std::map<std::wstring, std::wstring> held;  // held cvar -> the game's value
    std::vector<std::string> rejected;
    std::string probeResult;
};
std::mutex g_viewMutex;
View g_view;

void PublishView() {
    View v;
    for (auto& [key, value] : g_applied) {
        auto it = g_originals.find(key);
        v.held[key] = it != g_originals.end() ? it->second : L"?";
    }
    for (const std::wstring& k : g_rejected) v.rejected.push_back(Narrow(k));
    std::lock_guard<std::mutex> lock(g_viewMutex);
    v.probeResult = std::move(g_view.probeResult);
    g_view = std::move(v);
}

View CopyView() {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_view;
}

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::mutex g_varsMutex;
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_varsMutex);
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    if (!g_manager) return info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

// How many change callbacks a variable has: OnChangedCallback is a multicast delegate at +0x20
// with its invocation count at +0x28 (FConsoleVariableBase in UE 4.2x/5.x; logged only).
int Callbacks(void* var) {
    int32_t n = -1;
    cvars::SafeCopy(&n, static_cast<uint8_t*>(var) + 0x28, 4);
    return n;
}

void __fastcall HookedSinks(void* self);
cvars::SinksFn g_origSinks = nullptr;
int g_sinkFlag = -1;  // FConsoleManager::bCallAllConsoleVariableSinks offset, read before hooking

void FindManager() {
    const uint8_t* base = GameBase();
    Log("game exe timestamp 0x%08x (%s)", PeTimestamp(base),
        PeTimestamp(base) == DT_KNOWN_PE_TIMESTAMP ? "the tested build" : "NOT the tested build");
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (RVA 0x%llx, %d of %d matches), manager %p", singleton,
        singleton ? static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(singleton) - base) : 0ull, votes,
        matches, manager);
    if (!manager || !cvars::IsConsoleManager(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    // r.ScreenPercentage can read 0 (UE 5's "automatic"), so r.ViewDistanceScale (positive)
    // confirms float reads too.
    void* view = cvars::FindVariable(manager, L"r.ViewDistanceScale");
    float d = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s) && view &&
                 cvars::HasModuleVtable(base, view) && cvars::GetFloat(view, d);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, vtables %d, IsVariableBool at +0x%x, values %d / %g / "
        "view distance %g",
        aniso, screen, vt, cvars::g_isVariableBool, a, s, d);
    if (!reads || a < 0 || a > 16 || !(s == 0 || (s >= 10 && s <= 400)) || !(d > 0.01f && d <= 100)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    for (auto [name, var] : {std::pair<const char*, void*>{"r.MaxAnisotropy", aniso}, {"r.ScreenPercentage", screen}}) {
        cvars::Type t = cvars::GetType(var);
        void* data = cvars::AsData(var, t);
        Log("  %s: %s, priority 0x%02x, data %s (+0x%llx), %d callbacks", name, cvars::TypeName(t), cvars::Priority(var),
            data ? "yes" : "no",
            data ? static_cast<unsigned long long>(static_cast<uint8_t*>(data) - static_cast<uint8_t*>(var)) : 0ull,
            Callbacks(var));
    }
    {
        std::lock_guard<std::mutex> lock(g_varsMutex);
        g_manager = manager;
    }
    g_sinkFlag = cvars::FindSinkFlagOffset(manager);  // from the game's own code, before it's hooked
    Log("console variable sinks flag at +0x%x (now %d)", g_sinkFlag, cvars::ReadSinkFlag(manager, g_sinkFlag));
    if (!cvars::HookSinks(base, manager, &HookedSinks, &g_origSinks)) {
        g_liveError = "couldn't reach the game thread (see the log)";
        Log("game-thread hook: CallAllConsoleVariableSinks slot not found");
        g_live = Live::Failed;
        return;
    }
    g_hooked = true;
    Log("game-thread hook in place (CallAllConsoleVariableSinks was %p, RVA 0x%llx)",
        reinterpret_cast<void*>(g_origSinks),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(g_origSinks) - base));
    g_readyTick = GetTickCount64();
    g_live = Live::Ready;
}

// Puts <value> in force at console priority. Game thread only.
bool SetVar(const std::wstring& key, const std::wstring& value) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str(), cvars::kSetByConsole)) return false;
    std::wstring now;  // on the game thread the getters read the main value, just set
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second)
            Log("%ls = %ls not taken (reads %ls, priority 0x%02x)", key.c_str(), value.c_str(), now.c_str(),
                cvars::Priority(v.var));
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Gives a value back: the game's value, then the game's priority (so its menu can set it again).
bool RestoreVar(const std::wstring& key) {
    auto it = g_originals.find(key);
    if (it == g_originals.end() || !SetVar(key, it->second)) return false;
    auto pr = g_origPriority.find(key);
    VarInfo v = Lookup(key);
    if (pr != g_origPriority.end() && v.var) cvars::RestorePriority(v.var, pr->second);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's. Game thread.
void Apply(std::map<std::wstring, std::wstring> want) {
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) {
                g_originals[key] = old;
                if (VarInfo v = Lookup(key); v.var) g_origPriority[key] = cvars::GetFlags(v.var) & cvars::kPriorityMask;
            }
        }
        // Already held at this value and still in force: nothing to set (a slider drag re-applies
        // every frame, and every Set makes the engine run its sinks).
        auto held = g_applied.find(key);
        std::wstring cur;
        if (held != g_applied.end() && held->second == value && Read(key, cur) && SameValue(Lookup(key).type, cur, value)) {
            ++set;
            continue;
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        if (RestoreVar(key)) ++restored;
    }
    g_applied = std::move(want);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
    PublishView();
}

// Every second: any value that changed anyway (the game's code can set at console priority too)
// becomes the new game value, and ours goes back in. Game thread.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    bool any = false;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++g_reapplied;
        any = true;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (any) PublishView();
}

// ---------------------------------------------------------------- probe
//
// Writes the game's value of every console variable under the prefixes below to
// NobodyTweaks-probe.txt: automatically 5 s and 60 s after connecting (title screen, then
// after the game has applied its menu), and from the Advanced tab at any time (in a scene). Names
// come from the game's read-only data (UTF-16 strings); nothing is enumerated. Runs on the game
// thread, so it reads the game thread's values.

const char* const kProbePrefixes[] = {
    "r.Lumen.", "r.RayTracing", "r.Raytracing", "r.Shadow.", "r.TSR.", "r.TemporalAA", "r.AntiAliasing",
    "r.ScreenPercentage", "r.SecondaryScreenPercentage", "r.DynamicRes.", "r.DynamicGlobalIlluminationMethod",
    "r.ReflectionMethod", "r.Streaming.", "r.MaxAnisotropy", "r.VT.", "r.HDR.", "r.Tonemapper", "r.Nanite.MaxPixelsPerEdge",
    "r.Nanite.ViewMeshLODBias", "r.HairStrands.", "r.SSS.", "r.SubsurfaceScattering", "r.SkinCache.", "r.MegaLights.",
    "r.VolumetricFog", "r.VolumetricCloud", "r.Bloom", "r.LensFlare", "r.DepthOfField", "r.DOF.", "r.MotionBlur",
    "r.SceneColorFringe", "r.FilmGrain", "r.Substrate", "r.PostProcessAAQuality", "r.SSR.", "r.AmbientOcclusion",
    "r.DistanceField", "r.ContactShadows", "r.CapsuleShadows", "r.ViewDistanceScale", "r.StaticMeshLODDistanceScale",
    "r.SkeletalMeshLODBias", "r.LandscapeLODBias", "r.Translucency", "r.SSGI", "r.LightMaxDrawDistanceScale",
    "r.MipMapLODBias", "r.Sharpen", "r.LUT.", "r.Color.", "r.EyeAdaptation", "r.DefaultFeature.", "r.FidelityFX",
    "r.D3D12.DXR", "foliage.", "grass.", "sg.", "r.PathTracing", "r.NGX.", "r.Streamline.", "r.XeSS", "r.FSR",
    "r.NIS.", "r.Reflex", "r.PostProcessing.", "r.Velocity.", "r.BasePassForceOutputsVelocity",
};

std::set<std::wstring> g_probeNames;  // collected once

void CollectProbeNames() {
    std::vector<std::wstring> prefixes;
    for (const char* p : kProbePrefixes) prefixes.emplace_back(p, p + strlen(p));
    for (const cvars::Section& s : cvars::Sections(GameBase(), cvars::SectionKind::ReadOnly)) {
        for (size_t i = 2; i + 8 < s.size; i += 2) {
            if (s.begin[i - 2] || s.begin[i - 1]) continue;  // must start right after a terminator
            const wchar_t* w = reinterpret_cast<const wchar_t*>(s.begin + i);
            wchar_t c0 = w[0];
            if (c0 != L'r' && c0 != L'f' && c0 != L'g' && c0 != L's') continue;
            size_t len = 0;
            while (len < 120 && i + 2 * len + 2 < s.size && w[len] > 32 && w[len] < 127) ++len;
            if (len < 3 || w[len] != 0) continue;
            for (const std::wstring& p : prefixes)
                if (len >= p.size() && !wcsncmp(w, p.c_str(), p.size())) {
                    g_probeNames.emplace(w, len);
                    break;
                }
        }
    }
    Log("probe: %zu candidate names", g_probeNames.size());
}

void WriteProbe(const char* label) {
    if (g_probeNames.empty()) CollectProbeNames();
    FILE* f = _wfopen((g_exeDir + DT_PRODUCT_W L"-probe.txt").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== %s (%04u-%02u-%02u %02u:%02u:%02u, %.0f s after start, %s, tweaks %s, holding %zu)\n", label,
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount64() - g_loadTick) / 1000.0, ApiName(),
            g_enabled ? "on" : "off", g_applied.size());
    int found = 0, unreadable = 0;
    for (const std::wstring& name : g_probeNames) {
        VarInfo v = Lookup(name);
        if (!v.var) continue;
        std::wstring value;
        if (Read(name, value)) {
            ++found;
            fprintf(f, "%ls (%s, set by 0x%02x) = %ls\n", name.c_str(), cvars::TypeName(v.type), cvars::Priority(v.var),
                    value.c_str());
        } else {
            ++unreadable;
            fprintf(f, "%ls (%s) = ?\n", name.c_str(), cvars::TypeName(v.type));
        }
    }
    fprintf(f, "---- %d values, %d unreadable (strings/commands skipped)\n", found, unreadable);
    fclose(f);
    Log("probe '%s': %d values written", label, found);
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_view.probeResult = std::to_string(found) + " values written to " DT_PRODUCT "-probe.txt";
}

void ProbeOnTick(const std::string& request) {
    static int done = 0;
    ULONGLONG since = GetTickCount64() - g_readyTick;
    if (done == 0 && since > 5000) WriteProbe("auto: 5 s after connecting"), done = 1;
    else if (done == 1 && since > 60000) WriteProbe("auto: 60 s after connecting"), done = 2;
    if (!request.empty()) WriteProbe(request.c_str());
}

// ---------------------------------------------------------------- game thread

// Once per frame, before the game's sinks run: all setting happens here.
void GameTick() {
    ++g_ticks;
    if (g_live.load() != Live::Ready) return;
    // Copy the requests; g_state is let go before anything is set (see the header).
    bool dirty = false;
    std::map<std::wstring, std::wstring> want;
    std::vector<std::pair<std::wstring, std::wstring>> console;
    std::string probe;
    {
        std::unique_lock<std::recursive_mutex> lock(g_state, std::try_to_lock);
        if (lock.owns_lock()) {
            dirty = g_dirty.exchange(false);
            if (dirty) want = Wanted();
            console.swap(g_pendingConsole);
            probe.swap(g_pendingProbe);
        }
    }
    for (auto& [key, value] : console) {
        bool ok = SetVar(key, value);
        Log("console: %ls = %ls %s", key.c_str(), value.c_str(), ok ? "set" : "NOT taken");
    }
    if (!console.empty()) PublishView();
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys = {L"r.RayTracing", L"r.RayTracing.Enable", L"r.RayTracing.EnableOnDemand",
                                       L"r.ScreenPercentage", L"r.DynamicGlobalIlluminationMethod",
                                       L"r.HDR.EnableHDROutput", L"r.FidelityFX.FSR3.Enabled"};
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s, priority 0x%02x, %d callbacks) = %ls", key.c_str(), cvars::TypeName(info.type),
                info.var ? cvars::Priority(info.var) : -1, info.var ? Callbacks(info.var) : 0,
                Read(key, v) ? v.c_str() : L"(not found)");
        }
    }
    {
        static ULONGLONG lastRt = 0;
        if (GetTickCount64() - lastRt > 2000) {
            lastRt = GetTickCount64();
            std::wstring v;
            g_rayTracing = Read(L"r.RayTracing", v) ? _wtoi(v.c_str()) : -1;
        }
    }
    if (dirty) Apply(std::move(want));
    Hold();
    ProbeOnTick(probe);
}

// The engine's HDR sink re-applies the game's own HDR brightness (1000) to
// r.HDR.Display.MaxLuminance whenever any console variable changed, keeping the console priority.
// So right after the sinks the held peak goes back in, before the frame is rendered. That Set
// raises the "run the sinks" flag again (0.3.1: the sink then ran every frame), so the flag is
// cleared right after it: the sinks run again only for the next real change. (0.3.2 wrote the
// value straight into the variable's data instead and the game crashed; not done any more.)
// Game thread.

void AfterSinks() {
    static const std::wstring kPeak = L"r.HDR.Display.MaxLuminance";
    auto it = g_applied.find(kPeak);
    if (it == g_applied.end()) return;
    VarInfo v = Lookup(kPeak);
    std::wstring cur;
    if (!v.var || !Read(kPeak, cur) || SameValue(v.type, cur, it->second)) return;
    const int before = cvars::ReadSinkFlag(g_manager, g_sinkFlag);
    cvars::Set(v.var, it->second.c_str(), cvars::kSetByConsole);
    if (before == 0) cvars::SetSinkFlag(g_manager, g_sinkFlag, 0);  // only the flag our Set raised
    static int n = 0;
    if (++n <= 3 || n % 100 == 0)
        Log("HDR peak: the engine's HDR sink set %ls; %ls back in after the sinks (%d times)", cur.c_str(),
            it->second.c_str(), n);
}

void __fastcall HookedSinks(void* self) {
    GameTick();
    g_origSinks(self);  // then the game's sinks, which see what GameTick set
    AfterSinks();
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset are left alone. The preset shown is derived from the options,
// "Custom" when none matches. Ray tracing and HDR are in no preset.

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = not measured)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

// Costs from "Measure each option" (2026-11-01, RTX 5080, 4K, DLSS, one scene, Game 12.43 ms):
// GI Cinematic +60%, fog Finest +18%, shadows Cinematic +18%, reflections 0.6-1.0 +5-6%, fog Fine
// +4%, GI High +2.4%, detailed tracing +1.8%, glass reflections +0.8%, the rest within +-1%.
// Ultra+ takes the cheap ones. Measured as a whole (0.3.4): Game 12.84 ms (77.9 fps), Ultra+
// 14.48 ms (69.0, +13%), Cinematic 25.26 ms (39.6, +97%).
const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything below at Game.", {}},
    {"Ultra+", "about 13% slower",
     "The upgrades that cost little: finer Lumen lighting (High) with detailed tracing and\n"
     "contact shadows, reflections on glass and on surfaces up to roughness 0.6, sharp\n"
     "shadows, fine fog, 8 GB textures, 16x filtering, finer geometry and longer draw\n"
     "distance. The Ray tracing and HDR tabs are left as they are.\n\n"
     "Measured on an RTX 5080 at 4K with DLSS in one scene: 78 fps with Game, 69 with\n"
     "Ultra+.",
     {{"GiDetail", 1}, {"MeshSdf", 1}, {"ShortAo", 1}, {"Roughness", 6}, {"FrontLayer", 1}, {"Shadows", 1},
      {"FogCells", 6}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"NaniteDetail", 1}, {"ViewDistance", 2}}},
    {"Cinematic", "about 97% slower",
     "Ultra+ plus the costly upgrades: Cinematic Lumen lighting (4x the probes, +60% alone),\n"
     "Cinematic shadows (16 rays, +18%) and reflections up to roughness 0.8. For screenshots\n"
     "or a lot of GPU headroom.\n\n"
     "Measured on an RTX 5080 at 4K with DLSS in one scene: 78 fps with Game, 40 with\n"
     "Cinematic.",
     {{"GiDetail", 2}, {"MeshSdf", 1}, {"ShortAo", 1}, {"Roughness", 8}, {"FrontLayer", 1}, {"Shadows", 2},
      {"FogCells", 6}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"NaniteDetail", 1}, {"ViewDistance", 2}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with frame generation the present intervals alternate.
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// A measurement runs stages one after another: each sets the quality options (key -> choice id,
// every other measured option at Game), settles, then averages the frame time. "Presets" runs
// Game and each preset; "Each option" runs Game, then every quality option (all tabs but
// Post-processing; Ray tracing only on DirectX 12) at each of its choices, one at a time, then
// Game again (to show drift).
// Ray tracing activity counters (filled in by the ray tracing activity section below).
std::atomic<uint64_t> g_asBuilds{0}, g_rayDispatches{0};
uint64_t g_asPerSec = 0, g_raysPerSec = 0;

struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 6000, sampleMs = 8000;
    std::vector<Stage> stages;
    std::set<std::string> keys;  // the options the stages control
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : g_measure.keys)
        if (Option* o = FindOption(key.c_str())) o->value = 0;
    for (auto& [key, id] : st.values)
        if (Option* o = FindOption(key.c_str())) o->value = id;
    g_dirty = true;
}

bool Measurable(const Option& o) {
    if (!strcmp(o.tab, kHdrTab)) return false;
    if (!strcmp(o.tab, kRayTracingTab)) return g_asPerSec > 0;  // only while ray tracing runs
    return true;
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 5000, g_measure.sampleMs = 6000;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const Option& o : g_options) {
            if (!Measurable(o)) continue;
            g_measure.keys.insert(o.key);
            for (const Choice& c : o.choices)
                if (c.id) g_measure.stages.push_back({std::string(o.key) + " " + c.label, {{o.key, c.id}}});
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        g_measure.keys = PresetKeys();
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, id] : kPresets[i].values) st.values.push_back({key, id});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages, %s)", perOption ? "each option" : "presets", g_measure.stages.size(), ApiName());
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        std::string log = "measure: result";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
            snprintf(line, sizeof(line), ", %s %.2f ms %+.0f%% frame time", g_measure.stages[i].label.c_str(), r[i],
                     (r[i] / base - 1) * 100);
            log += line;
        }
        g_measure.summary = sum;
        Log("%s (Game %.2f ms)", log.c_str(), r[0]);
        return;
    }
    // Each option, costliest first.
    std::vector<size_t> order;
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in " DT_PRODUCT ".log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- ray tracing activity
//
// Counts acceleration structure builds and DispatchRays calls (DirectX 12): the sure sign that
// the GPU is doing hardware ray tracing. Logged every 10 s while the count changes, for the
// first ten minutes.


bool OnBuildAs(command_list*, acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
               const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
               acceleration_structure_build_mode) {
    ++g_asBuilds;
    return false;
}

bool OnDispatchRays(command_list*, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource,
                    uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) {
    ++g_rayDispatches;
    return false;
}

void RayTracingOnPresent() {
    static ULONGLONG last = 0, lastLog = 0;
    static uint64_t prevAs = 0, prevRays = 0, loggedRays = ~0ull;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    uint64_t as = g_asBuilds.load(), rays = g_rayDispatches.load();
    g_asPerSec = as - prevAs, g_raysPerSec = rays - prevRays;
    prevAs = as, prevRays = rays, last = now;
    if (now - lastLog >= 10000 && now - g_loadTick < 600000 && rays != loggedRays) {
        lastLog = now, loggedRays = rays;
        Log("ray tracing: %llu acceleration structure builds, %llu DispatchRays so far (%llu / %llu per second)", as,
            rays, g_asPerSec, g_raysPerSec);
    }
}

// ---------------------------------------------------------------- present

// ---------------------------------------------------------------- HDR colour space
//
// With r.HDR.EnableHDROutput the engine writes PQ (Rec. 2020) into its 10-bit back buffer and
// should tell DXGI so (SetColorSpace1). If the swap chain still isn't HDR10 two seconds after the
// HDR option went on (the game turned HDR off on DirectX 12, which may include that step), the
// add-on sets the colour space itself, and puts sRGB back when the option goes off. ReShade
// doesn't see a change made this way, so its own overlay may look dim while it's in effect.

ULONGLONG g_hdrSince = 0;  // when the HDR option went on (0 = off)
bool g_hdrOurs = false;    // we set HDR10 on the swap chain ourselves
std::string g_hdrStatus = "off";

bool SetSwapchainColorSpace(swapchain* sc, DXGI_COLOR_SPACE_TYPE cs) {
    auto native = reinterpret_cast<IUnknown*>(sc->get_native());
    IDXGISwapChain3* sc3 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return false;
    UINT support = 0;
    bool ok = SUCCEEDED(sc3->CheckColorSpaceSupport(cs, &support)) &&
              (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(sc3->SetColorSpace1(cs));
    sc3->Release();
    return ok;
}

void HdrOnPresent(swapchain* sc) {
    const Option* o = FindOption("Hdr");
    const bool want = o && o->value && g_enabled && g_live.load() == Live::Ready;
    const ULONGLONG now = GetTickCount64();
    if (!want) {
        g_hdrSince = 0;
        if (g_hdrOurs) {
            const bool ok = SetSwapchainColorSpace(sc, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            Log("HDR off: swap chain colour space back to sRGB %s", ok ? "done" : "FAILED");
            g_hdrOurs = false;
        }
        g_hdrStatus = "off";
        return;
    }
    if (!g_hdrSince) g_hdrSince = now;
    if (sc->get_color_space() == color_space::hdr10_pq && !g_hdrOurs) {
        g_hdrStatus = "HDR10, set by the game";
        return;
    }
    if (g_hdrOurs || now < g_hdrSince + 2000) return;
    const format f = sc->get_device()->get_resource_desc(sc->get_current_back_buffer()).texture.format;
    if (f != format::r10g10b10a2_unorm && f != format::b10g10r10a2_unorm) {
        static format logged = format::unknown;
        if (logged != f) Log("HDR: back buffer is format %u, not 10-bit; can't switch it to HDR10", static_cast<uint32_t>(f));
        logged = f;
        g_hdrStatus = "not possible: the back buffer isn't 10-bit";
        return;
    }
    g_hdrOurs = SetSwapchainColorSpace(sc, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    Log("HDR: the game left the swap chain in %s; set HDR10 ourselves: %s",
        sc->get_color_space() == color_space::srgb ? "sRGB" : "another colour space", g_hdrOurs ? "done" : "FAILED");
    g_hdrStatus = g_hdrOurs ? "HDR10, set by " DT_PRODUCT : "the display refused HDR10 (is Windows HDR on?)";
    if (!g_hdrOurs) g_hdrSince = now + 60000;  // try again in a minute
}

// Marks hardware ray tracing as safe to restore at launch once it has run for 15 s.
void HwRtGuardOnPresent() {
    static ULONGLONG since = 0;
    const Option* o = FindOption("HwRt");
    if (g_hwRtConfirmed || !o || !o->value || !g_enabled || !g_asPerSec) {
        since = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (now - since < 15000) return;
    g_hwRtConfirmed = 1;
    reshade::set_config_value(nullptr, DT_PRODUCT, "HwRtConfirmed", 1);
    Log("hardware ray tracing confirmed running (%llu structure builds per second): restored at launch from now on",
        static_cast<unsigned long long>(g_asPerSec));
}

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void LiveOnPresent() {
    if (g_live.load() != Live::Idle) return;
    g_live = Live::Searching;
    std::thread(FindManager).detach();
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    const int api = static_cast<int>(sc->get_device()->get_api());
    if (g_api.exchange(api) != api) Log("presenting on %s", ApiName());
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                          : g_colorSpace == color_space::extended_srgb_linear ? "scRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::lock_guard<std::recursive_mutex> lock(g_state);
    bool down = focused && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(focused);
    RayTracingOnPresent();
    LiveOnPresent();
    HdrOnPresent(sc);
    HwRtGuardOnPresent();
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

// The game's value of <name> as text: the one we saved if we're holding it, else the current
// (the renderer's copy, read from this thread).
std::string GameValue(const View& view, const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = view.held.find(name);
    if (it != view.held.end()) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

std::string ChoiceLabel(const View& view, const Option& o, const Choice& c) {
    if (c.id != 0) return c.label;
    std::string v = GameValue(view, o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

bool DrawOption(const View& view, Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(view, o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(view, o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

bool DrawTab(const View& view, const char* tab) {
    bool changed = false;
    if (!strcmp(tab, kRayTracingTab)) {
        if (g_rayTracing.load() == 0)
            ImGui::TextColored(kWarn, "The game started without ray tracing (r.RayTracing = 0): these do nothing.");
        // Lumen traces mostly with inline ray queries, so acceleration structure builds (every
        // frame while ray tracing runs) are the reliable sign, not DispatchRays.
        ImGui::TextDisabled("Ray tracing on the GPU now: %s (%llu structure builds per second)",
                            g_asPerSec ? "yes" : "no", g_asPerSec);
    }
    if (!strcmp(tab, kHdrTab)) {
        ImGui::TextDisabled("Swap chain: %s", g_colorSpace == color_space::hdr10_pq ? "HDR10" : "SDR (sRGB)");
        ImGui::TextDisabled("HDR output: %s", g_hdrStatus.c_str());
    }
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(view, o);
    }
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

// (0.3.1 also wrote the peak to GameUserSettings.ini HDRDisplayOutputNits; the HDR sink kept asking
// for 1000 with 3700 there, so that was dropped.)

// Brightness sliders for the HDR tab; true if one changed.
bool DrawHdrSliders() {
    const Option* o = FindOption("Hdr");
    if (!o || !o->value) return false;
    bool changed = false;
    ImGui::SeparatorText("Brightness");
    ChangedMark(true);
    ImGui::SetNextItemWidth(ControlWidth());
    changed |= ImGui::SliderInt("Peak brightness", &g_hdrPeak, 400, 10000, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    Help("The brightest highlight the tonemapper aims for. Set it to your display's peak\n"
         "brightness (for example from Windows' HDR calibration app).");
    bool ui = g_hdrUi != 0;
    ChangedMark(ui);
    if (ImGui::Checkbox("Set UI brightness (game: 300 nits)", &ui)) {
        g_hdrUi = ui ? 300 : 0;
        changed = true;
    }
    if (ui) {
        ImGui::TextUnformatted(" ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ControlWidth());
        changed |= ImGui::SliderInt("UI brightness", &g_hdrUi, 80, 1000, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    }
    bool mid = g_hdrMid != 0;
    ChangedMark(mid);
    if (ImGui::Checkbox("Set mid-grey brightness (game: 15 nits)", &mid)) {
        g_hdrMid = mid ? 15 : 0;
        changed = true;
    }
    Help("How bright mid-tones are: higher makes the whole picture brighter without moving\n"
         "the highlights.");
    if (mid) {
        ImGui::TextUnformatted(" ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ControlWidth());
        changed |= ImGui::SliderInt("Mid-grey", &g_hdrMid, 5, 50, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    }
    return changed;
}

char g_cvarName[128] = "r.RayTracing.Enable";
char g_cvarValue[64] = "";
std::string g_cvarResult;

void DrawAdvanced(const View& view) {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Game thread: %s, %llu frames", g_hooked ? "reached" : "not reached",
                        static_cast<unsigned long long>(g_ticks.load()));
    ImGui::TextDisabled("Holding %zu settings; put back %d times after they changed", view.held.size(),
                        g_reapplied.load());
    if (!view.rejected.empty()) {
        std::string list;
        for (const std::string& k : view.rejected) list += (list.empty() ? "" : ", ") + k;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Graphics API: %s; r.RayTracing = %d", ApiName(), g_rayTracing.load());
    ImGui::TextDisabled("HDR: %s (swap chain %s)", g_hdrStatus.c_str(),
                        g_colorSpace == color_space::hdr10_pq ? "HDR10" : g_colorSpace == color_space::srgb ? "sRGB" : "other");
    ImGui::TextDisabled("Hardware ray tracing: %s (%llu structure builds, %llu ray dispatches per second)",
                        g_asPerSec || g_raysPerSec ? "active" : "not in use", g_asPerSec, g_raysPerSec);
    ImGui::TextDisabled("Log: " DT_PRODUCT ".log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Probe");
    if (ImGui::Button("Write all settings to file")) g_pendingProbe = "manual";
    Help("Writes the game's value of every lighting, shadow, ray tracing, detail and\n"
         "post-processing setting to " DT_PRODUCT "-probe.txt next to the game's exe.\n"
         "Do it in a scene (not a menu) for the values the game really plays with.");
    if (!view.probeResult.empty()) ImGui::TextDisabled("%s", view.probeResult.c_str());

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s (set by 0x%02x)%s", cvars::TypeName(v.type),
                            Read(name, value) ? Narrow(value).c_str() : "?", cvars::Priority(v.var),
                            view.held.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        g_pendingConsole.push_back({name, val});  // set by the game thread at its next frame
        g_cvarResult = "Sent (see the log; lasts until an option changes it or the game restarts)";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value at the game's next frame, at console priority.\n"
         "It isn't saved.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard<std::recursive_mutex> lock(g_state);  // the game thread skips a frame meanwhile
    const View view = CopyView();
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    ImGui::Spacing();
    ImGui::BeginDisabled(measuring || live != Live::Ready);
    const int current = CurrentPreset();
    ChangedMark(current != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                ApplyPreset(i);
                changed = true;
                Log("preset: %s", kPresets[i].label);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the quality options together. Measure preset costs checks them on your PC.\n"
         "Changing an option afterwards shows Custom.");
    ImGui::SameLine();
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (measuring) {
        ImGui::TextColored(kAccent, "Measuring %s (%zu of %zu)... keep the camera still.",
                           g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1, g_measure.stages.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
    } else {
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
        ImGui::EndDisabled();
        Help("Runs each preset for 14 s (6 s to settle, 8 s measured) and shows how much longer\n"
             "each frame takes than with Game. Stand still in a typical scene and leave the mouse\n"
             "alone. Your options come back afterwards.");
        ImGui::SameLine();
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
        ImGui::EndDisabled();
        Help("Runs every quality option on its own at each of its choices (11 s each, several\n"
             "minutes in all; HDR isn't measured, Ray tracing only while it runs)\n"
             "and writes how much each one costs to " DT_PRODUCT ".log, costliest first.");
        if (g_measure.done) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextDisabled("%s", g_measure.summary.c_str());
            ImGui::PopTextWrapPos();
        } else if (!g_measure.note.empty()) {
            ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        std::vector<const char*> tabs;  // in the order the options list them
        for (const Option& o : g_options)
            if (std::none_of(tabs.begin(), tabs.end(), [&](const char* t) { return !strcmp(t, o.tab); }))
                tabs.push_back(o.tab);
        for (const char* tab : tabs)
            if (ImGui::BeginTabItem(tab)) {
                changed |= DrawTab(view, tab);
                if (!strcmp(tab, kHdrTab)) changed |= DrawHdrSliders();
                ImGui::EndTabItem();
            }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced(view);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::build_acceleration_structure>(&OnBuildAs);
            reshade::register_event<reshade::addon_event::dispatch_rays>(&OnDispatchRays);
            reshade::register_overlay(DT_PRODUCT, &DrawOverlay);
            break;
        case DLL_PROCESS_DETACH:
            if (g_hooked) cvars::UnhookSinks(g_manager);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
