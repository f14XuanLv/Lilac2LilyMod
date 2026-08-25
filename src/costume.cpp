#include "costume.h"

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
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/Engine/UDataTable.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/Core/Containers/FString.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <glaze/glaze.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

using namespace RC;
using namespace RC::Unreal;

struct FilterFunction {
    UFunction* fn = nullptr;
    FProperty* handle_prop = nullptr;
    FProperty* return_prop = nullptr;
    int32_t handle_offset = -1;
    int32_t return_offset = -1;
    int32_t instance_handle_offset = -1;
    bool handle_is_ref = false;
    bool handle_is_instance_field = false;
};

std::atomic<bool> g_filter_installed{false};
std::atomic<bool> g_filter_enabled{false};
std::atomic<bool> g_row_injected{false};
Hook::GlobalCallbackId g_filter_callback = Hook::ERROR_ID;
FilterFunction g_can_display{};
FilterFunction g_is_owned{};
uint32_t g_lily_row_cmp_index = 0;

using LocKeyMap = std::unordered_map<std::wstring, std::wstring>;
using LocTable = std::unordered_map<std::wstring, LocKeyMap>;

LocTable g_loc_table;
std::atomic<bool> g_loc_loaded{false};

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

bool game_thread() noexcept {
    return RC::Unreal::IsInGameThread();
}

std::string to_ascii(StringType const& text) {
    std::string out;
    out.reserve(text.size());
    for (auto c : text) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
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

std::wstring utf8_to_utf16(const std::string& s) noexcept {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string ascii_log(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
}

size_t load_text_table(const wchar_t* mod_dll_dir) noexcept {
    g_loc_loaded.store(false, std::memory_order_release);
    g_loc_table.clear();
    if (!mod_dll_dir) {
        log_line("[Lilac2LilyMod][costume] text table load failed: NULL dll dir\n");
        return 0;
    }

    std::filesystem::path json_path = std::filesystem::path{mod_dll_dir} / L"config" / L"DT_ItemCostumes_Extension.json";
    std::string buf = slurp(json_path);
    if (buf.empty()) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][costume] text table missing: %s\n", ascii_log(json_path.wstring()).c_str());
        log_line(msg);
        return 0;
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tmp;
    auto err = glz::read_json(tmp, buf);
    if (err) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][costume] text table JSON parse failed code=%d\n", static_cast<int>(err.ec));
        log_line(msg);
        return 0;
    }

    for (auto& [culture_u8, kv] : tmp) {
        std::wstring culture = utf8_to_utf16(culture_u8);
        if (culture.empty()) continue;
        LocKeyMap& dst = g_loc_table[culture];
        for (auto& [key_u8, value_u8] : kv) {
            std::wstring key = utf8_to_utf16(key_u8);
            std::wstring value = utf8_to_utf16(value_u8);
            if (!key.empty()) dst.emplace(std::move(key), std::move(value));
        }
    }

    g_loc_loaded.store(true, std::memory_order_release);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][costume] text table loaded cultures=%zu\n", g_loc_table.size());
    log_line(msg);
    return g_loc_table.size();
}

bool text_table_loaded() noexcept {
    return g_loc_loaded.load(std::memory_order_acquire);
}

const wchar_t* text_lookup(const wchar_t* culture, const wchar_t* key) noexcept {
    if (!text_table_loaded() || !culture || !key) return nullptr;

    auto find_in_bucket = [&](const std::wstring& bucket) -> const wchar_t* {
        auto it = g_loc_table.find(bucket);
        if (it == g_loc_table.end()) return nullptr;
        auto kit = it->second.find(std::wstring{key});
        return kit == it->second.end() ? nullptr : kit->second.c_str();
    };

    std::wstring current{culture};
    if (auto* value = find_in_bucket(current)) return value;

    while (true) {
        size_t pos = current.find_last_of(L'-');
        if (pos == std::wstring::npos) break;
        current.resize(pos);
        if (auto* value = find_in_bucket(current)) return value;
    }

    static const std::wstring en_us = L"en-US";
    if (current != en_us) {
        if (auto* value = find_in_bucket(en_us)) return value;
    }

    for (auto& [_, kv] : g_loc_table) {
        auto it = kv.find(std::wstring{key});
        if (it != kv.end()) return it->second.c_str();
    }
    return nullptr;
}

