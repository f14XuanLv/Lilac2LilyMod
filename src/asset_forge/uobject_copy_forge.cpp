#include "asset_forge/uobject_copy_forge.h"

#include "rt_config.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace RC;
using namespace RC::Unreal;

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

std::string to_ascii(StringType const& text) {
    std::string out;
    out.reserve(text.size());
    for (auto c : text) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
}

void log_object(const char* tag, UObject* obj) noexcept {
    char msg[768];
    if (!obj) {
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][uobject_copy_forge] %s ptr=null\n", tag ? tag : "object");
        log_line(msg);
        return;
    }

    std::string full;
    try { full = to_ascii(obj->GetFullName()); } catch (...) { full = "<GetFullName failed>"; }
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][uobject_copy_forge] %s ptr=%p full=%s\n",
                  tag ? tag : "object", obj, full.c_str());
    log_line(msg);
}

bool valid_object(UObject* object) noexcept {
    return object &&
           !object->IsUnreachable() &&
           !object->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
}

bool valid_request(const UObjectCopyForgeRequest& request) noexcept {
    return request.outer_path && request.outer_path[0] &&
           request.template_object_path && request.template_object_path[0] &&
           request.object_name && request.object_name[0];
}

std::wstring make_object_path(const UObjectCopyForgeRequest& request) {
    std::wstring path{request.outer_path};
    path.push_back(L'.');
    path.append(request.object_name);
    return path;
}

void root_object(UObject* object) noexcept {
    if (!object) return;
    object->SetFlags(static_cast<EObjectFlags>(RF_Public | RF_Standalone | RF_MarkAsRootSet));
    if (!object->IsRootSet()) object->SetRootSet();
}

UObject* static_find_safe(const wchar_t* path, UObject* outer = nullptr) noexcept {
    if (!path) return nullptr;
    __try {
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, outer, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

UObject* scan_object_by_path(const wchar_t* path) noexcept {
    if (!path) return nullptr;
    try {
        const std::string wanted = to_ascii(StringType{path});
        UObject* found = nullptr;
        UObjectGlobals::ForEachUObject([&](UObject* object, int32, int32) {
            if (!object || found) return LoopAction::Continue;
            try {
                if (to_ascii(object->GetPathName()) == wanted) {
                    found = object;
                    return LoopAction::Break;
                }
            } catch (...) {
            }
            return LoopAction::Continue;
        });
        return found;
    } catch (...) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] object scan exception\n");
        return nullptr;
    }
}

UObject* find_existing_object(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) return nullptr;
    const std::wstring path = make_object_path(request);
    return static_find_safe(path.c_str());
}

UObject* find_outer_object(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) return nullptr;
    if (UObject* object = static_find_safe(request.outer_path)) return object;
    return scan_object_by_path(request.outer_path);
}

bool any_nonzero(const uint8_t* data, size_t size) noexcept {
    if (!data) return false;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] != 0) return true;
    }
    return false;
}

FProperty* find_param(UFunction* fn, const wchar_t* name) noexcept {
    if (!fn || !name) return nullptr;
    try {
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated)) {
            if (p && (p->GetPropertyFlags() & CPF_Parm) && p->GetName() == StringType{name}) return p;
        }
    } catch (...) {
    }
    return nullptr;
}

FProperty* find_return_param(UFunction* fn) noexcept {
    if (!fn) return nullptr;
    try {
        for (FProperty* p : TFieldRange<FProperty>(fn, EFieldIterationFlags::IncludeDeprecated)) {
            if (p && (p->GetPropertyFlags() & CPF_ReturnParm)) return p;
        }
    } catch (...) {
    }
    return nullptr;
}

