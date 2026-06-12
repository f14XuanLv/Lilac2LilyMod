#include "lifecycle.h"
#include "asset_forge/paper_sprite_forge.h"
#include "asset_forge/spine_atlas_forge.h"
#include "asset_forge/spine_skeleton_data_forge.h"
#include "asset_forge/texture2d_forge.h"
#include "costume.h"
#include "rt_config.h"
#include "rva_check.h"
#include "spine.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <MinHook.h>
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace RC;
using namespace RC::Unreal;

std::atomic<int> g_stage{RT_STAGE_COLD};
std::atomic<bool> g_shutting_down{false};
std::atomic<bool> g_shutdown_cleanups_done{false};
std::mutex g_cleanup_mutex;
std::vector<RtCleanupFn> g_cleanups;
std::wstring g_dll_dir;

std::atomic<bool> g_process_event_installed{false};
Hook::GlobalCallbackId g_process_event_id = Hook::ERROR_ID;
UFunction* g_client_restart = nullptr;
UFunction* g_back_to_title = nullptr;
UFunction* g_on_exit = nullptr;
UFunction* g_title_exit = nullptr;
std::atomic<bool> g_forged_assets_ready{false};

std::atomic<bool> g_window_hook_created{false};
std::atomic<bool> g_window_hook_enabled{false};
void* g_window_target = nullptr;
using WndProcFn = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
WndProcFn g_window_original = nullptr;

UObjectCopyForgeRequest costume_texture_request() noexcept {
    return UObjectCopyForgeRequest{
        rt_path("objects", "ModForgeOuter"),
        rt_path("objects", "CostumeTexture2DTemplate"),
        rt_path("names", "CostumeTexture2DForgeName")
    };
}

UObjectCopyForgeRequest lily_spine_texture_request() noexcept {
    return UObjectCopyForgeRequest{
        rt_path("objects", "ModForgeOuter"),
        rt_path("objects", "LilySpineTexture2DTemplate"),
        rt_path("names", "LilySpineTexture2DForgeName")
    };
}

UObjectCopyForgeRequest lily_spine_atlas_request() noexcept {
    return UObjectCopyForgeRequest{
        rt_path("objects", "ModForgeOuter"),
        rt_path("objects", "LilySpineAtlasTemplate"),
        rt_path("names", "LilySpineAtlasForgeName")
    };
}

UObjectCopyForgeRequest lily_spine_skeleton_data_request() noexcept {
    return UObjectCopyForgeRequest{
        rt_path("objects", "ModForgeOuter"),
        rt_path("objects", "LilySpineSkeletonDataTemplate"),
        rt_path("names", "LilySpineSkeletonDataForgeName")
    };
}

void log_line(const char* msg) noexcept {
    if (!msg) return;
    OutputDebugStringA(msg);
    try {
        StringType wide;
        wide.reserve(256);
        for (const char* p = msg; *p && wide.size() < 1024; ++p) {
            wide.push_back(static_cast<CharType>(static_cast<unsigned char>(*p)));
        }
        Output::send(STR("{}"), wide);
    } catch (...) {
    }
}

const char* stage_name(RtStage stage) noexcept {
    switch (stage) {
        case RT_STAGE_COLD: return "Cold";
        case RT_STAGE_ARMED: return "Armed";
        case RT_STAGE_SESSION: return "Session";
        case RT_STAGE_SHUTDOWN: return "Shutdown";
    }
    return "Unknown";
}

void log_stage(RtStage from, RtStage to, const char* trigger) noexcept {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][stage] %s -> %s (%s)\n",
                  stage_name(from), stage_name(to), trigger ? trigger : "");
    log_line(msg);
}