UDataTable* find_costume_table() {
    const wchar_t* path = rt_path("objects", "ItemCostumesDataTable");
    return static_cast<UDataTable*>(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path));
}

FName row_name(const char* key, EFindName mode) {
    return FName{StringViewType{rt_path("names", key)}, mode};
}

FProperty* find_property_by_name(UStruct* type, const wchar_t* name) {
    for (FProperty* p : TFieldRange<FProperty>(type, static_cast<EFieldIterationFlags>(
                              static_cast<uint8>(EFieldIterationFlags::IncludeSuper) |
                              static_cast<uint8>(EFieldIterationFlags::IncludeDeprecated)))) {
        if (p && p->GetName() == StringType{name}) return p;
    }
    return nullptr;
}

bool find_bool_out_param(UFunction* fn, FProperty*& prop, int32_t& offset) {
    prop = nullptr;
    offset = -1;
    int count = 0;
    for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated)) {
        if (!p) continue;
        uint64 flags = p->GetPropertyFlags();
        if (!(flags & CPF_Parm)) continue;
        if (!(flags & CPF_OutParm)) continue;
        if (flags & CPF_ReferenceParm) continue;
        StringType cls_name;
        try { cls_name = p->GetClass().GetName(); } catch (...) {}
        if (cls_name == StringType{STR("BoolProperty")} && (flags & CPF_OutParm)) {
            prop = p;
            offset = p->GetOffset_Internal();
            ++count;
        }
    }
    if (count != 1) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][costume] bool out param resolve failed count=%d\n", count);
        log_line(msg);
        prop = nullptr;
        offset = -1;
        return false;
    }
    return true;
}

bool resolve_filter_function(const wchar_t* function_path, const wchar_t* handle_name, FilterFunction& out) {
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path);
    if (!fn) return false;

    FProperty* ret = nullptr;
    int32_t ret_offset = -1;
    if (!find_bool_out_param(fn, ret, ret_offset)) return false;

    FProperty* handle = nullptr;
    int32_t handle_offset = -1;
    bool handle_ref = false;
    for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated)) {
        if (!p) continue;
        uint64 flags = p->GetPropertyFlags();
        if (!(flags & CPF_Parm)) continue;
        if (p->GetName() != StringType{handle_name}) continue;
        handle = p;
        handle_offset = p->GetOffset_Internal();
        handle_ref = (flags & CPF_ReferenceParm) != 0;
        break;
    }
    if (!handle || handle_offset < 0) return false;

    out = {};
    out.fn = fn;
    out.handle_prop = handle;
    out.return_prop = ret;
    out.handle_offset = handle_offset;
    out.return_offset = ret_offset;
    out.handle_is_ref = handle_ref;
    return true;
}

bool resolve_entry_is_owned(FilterFunction& out) {
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "IsCostumeOwned"));
    auto* cls = static_cast<UClass*>(UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, rt_path("classes", "CostumeEntryClass")));
    if (!fn || !cls) return false;

    FProperty* ret = nullptr;
    int32_t ret_offset = -1;
    if (!find_bool_out_param(fn, ret, ret_offset)) return false;

    FProperty* handle = find_property_by_name(cls, L"CostumeHandle");
    if (!handle) return false;

    out = {};
    out.fn = fn;
    out.return_prop = ret;
    out.return_offset = ret_offset;
    out.instance_handle_offset = handle->GetOffset_Internal();
    out.handle_is_instance_field = true;
    return true;
}

void resolve_filter_targets() noexcept {
    if (!g_can_display.fn) {
        (void)resolve_filter_function(rt_path("ufunctions", "CanDisplayCostumeData"), L"Handle", g_can_display);
    }
    if (!g_is_owned.fn) {
        (void)resolve_entry_is_owned(g_is_owned);
    }
}