UObject* load_template_object(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) return nullptr;

    if (UObject* object = static_find_safe(request.template_object_path)) return object;

    const wchar_t* helpers_path = rt_path("objects", "AssetRegistryHelpersDefault");
    const wchar_t* registry_path = rt_path("objects", "AssetRegistryDefault");
    const wchar_t* get_asset_registry_path = rt_path("ufunctions", "AssetRegistry_GetAssetRegistry");
    const wchar_t* get_asset_by_path_path = rt_path("ufunctions", "AssetRegistry_GetAssetByObjectPath");
    const wchar_t* get_asset_path = rt_path("ufunctions", "AssetRegistry_GetAsset");
    if (!helpers_path || !registry_path || !get_asset_registry_path || !get_asset_by_path_path || !get_asset_path) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] AssetRegistry config paths unavailable\n");
        return nullptr;
    }

    auto* helpers = static_find_safe(helpers_path);
    auto* get_asset_registry_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, get_asset_registry_path);
    auto* get_asset_by_path_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, get_asset_by_path_path);
    auto* get_asset_fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, get_asset_path);
    if (!helpers || !get_asset_registry_fn || !get_asset_by_path_fn || !get_asset_fn) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] AssetRegistry reflected functions unavailable\n");
        return nullptr;
    }

    struct GetAssetRegistryParams {
        UObject* ObjectPointer = nullptr;
        void* InterfacePointer = nullptr;
    } registry_params{};
    helpers->ProcessEvent(get_asset_registry_fn, &registry_params);
    UObject* registry = registry_params.ObjectPointer;
    if (!registry) registry = static_find_safe(registry_path);
    if (!registry) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] AssetRegistry unavailable\n");
        return nullptr;
    }

    FProperty* object_path_prop = find_param(get_asset_by_path_fn, L"ObjectPath");
    FProperty* include_disk_prop = find_param(get_asset_by_path_fn, L"bIncludeOnlyOnDiskAssets");
    FProperty* asset_data_return_prop = find_return_param(get_asset_by_path_fn);
    FProperty* in_asset_data_prop = find_param(get_asset_fn, L"InAssetData");
    FProperty* object_return_prop = find_return_param(get_asset_fn);
    if (!object_path_prop || !include_disk_prop || !asset_data_return_prop || !in_asset_data_prop || !object_return_prop) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] AssetRegistry parameter reflection failed\n");
        return nullptr;
    }

    const int32_t asset_data_size = asset_data_return_prop->GetSize();
    if (asset_data_size <= 0 || asset_data_size > 1024 || in_asset_data_prop->GetSize() != asset_data_size) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] unexpected FAssetData size\n");
        return nullptr;
    }

    std::vector<uint8_t> path_params(static_cast<size_t>(get_asset_by_path_fn->GetParmsSize()), 0);
    FName object_path_name{StringViewType{request.template_object_path}, FNAME_Add};
    std::memcpy(path_params.data() + object_path_prop->GetOffset_Internal(), &object_path_name, sizeof(FName));
    *reinterpret_cast<uint8_t*>(path_params.data() + include_disk_prop->GetOffset_Internal()) = 1;

    registry->ProcessEvent(get_asset_by_path_fn, path_params.data());
    uint8_t* asset_data = path_params.data() + asset_data_return_prop->GetOffset_Internal();
    if (UObject* object = static_find_safe(request.template_object_path)) return object;
    if (!any_nonzero(asset_data, static_cast<size_t>(asset_data_size))) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] template load failed: no asset data\n");
        return nullptr;
    }

    std::vector<uint8_t> asset_params(static_cast<size_t>(get_asset_fn->GetParmsSize()), 0);
    std::memcpy(asset_params.data() + in_asset_data_prop->GetOffset_Internal(), asset_data, static_cast<size_t>(asset_data_size));
    helpers->ProcessEvent(get_asset_fn, asset_params.data());
    UObject* object = *reinterpret_cast<UObject**>(asset_params.data() + object_return_prop->GetOffset_Internal());
    if (!object) object = static_find_safe(request.template_object_path);
    if (object) log_object("template loaded via AssetRegistry", object);
    else log_line("[Lilac2LilyMod][uobject_copy_forge] template load failed\n");
    return object;
}

} // namespace

bool uobject_copy_forge_add(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] add skipped: request incomplete\n");
        return false;
    }

    UObject* outer = find_outer_object(request);
    if (!valid_object(outer)) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] add skipped: outer unavailable\n");
        return false;
    }

    if (UObject* existing = find_existing_object(request)) {
        if (!valid_object(existing)) {
            log_line("[Lilac2LilyMod][uobject_copy_forge] add skipped: existing object invalid\n");
            return false;
        }
        root_object(existing);
        return true;
    }

    UObject* tmpl = load_template_object(request);
    if (!valid_object(tmpl)) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] add skipped: template unavailable\n");
        return false;
    }

    UClass* cls = tmpl->GetClassPrivate();
    if (!cls) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] add skipped: template class missing\n");
        return false;
    }

    FStaticConstructObjectParameters params{cls};
    params.Outer = outer;
    params.Name = FName{StringViewType{request.object_name}, FNAME_Add};
    params.SetFlags = static_cast<EObjectFlags>(RF_Public | RF_Standalone | RF_MarkAsRootSet);
    params.InternalSetFlags = EInternalObjectFlags::RootSet;
    params.Template = tmpl;

    UObject* obj = UObjectGlobals::StaticConstructObject<UObject*>(params);
    if (!obj) {
        log_line("[Lilac2LilyMod][uobject_copy_forge] StaticConstructObject failed\n");
        return false;
    }

    root_object(obj);
    log_object("object copy-constructed and rooted", obj);
    return true;
}