bool contains_default_object_marker(StringType const& full_name) {
    static const wchar_t marker[] = L"Default__";
    constexpr size_t len = 9;
    if (full_name.size() < len) return false;
    for (size_t i = 0; i + len <= full_name.size(); ++i) {
        bool matched = true;
        for (size_t j = 0; j < len; ++j) {
            if (full_name[i + j] != static_cast<CharType>(marker[j])) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

UObject* find_lilac_actor() {
    std::vector<UObject*> objects;
    UObjectGlobals::FindAllOf(StringViewType{rt_path("classes", "LilacActorClassName")}, objects);
    for (UObject* object : objects) {
        if (object && !contains_default_object_marker(object->GetFullName())) return object;
    }
    return nullptr;
}

void log_session_function_bind(const char* key, UFunction* function, const char* mode) noexcept {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][lifecycle] %s %s ptr=%p\n",
                  key ? key : "", mode ? mode : "", function);
    log_line(msg);
}

void resolve_session_lifecycle_functions() noexcept {
    if (!g_back_to_title) {
        g_back_to_title = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "BackToTitleConfirm"));
        if (g_back_to_title) log_session_function_bind("BackToTitleConfirm", g_back_to_title, "resolved");
    }
    if (!g_on_exit) {
        g_on_exit = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "OnExitConfirm"));
        if (g_on_exit) log_session_function_bind("OnExitConfirm", g_on_exit, "resolved");
    }
}

void resolve_title_lifecycle_functions() noexcept {
    if (!g_title_exit) {
        g_title_exit = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "TitleExitPressed"));
        if (g_title_exit) log_session_function_bind("TitleExitPressed", g_title_exit, "resolved");
    }
}