uint8_t* out_param_addr(FFrame& stack, FProperty* prop) noexcept {
    if (!prop) return nullptr;
    for (FOutParmRec* p = stack.OutParms(); p; p = p->NextOutParm) {
        if (p->Property == prop) return p->PropAddr;
    }
    return nullptr;
}

void force_bool_return(FFrame& stack, void* result, FilterFunction& filter) noexcept {
    uint8_t* ret = out_param_addr(stack, filter.return_prop);
    if (ret) {
        *ret = 1;
    } else if (filter.return_offset >= 0 && stack.Locals()) {
        *(stack.Locals() + filter.return_offset) = 1;
    }
    if (result) *reinterpret_cast<uint8_t*>(result) = 1;
}

void filter_post_callback(UObject* context, FFrame& stack, void* result) noexcept {
    if (rt_is_shutting_down() || !g_filter_enabled.load(std::memory_order_acquire)) return;

    __try {
        UFunction* fn = stack.Node();
        if (!fn) return;
        FilterFunction* filter = nullptr;
        if (fn == g_can_display.fn) filter = &g_can_display;
        else if (fn == g_is_owned.fn) filter = &g_is_owned;
        else return;

        uint8_t* locals = stack.Locals();
        if (!locals) return;

        uint8_t* handle = nullptr;
        if (filter->handle_is_instance_field) {
            if (context && filter->instance_handle_offset >= 0) {
                handle = reinterpret_cast<uint8_t*>(context) + filter->instance_handle_offset;
            }
        } else {
            handle = locals + filter->handle_offset;
            if (filter->handle_is_ref) {
                uint8_t* out = out_param_addr(stack, filter->handle_prop);
                if (out) handle = out;
                else handle = *reinterpret_cast<uint8_t**>(handle);
            }
        }
        if (!handle) return;

        uint32_t cmp_index = 0;
        std::memcpy(&cmp_index, handle + 8, sizeof(cmp_index));
        if (cmp_index == g_lily_row_cmp_index) force_bool_return(stack, result, *filter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void install_filter_callback() {
    if (g_filter_installed.exchange(true, std::memory_order_acq_rel)) return;

    Hook::FCallbackOptions opts{};
    opts.bOnce = false;
    opts.OwnerModName = STR("Lilac2LilyMod");
    opts.HookName = STR("CostumeFilter");
    g_filter_callback = Hook::RegisterProcessLocalScriptFunctionPostCallback(
        [](Hook::TCallbackIterationData<void>&, UObject* context, FFrame& stack, void* result) {
            filter_post_callback(context, stack, result);
        },
        opts);
    if (g_filter_callback == Hook::ERROR_ID) {
        g_filter_installed.store(false, std::memory_order_release);
        return;
    }
}

bool patch_soft_path(uint8_t* field, const wchar_t* package_name, const wchar_t* asset_name) {
    if (!field || !package_name || !asset_name) return false;
    FName package{StringViewType{package_name}, FNAME_Add};
    FName asset{StringViewType{asset_name}, FNAME_Add};
    std::memcpy(field + 8, &package, 8);
    std::memcpy(field + 16, &asset, 8);
    return true;
}

bool current_culture(std::wstring& out) {
    out.clear();
    auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, rt_path("ufunctions", "Kismet_GetCurrentCulture"));
    auto* self = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, rt_path("objects", "KismetInternationalizationLibraryDefault"));
    if (!fn || !self) return false;

    struct Params {
        FString ReturnValue;
    } params{};
    self->ProcessEvent(fn, &params);
    const auto* data = params.ReturnValue.GetCharArray().GetData();
    int32_t count = params.ReturnValue.GetCharArray().Num();
    if (!data || count <= 0) return false;
    while (count > 0 && data[count - 1] == 0) --count;
    out.assign(data, data + count);
    return !out.empty();
}

bool write_text(uint8_t* row, FProperty* prop, const wchar_t* value) {
    if (!row || !prop || !value) return false;
    FText* text = reinterpret_cast<FText*>(row + prop->GetOffset_Internal());
    FText replacement{FString{value}};
    *text = replacement;
    return true;
}

bool remove_lily_row(bool remove_row_api) {
    log_line("[Lilac2LilyMod][costume][revert] begin\n");
    if (!g_row_injected.load(std::memory_order_acquire)) {
        log_line("[Lilac2LilyMod][costume][revert] skip: row not injected\n");
        return true;
    }
    if (!game_thread()) {
        log_line("[Lilac2LilyMod][costume][revert] abort: not game thread\n");
        return false;
    }

    UDataTable* dt = find_costume_table();
    if (!dt) {
        log_line("[Lilac2LilyMod][costume][revert] abort: DT not found\n");
        return false;
    }

    FName lily = row_name("LilyCostumeRow", FNAME_Find);
    uint8_t* row = dt->FindRowUnchecked(lily);
    if (row) {
        if (remove_row_api) {
            log_line("[Lilac2LilyMod][costume][revert] RemoveRow begin\n");
            dt->RemoveRow(lily);
            log_line("[Lilac2LilyMod][costume][revert] RemoveRow end\n");
        } else {
            log_line("[Lilac2LilyMod][costume][revert] RowMap.Remove begin\n");
            dt->GetRowMap().Remove(lily);
            log_line("[Lilac2LilyMod][costume][revert] RowMap.Remove end\n");
        }
    } else {
        log_line("[Lilac2LilyMod][costume][revert] row already missing\n");
    }
    g_row_injected.store(false, std::memory_order_release);
    log_line("[Lilac2LilyMod][costume][revert] success\n");
    return true;
}

bool add_lily_row() noexcept {
    try {
        if (!game_thread()) {
            log_line("[Lilac2LilyMod][costume] row inject failed: not game thread\n");
            return false;
        }
        UDataTable* dt = find_costume_table();
        if (!dt) {
            log_line("[Lilac2LilyMod][costume] row inject failed: DT not found\n");
            return false;
        }

        UScriptStruct* row_struct = dt->GetRowStruct();
        if (!row_struct) {
            log_line("[Lilac2LilyMod][costume] row inject failed: RowStruct missing\n");
            return false;
        }

        FName lily = row_name("LilyCostumeRow", FNAME_Add);
        if (dt->FindRowUnchecked(lily)) {
            log_line("[Lilac2LilyMod][costume] Lily row already exists\n");
            return true;
        }

        uint8_t* template_row = dt->FindRowUnchecked(row_name("LilacCostumeRow", FNAME_Find));
        if (!template_row) {
            log_line("[Lilac2LilyMod][costume] row inject failed: p0000 template missing\n");
            return false;
        }

        FProperty* atlas = find_property_by_name(row_struct, L"AtlasAsset");
        FProperty* skel = find_property_by_name(row_struct, L"SkeletonDataAsset");
        FProperty* icon = find_property_by_name(row_struct, L"Icon");
        if (!atlas || !skel) {
            log_line("[Lilac2LilyMod][costume] row inject failed: asset properties missing\n");
            return false;
        }

        dt->AddRow(lily, template_row, row_struct);
        uint8_t* row = dt->FindRowUnchecked(lily);
        if (!row) {
            log_line("[Lilac2LilyMod][costume] row inject failed: AddRow produced no row\n");
            return false;
        }

        bool ok = true;
        ok = patch_soft_path(row + atlas->GetOffset_Internal(), rt_path("assets", "ForgedLilyAtlasPackage"), rt_path("assets", "ForgedLilyAtlasAsset")) && ok;
        ok = patch_soft_path(row + skel->GetOffset_Internal(), rt_path("assets", "ForgedLilySkeletonPackage"), rt_path("assets", "ForgedLilySkeletonAsset")) && ok;
        if (icon) {
            // Old static mod asset route, kept for quick rollback:
            // ok = patch_soft_path(row + icon->GetOffset_Internal(), rt_path("assets", "LilyIconPackage"), rt_path("assets", "LilyIconAsset")) && ok;
            ok = patch_soft_path(row + icon->GetOffset_Internal(), rt_path("assets", "ForgedLilyIconPackage"), rt_path("assets", "ForgedLilyIconAsset")) && ok;
        }
        if (!ok) {
            dt->RemoveRow(lily);
            log_line("[Lilac2LilyMod][costume] row inject failed: soft paths patch failed\n");
            return false;
        }

        g_row_injected.store(true, std::memory_order_release);
        log_line("[Lilac2LilyMod][costume] Lily row injected\n");
        return true;
    } catch (...) {
        log_line("[Lilac2LilyMod][costume] Lily row injection failed\n");
        return false;
    }
}

bool apply_text_overrides() noexcept {
    try {
        if (!game_thread() || !g_row_injected.load(std::memory_order_acquire)) return false;
        if (!text_table_loaded()) return false;

        UDataTable* dt = find_costume_table();
        if (!dt) return false;
        uint8_t* row = dt->FindRowUnchecked(row_name("LilyCostumeRow", FNAME_Find));
        UScriptStruct* row_struct = dt->GetRowStruct();
        if (!row || !row_struct) return false;

        std::wstring culture;
        if (!current_culture(culture)) {
            log_line("[Lilac2LilyMod][costume][compat] culture fallback en-US used\n");
            culture = L"en-US";
        }

        FProperty* name_prop = find_property_by_name(row_struct, L"Name");
        FProperty* desc_prop = find_property_by_name(row_struct, L"Description");
        const wchar_t* name = text_lookup(culture.c_str(), L"p0101.Name");
        const wchar_t* desc = text_lookup(culture.c_str(), L"p0101.Description");

        bool ok = false;
        ok = write_text(row, name_prop, name) || ok;
        ok = write_text(row, desc_prop, desc) || ok;
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][costume] text culture=%s ok=%d\n", ascii_log(culture).c_str(), ok ? 1 : 0);
        log_line(msg);
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][costume] text override failed\n");
        return false;
    }
}

