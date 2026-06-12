#include "spine.h"

#include "lifecycle.h"
#include "rt_config.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/Hooks/Hooks.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UActorComponent.hpp>
#include <Unreal/AActor.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/Core/HAL/UnrealMemory.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/UnrealCoreStructs.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <glaze/glaze.hpp>
#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct UE_FString {
    wchar_t* Data;
    int32_t ArrayNum;
    int32_t ArrayMax;
};
static_assert(sizeof(UE_FString) == 16, "UE_FString must be 16 bytes");

using namespace RC;
using namespace RC::Unreal;

constexpr int kVisualWriteDelayFrames = 8;

using ReplaceSpineDataFn = void(__fastcall*)(void*, void*, void*, void*);
using ComponentSetAnimationFn = void*(__fastcall*)(void*, int, UE_FString*, bool);
using ComponentAddAnimationFn = void*(__fastcall*)(void*, int, UE_FString*, bool, float);
using WidgetSetAnimationFn = void*(__fastcall*)(void*, int, UE_FString*, bool);
using WidgetAddAnimationFn = void*(__fastcall*)(void*, int, UE_FString*, bool, float);

std::unordered_map<std::string, std::string> g_anim_map;
std::atomic<bool> g_anim_map_loaded{false};

std::atomic<bool> g_replace_created{false};
std::atomic<bool> g_replace_enabled{false};
void* g_replace_target = nullptr;
ReplaceSpineDataFn g_replace_original = nullptr;

std::atomic<bool> g_component_created{false};
std::atomic<bool> g_component_enabled{false};
void* g_component_set_target = nullptr;
void* g_component_add_target = nullptr;
ComponentSetAnimationFn g_component_set_original = nullptr;
ComponentAddAnimationFn g_component_add_original = nullptr;

std::atomic<bool> g_widget_created{false};
std::atomic<bool> g_widget_enabled{false};
void* g_widget_set_target = nullptr;
void* g_widget_add_target = nullptr;
WidgetSetAnimationFn g_widget_set_original = nullptr;
WidgetAddAnimationFn g_widget_add_original = nullptr;

std::atomic<bool> g_tick_installed{false};
std::atomic<bool> g_visual_write_pending{false};
std::atomic<int> g_visual_write_frames{0};
Hook::GlobalCallbackId g_tick_callback = Hook::ERROR_ID;
double g_visual_x = 1.0;
double g_visual_y = 1.0;
double g_visual_z = 1.0;
const wchar_t* g_visual_tag = L"";

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

void logf(const char* fmt, ...) noexcept {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) log_line(buf);
}

std::string slurp(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    } catch (...) {
        return {};
    }
}

size_t load_anim_map(const wchar_t* mod_dll_dir) noexcept {
    g_anim_map_loaded.store(false, std::memory_order_release);
    g_anim_map.clear();
    if (!mod_dll_dir) return 0;

    std::string buf = slurp(std::filesystem::path{mod_dll_dir} / L"config" / L"Lilac2LilyAnimationMap.json");
    if (buf.empty()) {
        log_line("[Lilac2LilyMod][spine] animation map missing\n");
        return 0;
    }

    if (auto err = glz::read_json(g_anim_map, buf); err) {
        g_anim_map.clear();
        log_line("[Lilac2LilyMod][spine] animation map parse failed\n");
        return 0;
    }

    g_anim_map_loaded.store(true, std::memory_order_release);
    logf("[Lilac2LilyMod][spine] animation map loaded entries=%zu\n", g_anim_map.size());
    return g_anim_map.size();
}

const char* map_animation(const char* src_name) noexcept {
    if (!g_anim_map_loaded.load(std::memory_order_acquire) || !src_name) return nullptr;
    auto it = g_anim_map.find(src_name);
    return it == g_anim_map.end() ? nullptr : it->second.c_str();
}

bool text_contains(StringType const& text, const wchar_t* needle) {
    return needle && text.find(StringType{needle}) != StringType::npos;
}

bool object_name_contains(UObject* obj, const wchar_t* marker) {
    return obj && text_contains(obj->GetFullName(), marker);
}