bool is_title_exit_function(UFunction* function) noexcept {
    if (!function) return false;
    if (g_title_exit && function == g_title_exit) {
        static std::atomic<bool> logged{false};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            log_line("[Lilac2LilyMod][lifecycle] TitleExitPressed matched by pointer\n");
        }
        return true;
    }
    try {
        const wchar_t* configured_path = rt_path("ufunctions", "TitleExitPressed");
        if (!configured_path || !configured_path[0]) return false;
        StringType full_name = function->GetFullName();
        if (full_name.find(StringType{configured_path}) == StringType::npos) return false;

        static std::atomic<bool> logged{false};
        bool expected = false;
        if (logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            log_line("[Lilac2LilyMod][lifecycle] TitleExitPressed matched by configured function path\n");
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ensure_forged_assets() noexcept {
    if (g_forged_assets_ready.load(std::memory_order_acquire)) return true;

    try {
        bool ok = true;
        const std::filesystem::path assets_dir = std::filesystem::path{g_dll_dir} / L"assets";

        const std::filesystem::path texture_json = assets_dir / L"T_Img_Costume_p0101.json";
        const std::filesystem::path texture_png = assets_dir / L"T_Img_Costume_p0101.png";
        if (!asset_forge_add_texture2d(costume_texture_request(), texture_json.c_str(), texture_png.c_str())) {
            log_line("[Lilac2LilyMod][lifecycle] Texture2D forge add failed\n");
            ok = false;
        }
        const std::filesystem::path lily_spine_texture_json = assets_dir / L"p0101_Lily.json";
        const std::filesystem::path lily_spine_texture_png = assets_dir / L"p0101_Lily.png";
        if (!asset_forge_add_texture2d(lily_spine_texture_request(), lily_spine_texture_json.c_str(), lily_spine_texture_png.c_str())) {
            log_line("[Lilac2LilyMod][lifecycle] Lily spine Texture2D forge add failed\n");
            ok = false;
        }
        const std::filesystem::path lily_spine_atlas_json = assets_dir / L"p0101_Lily-atlas.json";
        if (!asset_forge_add_spine_atlas(lily_spine_atlas_request(), lily_spine_atlas_json.c_str(), lily_spine_texture_request())) {
            log_line("[Lilac2LilyMod][lifecycle] Lily SpineAtlasAsset forge add failed\n");
            ok = false;
        }
        const std::filesystem::path lily_spine_skeleton_json = assets_dir / L"p0101_Lily-data.json";
        const std::filesystem::path lily_spine_skeleton_skel = assets_dir / L"p0101_Lily-data.skel";
        if (!asset_forge_add_spine_skeleton_data(lily_spine_skeleton_data_request(), lily_spine_skeleton_json.c_str(), lily_spine_skeleton_skel.c_str())) {
            log_line("[Lilac2LilyMod][lifecycle] Lily SpineSkeletonDataAsset forge add failed\n");
            ok = false;
        }

        UObjectCopyForgeRequest sprite_request{
            rt_path("objects", "ModForgeOuter"),
            rt_path("objects", "CostumePaperSpriteTemplate"),
            rt_path("names", "CostumePaperSpriteForgeName")
        };
        const std::filesystem::path sprite_json = assets_dir / L"SPR_Img_Costume_p0101.json";
        if (!asset_forge_add_paper_sprite(sprite_request, sprite_json.c_str())) {
            log_line("[Lilac2LilyMod][lifecycle] PaperSprite forge add failed\n");
            ok = false;
        }

        if (ok) g_forged_assets_ready.store(true, std::memory_order_release);
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][lifecycle] forged asset initialization failed\n");
        return false;
    }
}

void cleanup_forged_assets_for_exit() noexcept {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (!done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    // Must run from the game exit path, before UE starts destroying rooted forged Texture2D objects.
    log_line("[Lilac2LilyMod][lifecycle] forged asset cleanup begin\n");
    asset_forge_release_texture2d_platform_data(costume_texture_request());
    asset_forge_release_texture2d_platform_data(lily_spine_texture_request());
    log_line("[Lilac2LilyMod][lifecycle] forged asset cleanup end\n");
}

void enter_session_from_client_restart() noexcept {
    try {
        if (!find_lilac_actor()) return;
        if (!ensure_forged_assets()) return;

        int expected = RT_STAGE_ARMED;
        if (!g_stage.compare_exchange_strong(expected, RT_STAGE_SESSION, std::memory_order_acq_rel)) return;

        log_stage(RT_STAGE_ARMED, RT_STAGE_SESSION, "ClientRestart");
        resolve_session_lifecycle_functions();
        costume_add(g_dll_dir.c_str());
        spine_add(g_dll_dir.c_str());

        static std::once_flag rva_check_once;
        std::call_once(rva_check_once, [] { rt_verify_rva_wrappers(); });
    } catch (...) {
        log_line("[Lilac2LilyMod][lifecycle] ClientRestart failed\n");
    }
}

void leave_session_for_title() noexcept {
    try {
        int old = g_stage.exchange(RT_STAGE_ARMED, std::memory_order_acq_rel);
        if (old != RT_STAGE_SESSION) return;

        log_stage(RT_STAGE_SESSION, RT_STAGE_ARMED, "BackToTitleConfirm");
        costume_remove(CostumeRemoveMode::DestroyRow);
        spine_remove();
        g_back_to_title = nullptr;
        g_on_exit = nullptr;
    } catch (...) {
        log_line("[Lilac2LilyMod][lifecycle] BackToTitleConfirm failed\n");
    }
}

void cleanup_for_exit_event(const char* trigger) noexcept {
    try {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][lifecycle] %s\n", trigger ? trigger : "Exit");
        log_line(msg);
        g_shutting_down.store(true, std::memory_order_release);
        spine_remove();
        costume_remove(CostumeRemoveMode::ForgetRow);
        cleanup_forged_assets_for_exit();
    } catch (...) {
        log_line("[Lilac2LilyMod][lifecycle] exit cleanup failed\n");
    }
}

void cleanup_for_exit() noexcept {
    cleanup_for_exit_event("OnExitConfirm");
}

void cleanup_for_title_exit() noexcept {
    cleanup_for_exit_event("TitleExitPressed");
}

void process_event(UFunction* function) {
    if (!function || rt_is_shutting_down()) return;
    resolve_title_lifecycle_functions();
    if (rt_is_in_session() && (!g_back_to_title || !g_on_exit)) {
        resolve_session_lifecycle_functions();
    }

    if (g_client_restart && function == g_client_restart) {
        enter_session_from_client_restart();
    } else if (g_back_to_title && function == g_back_to_title) {
        leave_session_for_title();
    } else if (g_on_exit && function == g_on_exit) {
        cleanup_for_exit();
    } else if (is_title_exit_function(function)) {
        cleanup_for_title_exit();
    }
}

bool install_process_event_hook() noexcept {
    if (g_process_event_installed.exchange(true, std::memory_order_acq_rel)) return true;
    try {
        g_client_restart = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "ClientRestart"));
        g_back_to_title = nullptr;
        g_on_exit = nullptr;
        g_title_exit = nullptr;
        resolve_title_lifecycle_functions();

        Hook::FCallbackOptions opts{};
        opts.bOnce = false;
        opts.OwnerModName = STR("Lilac2LilyMod");
        opts.HookName = STR("LifecycleProcessEvent");
        g_process_event_id = Hook::RegisterProcessEventPreCallback(
            [](Hook::TCallbackIterationData<void>&, UObject*, UFunction* function, void*) {
                try { process_event(function); } catch (...) {}
            },
            opts);
        if (g_process_event_id == Hook::ERROR_ID) {
            g_process_event_installed.store(false, std::memory_order_release);
            return false;
        }
        return true;
    } catch (...) {
        g_process_event_installed.store(false, std::memory_order_release);
        return false;
    }
}