bool enable_session_filter() noexcept {
    try {
        if (!game_thread()) return false;
        install_filter_callback();

        FName lily = row_name("LilyCostumeRow", FNAME_Add);
        std::memcpy(&g_lily_row_cmp_index, &lily, sizeof(g_lily_row_cmp_index));

        resolve_filter_targets();
        bool can_display = g_can_display.fn != nullptr;
        g_filter_enabled.store(can_display, std::memory_order_release);

        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "[Lilac2LilyMod][costume] filter %s can_display=%d is_owned=%d callback=%d\n",
                      can_display ? "enabled" : "resolve failed",
                      g_can_display.fn ? 1 : 0,
                      g_is_owned.fn ? 1 : 0,
                      g_filter_callback != Hook::ERROR_ID ? 1 : 0);
        log_line(msg);
        return can_display;
    } catch (...) {
        g_filter_enabled.store(false, std::memory_order_release);
        return false;
    }
}

void disable_session_filter() noexcept {
    g_filter_enabled.store(false, std::memory_order_release);
    g_can_display = {};
    g_is_owned = {};

    Hook::GlobalCallbackId id = g_filter_callback;
    g_filter_callback = Hook::ERROR_ID;
    if (id != Hook::ERROR_ID) {
        try { Hook::UnregisterCallback(id); } catch (...) {}
    }
    g_filter_installed.store(false, std::memory_order_release);
    log_line("[Lilac2LilyMod][costume] filter disabled\n");
}

void cleanup_costume_for_exit() noexcept {
    (void)costume_remove(CostumeRemoveMode::ForgetRow);
}

} // namespace

bool costume_add(const wchar_t* mod_dll_dir) noexcept {
    try {
        bool ok = true;
        if (!text_table_loaded()) {
            ok = load_text_table(mod_dll_dir) > 0 && ok;
        }
        ok = add_lily_row() && ok;
        ok = apply_text_overrides() && ok;
        ok = enable_session_filter() && ok;
        rt_add_cleanup(&cleanup_costume_for_exit);
        return ok;
    } catch (...) {
        return false;
    }
}

bool costume_remove(CostumeRemoveMode mode) noexcept {
    try {
        disable_session_filter();
        return remove_lily_row(mode == CostumeRemoveMode::DestroyRow);
    } catch (...) {
        return false;
    }
}