UObject* as_uobject(void* p) noexcept {
    auto addr = reinterpret_cast<uintptr_t>(p);
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFFULL) return nullptr;
    return reinterpret_cast<UObject*>(p);
}

bool is_lily_skeleton(void* skel) noexcept {
    __try {
        return object_name_contains(as_uobject(skel), rt_path("names", "LilyAssetMarker"));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool name_contains_default(StringType const& full) {
    static const wchar_t marker[] = L"Default__";
    constexpr size_t len = 9;
    if (full.size() < len) return false;
    for (size_t i = 0; i + len <= full.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < len; ++j) {
            if (full[i + j] != static_cast<CharType>(marker[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

UObject* find_lilac_actor() {
    std::vector<UObject*> objs;
    UObjectGlobals::FindAllOf(StringViewType{rt_path("classes", "LilacActorClassName")}, objs);
    for (UObject* obj : objs) {
        if (obj && !name_contains_default(obj->GetFullName())) return obj;
    }
    return nullptr;
}

bool class_name_is_lilac(UObject* obj) noexcept {
    if (!obj) return false;
    UClass* cls = obj->GetClassPrivate();
    return cls && cls->GetName() == StringType{rt_path("classes", "LilacActorClassName")};
}

bool is_lilac_component(void* component) noexcept {
    __try {
        UObject* obj = as_uobject(component);
        if (!obj) return false;
        auto* comp = static_cast<UActorComponent*>(obj);
        AActor* owner = comp->GetTypedOuter<AActor>();
        return class_name_is_lilac(owner);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool fstring_to_ascii(const UE_FString* fs, char* out, size_t cap) noexcept {
    if (!fs || !out || cap == 0) return false;
    out[0] = 0;
    __try {
        if (fs->ArrayNum <= 0 || !fs->Data) return false;
        size_t n = static_cast<size_t>(fs->ArrayNum);
        if (n >= cap) n = cap - 1;
        size_t w = 0;
        for (size_t i = 0; i + 1 < n; ++i) {
            wchar_t wc = fs->Data[i];
            if (wc == 0) break;
            if (wc >= 0x80) return false;
            out[w++] = static_cast<char>(wc);
        }
        out[w] = 0;
        return w > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool write_fstring_ascii(UE_FString* name, const char* text) noexcept {
    if (!name || !text) return false;
    size_t len = std::strlen(text);
    int32_t need = static_cast<int32_t>(len + 1);
    __try {
        wchar_t* data = name->Data;
        if (need > name->ArrayMax) {
            void* new_data = FMemory::Realloc(data, static_cast<size_t>(need) * sizeof(wchar_t), alignof(wchar_t));
            if (!new_data) return false;
            data = static_cast<wchar_t*>(new_data);
            name->Data = data;
            name->ArrayMax = need;
        }
        for (size_t i = 0; i < len; ++i) {
            data[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
        }
        data[len] = L'\0';
        name->ArrayNum = need;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool map_fstring_animation(UE_FString* name, const char* log_tag) noexcept {
    char src[128];
    if (!fstring_to_ascii(name, src, sizeof(src))) return false;
    const char* dst = map_animation(src);
    if (!dst || std::strcmp(src, dst) == 0) return false;
    if (!write_fstring_ascii(name, dst)) return false;
    logf("[Lilac2LilyMod][%s][animap] %s -> %s\n", log_tag ? log_tag : "spine", src, dst);
    return true;
}

std::string to_ascii(StringType const& text) {
    std::string out;
    out.reserve(text.size());
    for (auto c : text) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
}

UObject* read_object_property(UObject* obj, const wchar_t* name) noexcept {
    if (!obj || !name) return nullptr;
    __try {
        UObject** slot = obj->GetValuePtrByPropertyNameInChain<UObject*>(name);
        return slot ? *slot : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool widget_is_lily(void* self) noexcept {
    try {
        UObject* obj = as_uobject(self);
        if (!obj) return false;
        UObject* atlas = read_object_property(obj, STR("Atlas"));
        if (!atlas) return false;

        std::string atlas_name = to_ascii(atlas->GetFullName());
        constexpr char marker[] = "Characters/";
        size_t begin = atlas_name.find(marker);
        if (begin == std::string::npos) return false;
        begin += sizeof(marker) - 1;
        size_t end = atlas_name.find('/', begin);
        if (end == std::string::npos || end <= begin) return false;
        return atlas_name.compare(begin, end - begin, "p0101_Lily") == 0;
    } catch (...) {
        return false;
    }
}

bool configure_lily_skeleton(void* skel_ptr) noexcept {
    auto* skel = as_uobject(skel_ptr);
    if (!skel) return false;
    if (auto* p = skel->GetValuePtrByPropertyNameInChain<float>(STR("ScaleFactor"))) *p = 0.26f;
    if (auto* p = skel->GetValuePtrByPropertyNameInChain<float>(STR("DefaultMix"))) *p = 0.1f;

    auto* skins = skel->GetValuePtrByPropertyNameInChain<TArray<FString>>(STR("DefaultSkins"));
    if (!skins) return false;
    skins->Empty(0);
    skins->Add(FString{STR("_common")});
    skins->Add(FString{STR("_Meat_Head_0")});
    skins->Add(FString{STR("p0101_Lily")});
    return true;
}

bool write_visual_scale(double x, double y, double z, const wchar_t* tag) {
    UObject* actor = find_lilac_actor();
    if (!actor) return false;
    auto* comp_slot = actor->GetValuePtrByPropertyNameInChain<UObject*>(STR("VisualPivotSceneComponent"));
    if (!comp_slot || !*comp_slot) return false;
    auto* scale = (*comp_slot)->GetValuePtrByPropertyNameInChain<FVector>(STR("RelativeScale3D"));
    if (!scale) return false;
    scale->SetX(x);
    scale->SetY(y);
    scale->SetZ(z);
    Output::send(STR("[Lilac2LilyMod][visual] scale ({},{},{}) [{}]\n"), x, y, z, StringType{tag ? tag : L""});
    return true;
}

void disable_secondary_hooks() noexcept;

void tick_visual_write() noexcept {
    if (rt_is_shutting_down() || !g_visual_write_pending.load(std::memory_order_acquire)) return;
    int remaining = g_visual_write_frames.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining > 0) return;
    g_visual_write_pending.store(false, std::memory_order_release);
    try {
        write_visual_scale(g_visual_x, g_visual_y, g_visual_z, g_visual_tag);
    } catch (...) {
        log_line("[Lilac2LilyMod][visual] deferred scale write failed\n");
    }
}

void uninstall_tick_callback() noexcept {
    Hook::GlobalCallbackId id = g_tick_callback;
    g_tick_callback = Hook::ERROR_ID;
    if (id != Hook::ERROR_ID) {
        try { Hook::UnregisterCallback(id); } catch (...) {}
    }
    g_tick_installed.store(false, std::memory_order_release);
}

void install_tick_callback() noexcept {
    if (g_tick_installed.exchange(true, std::memory_order_acq_rel)) return;
    Hook::FCallbackOptions opts{};
    opts.bOnce = false;
    opts.OwnerModName = STR("Lilac2LilyMod");
    opts.HookName = STR("VisualPivotScaleWrite");
    g_tick_callback = Hook::RegisterEngineTickPreCallback(
        [](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) {
            tick_visual_write();
        },
        opts);
    if (g_tick_callback == Hook::ERROR_ID) {
        g_tick_installed.store(false, std::memory_order_release);
    }
}

void schedule_visual_scale(double x, double y, double z, const wchar_t* tag) noexcept {
    install_tick_callback();
    g_visual_x = x;
    g_visual_y = y;
    g_visual_z = z;
    g_visual_tag = tag ? tag : L"";
    g_visual_write_frames.store(kVisualWriteDelayFrames, std::memory_order_release);
    g_visual_write_pending.store(true, std::memory_order_release);
}

bool seed_idle_animation() noexcept {
    if (!IsInGameThread()) return false;
    UObject* actor = find_lilac_actor();
    if (!actor) return false;
    auto* comp_slot = actor->GetValuePtrByPropertyNameInChain<UObject*>(STR("SpineAnimationComponent"));
    if (!comp_slot || !*comp_slot) return false;

    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "SpineSkeletonAnimationComponent_SetAnimation"));
    if (!fn) return false;

    std::vector<uint8_t> params(fn->GetParmsSize(), 0);
    int index = 0;
    for (FProperty* prop : fn->ForEachProperty()) {
        if (!prop) continue;
        uint64 flags = prop->GetPropertyFlags();
        if (!(flags & CPF_Parm) || (flags & CPF_ReturnParm)) continue;
        uint8_t* slot = params.data() + prop->GetOffset_Internal();
        if (index == 0) *reinterpret_cast<int32_t*>(slot) = 0;
        else if (index == 1) *reinterpret_cast<FString*>(slot) = FString{STR("idle")};
        else if (index == 2) *reinterpret_cast<bool*>(slot) = true;
        ++index;
    }
    (*comp_slot)->ProcessEvent(fn, params.data());
    return true;
}

void* __fastcall component_set_detour(void* self, int track, UE_FString* name, bool loop) {
    (void)track;
    (void)loop;
    if (!rt_is_shutting_down() && is_lilac_component(self)) {
        map_fstring_animation(name, "Component.SetAnimation");
    }
    return g_component_set_original ? g_component_set_original(self, track, name, loop) : nullptr;
}

void* __fastcall component_add_detour(void* self, int track, UE_FString* name, bool loop, float delay) {
    (void)track;
    (void)loop;
    (void)delay;
    if (!rt_is_shutting_down() && is_lilac_component(self)) {
        map_fstring_animation(name, "Component.AddAnimation");
    }
    return g_component_add_original ? g_component_add_original(self, track, name, loop, delay) : nullptr;
}

void* __fastcall widget_set_detour(void* self, int track, UE_FString* name, bool loop) {
    (void)track;
    (void)loop;
    if (!rt_is_shutting_down() && widget_is_lily(self)) {
        map_fstring_animation(name, "Widget.SetAnimation");
    }
    return g_widget_set_original ? g_widget_set_original(self, track, name, loop) : nullptr;
}

void* __fastcall widget_add_detour(void* self, int track, UE_FString* name, bool loop, float delay) {
    (void)track;
    (void)loop;
    (void)delay;
    if (!rt_is_shutting_down() && widget_is_lily(self)) {
        map_fstring_animation(name, "Widget.AddAnimation");
    }
    return g_widget_add_original ? g_widget_add_original(self, track, name, loop, delay) : nullptr;
}

bool create_component_hooks() noexcept {
    if (g_component_created.load(std::memory_order_acquire)) return true;
    HMODULE mod = GetModuleHandleW(nullptr);
    uintptr_t rva_set = rt_rva("SpineSkeletonAnimationComponent_SetAnimation");
    uintptr_t rva_add = rt_rva("SpineSkeletonAnimationComponent_AddAnimation");
    if (!mod || !rva_set || !rva_add) return false;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;

    g_component_set_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva_set);
    g_component_add_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva_add);
    st = MH_CreateHook(g_component_set_target, reinterpret_cast<void*>(&component_set_detour), reinterpret_cast<void**>(&g_component_set_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_CreateHook(g_component_add_target, reinterpret_cast<void*>(&component_add_detour), reinterpret_cast<void**>(&g_component_add_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;

    g_component_created.store(true, std::memory_order_release);
    return true;
}

bool create_widget_hooks() noexcept {
    if (g_widget_created.load(std::memory_order_acquire)) return true;
    HMODULE mod = GetModuleHandleW(nullptr);
    uintptr_t rva_set = rt_rva("SpineWidget_SetAnimation");
    uintptr_t rva_add = rt_rva("SpineWidget_AddAnimation");
    if (!mod || !rva_set || !rva_add) return false;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;

    g_widget_set_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva_set);
    g_widget_add_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva_add);
    st = MH_CreateHook(g_widget_set_target, reinterpret_cast<void*>(&widget_set_detour), reinterpret_cast<void**>(&g_widget_set_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;
    st = MH_CreateHook(g_widget_add_target, reinterpret_cast<void*>(&widget_add_detour), reinterpret_cast<void**>(&g_widget_add_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;

    g_widget_created.store(true, std::memory_order_release);
    return true;
}

bool enable_secondary_hooks() noexcept {
    if (rt_is_shutting_down()) return false;
    if (!create_component_hooks() || !create_widget_hooks()) return false;
    bool component_was_enabled = g_component_enabled.exchange(true, std::memory_order_acq_rel);
    bool widget_was_enabled = g_widget_enabled.exchange(true, std::memory_order_acq_rel);
    if (!component_was_enabled) {
        MH_EnableHook(g_component_set_target);
        MH_EnableHook(g_component_add_target);
        log_line("[Lilac2LilyMod][spine] component animap enabled\n");
    }
    if (!widget_was_enabled) {
        MH_EnableHook(g_widget_set_target);
        MH_EnableHook(g_widget_add_target);
        log_line("[Lilac2LilyMod][spine] widget animap enabled\n");
    }
    return true;
}

void disable_secondary_hooks() noexcept {
    if (g_component_enabled.exchange(false, std::memory_order_acq_rel)) {
        if (g_component_set_target) MH_DisableHook(g_component_set_target);
        if (g_component_add_target) MH_DisableHook(g_component_add_target);
        log_line("[Lilac2LilyMod][spine] component animap disabled\n");
    }
    if (g_widget_enabled.exchange(false, std::memory_order_acq_rel)) {
        if (g_widget_set_target) MH_DisableHook(g_widget_set_target);
        if (g_widget_add_target) MH_DisableHook(g_widget_add_target);
        log_line("[Lilac2LilyMod][spine] widget animap disabled\n");
    }
}

void after_replace_spine_data(void* skel) noexcept {
    if (rt_is_shutting_down() || !g_replace_enabled.load(std::memory_order_acquire)) return;
    if (is_lily_skeleton(skel)) {
        configure_lily_skeleton(skel);
        enable_secondary_hooks();
        schedule_visual_scale(-1.0, 1.0, 1.0, L"flip-to-lily");
        seed_idle_animation();
    } else {
        disable_secondary_hooks();
        schedule_visual_scale(1.0, 1.0, 1.0, L"unflip-to-normal");
    }
}

void __fastcall replace_spine_detour(void* self, void* atlas, void* skel, void* notify) {
    (void)atlas;
    bool to_lily = is_lily_skeleton(skel);
    if (to_lily) configure_lily_skeleton(skel);
    if (g_replace_original) g_replace_original(self, atlas, skel, notify);
    after_replace_spine_data(skel);
}

bool create_replace_hook() noexcept {
    if (g_replace_created.load(std::memory_order_acquire)) return true;
    HMODULE mod = GetModuleHandleW(nullptr);
    uintptr_t rva = rt_rva("SpineSkeletonAnimationExComponent_ReplaceSpineData");
    if (!mod || !rva) return false;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;

    g_replace_target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mod) + rva);
    st = MH_CreateHook(g_replace_target, reinterpret_cast<void*>(&replace_spine_detour), reinterpret_cast<void**>(&g_replace_original));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) return false;

    g_replace_created.store(true, std::memory_order_release);
    return true;
}

bool spine_add(const wchar_t* mod_dll_dir) noexcept {
    if (rt_is_shutting_down()) return false;
    try {
        load_anim_map(mod_dll_dir);
        if (!create_replace_hook()) return false;
        rt_add_cleanup(&spine_remove);
        if (!g_replace_enabled.exchange(true, std::memory_order_acq_rel)) {
            MH_EnableHook(g_replace_target);
            log_line("[Lilac2LilyMod][spine] ReplaceSpineData enabled\n");
        }
        return true;
    } catch (...) {
        return false;
    }
}

void spine_remove() noexcept {
    g_visual_write_pending.store(false, std::memory_order_release);
    g_visual_write_frames.store(0, std::memory_order_release);
    disable_secondary_hooks();
    if (g_replace_enabled.exchange(false, std::memory_order_acq_rel) && g_replace_target) {
        MH_DisableHook(g_replace_target);
        log_line("[Lilac2LilyMod][spine] ReplaceSpineData disabled\n");
    }
    uninstall_tick_callback();
}