void uninstall_process_event_hook() {
    Hook::GlobalCallbackId id = g_process_event_id;
    g_process_event_id = Hook::ERROR_ID;
    if (id != Hook::ERROR_ID) {
        try { Hook::UnregisterCallback(id); } catch (...) {}
    }
    g_process_event_installed.store(false, std::memory_order_release);
}

void close_window_cleanup_once() noexcept {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        cleanup_for_exit_event("WindowClose");
    }
}

LRESULT CALLBACK window_proc_detour(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if ((msg == WM_SYSCOMMAND && (wparam & 0xFFF0u) == SC_CLOSE) || msg == WM_CLOSE) {
        close_window_cleanup_once();
    }
    if (g_window_original) return g_window_original(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool install_window_close_hook() noexcept {
    if (g_window_hook_created.load(std::memory_order_acquire)) return true;
    HMODULE module = GetModuleHandleW(nullptr);
    uintptr_t rva = rt_rva("UnrealWindow_WndProcThunk");
    if (!module || !rva) return false;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;

    g_window_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(module) + rva);
    st = MH_CreateHook(g_window_target, reinterpret_cast<void*>(&window_proc_detour), reinterpret_cast<void**>(&g_window_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;

    g_window_hook_created.store(true, std::memory_order_release);
    if (!g_window_hook_enabled.exchange(true, std::memory_order_acq_rel)) {
        st = MH_EnableHook(g_window_target);
        if (st != MH_OK && st != MH_ERROR_ENABLED) {
            g_window_hook_enabled.store(false, std::memory_order_release);
            return false;
        }
    }
    return true;
}

void uninstall_window_close_hook() {
    if (g_window_hook_enabled.exchange(false, std::memory_order_acq_rel) && g_window_target) {
        MH_DisableHook(g_window_target);
    }
}

} // namespace

bool rt_on_unreal_init(const wchar_t* dll_dir) noexcept {
    int expected = RT_STAGE_COLD;
    if (!g_stage.compare_exchange_strong(expected, RT_STAGE_ARMED, std::memory_order_acq_rel)) return true;

    if (!rt_config_load(dll_dir)) {
        log_line("[Lilac2LilyMod][lifecycle] config load failed\n");
        return false;
    }
    g_dll_dir = dll_dir;

    log_stage(RT_STAGE_COLD, RT_STAGE_ARMED, "on_unreal_init");
    bool ok = install_process_event_hook();
    if (!install_window_close_hook()) log_line("[Lilac2LilyMod][lifecycle] window close hook failed\n");
    rt_add_cleanup(&uninstall_window_close_hook);
    rt_add_cleanup(&uninstall_process_event_hook);
    rt_add_cleanup(&cleanup_forged_assets_for_exit);
    return ok;
}

void rt_shutdown() noexcept {
    g_shutting_down.store(true, std::memory_order_release);
    bool expected = false;
    if (!g_shutdown_cleanups_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    g_stage.store(RT_STAGE_SHUTDOWN, std::memory_order_release);

    std::vector<RtCleanupFn> cleanups;
    {
        std::lock_guard<std::mutex> lock(g_cleanup_mutex);
        cleanups.swap(g_cleanups);
    }

    log_line("[Lilac2LilyMod][shutdown] begin\n");
    for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
        if (*it) {
            try { (*it)(); } catch (...) {}
        }
    }
    log_line("[Lilac2LilyMod][shutdown] end\n");
}

RtStage rt_stage() noexcept {
    return static_cast<RtStage>(g_stage.load(std::memory_order_acquire));
}

bool rt_is_in_session() noexcept {
    return rt_stage() == RT_STAGE_SESSION;
}

bool rt_is_shutting_down() noexcept {
    return g_shutting_down.load(std::memory_order_acquire);
}

void rt_add_cleanup(RtCleanupFn fn) noexcept {
    if (!fn || rt_is_shutting_down()) return;
    try {
        std::lock_guard<std::mutex> lock(g_cleanup_mutex);
        for (RtCleanupFn existing : g_cleanups) {
            if (existing == fn) return;
        }
        g_cleanups.push_back(fn);
    } catch (...) {
    }
}
