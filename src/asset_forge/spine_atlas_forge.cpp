#include "asset_forge/spine_atlas_forge.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <glaze/glaze.hpp>
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

struct SpineAtlasDumpJson {
    glz::raw_json Meta;
    std::unordered_map<std::string, std::string> Values;
};

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

bool valid_request(const UObjectCopyForgeRequest& request) noexcept {
    return request.outer_path && request.outer_path[0] &&
           request.template_object_path && request.template_object_path[0] &&
           request.object_name && request.object_name[0];
}

bool valid_object(UObject* object) noexcept {
    return object &&
           !object->IsUnreachable() &&
           !object->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
}

std::wstring make_object_path(const UObjectCopyForgeRequest& request) {
    std::wstring path{request.outer_path};
    path.push_back(L'.');
    path.append(request.object_name);
    return path;
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

std::string utf16_to_utf8(const wchar_t* s) noexcept {
    if (!s || !s[0]) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

UObject* find_target_object(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) return nullptr;
    const std::wstring path = make_object_path(request);
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
}

FProperty* find_property_by_name(UStruct* type, const wchar_t* name) noexcept {
    if (!type || !name) return nullptr;
    try {
        for (FProperty* p : TFieldRange<FProperty>(type, static_cast<EFieldIterationFlags>(
                                  static_cast<uint8>(EFieldIterationFlags::IncludeSuper) |
                                  static_cast<uint8>(EFieldIterationFlags::IncludeDeprecated)))) {
            if (p && p->GetName() == StringType{name}) return p;
        }
    } catch (...) {
    }
    return nullptr;
}

bool class_name_is(UObject* object, const wchar_t* class_name) noexcept {
    if (!object || !class_name) return false;
    try {
        UClass* cls = object->GetClassPrivate();
        return cls && cls->GetName() == StringType{class_name};
    } catch (...) {
        return false;
    }
}

bool import_text_property(UObject* object, const wchar_t* property_name, const std::string& text) noexcept {
    if (!object || !property_name) return false;
    FProperty* prop = find_property_by_name(object->GetClassPrivate(), property_name);
    if (!prop) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_atlas_forge] property missing: %ls\n", property_name);
        log_line(msg);
        return false;
    }

    const std::wstring wide = utf8_to_utf16(text);
    const wchar_t* end = prop->ImportText_InContainer(wide.c_str(), object, object, PPF_None, nullptr);
    if (!end) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_atlas_forge] property import failed: %ls\n", property_name);
        log_line(msg);
        return false;
    }
    return true;
}

bool import_if_present(UObject* object, const SpineAtlasDumpJson& dump, const char* json_key, const wchar_t* property_name) noexcept {
    auto it = dump.Values.find(json_key);
    if (it == dump.Values.end()) return true;
    return import_text_property(object, property_name, it->second);
}

bool clear_native_atlas_cache(UObject* atlas) noexcept {
    if (!atlas) return false;
    FProperty* pages_prop = find_property_by_name(atlas->GetClassPrivate(), L"atlasPages");
    if (!pages_prop) {
        log_line("[Lilac2LilyMod][spine_atlas_forge] atlasPages property missing; native cache not cleared\n");
        return false;
    }

    const int32_t atlas_ptr_offset = pages_prop->GetOffset_Internal() + 16;
    __try {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(atlas) + atlas_ptr_offset);
        void* old = *slot;
        *slot = nullptr;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_atlas_forge] native atlas cache cleared offset=%d old=%p\n",
                      atlas_ptr_offset, old);
        log_line(msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[Lilac2LilyMod][spine_atlas_forge] native atlas cache clear raised exception\n");
        return false;
    }
}

std::string object_export_text(const UObjectCopyForgeRequest& request, const char* class_path) noexcept {
    std::string path = utf16_to_utf8(request.outer_path);
    std::string name = utf16_to_utf8(request.object_name);
    if (path.empty() || name.empty() || !class_path || !class_path[0]) return {};

    std::string out = "(\"";
    out += class_path;
    out += "'";
    out += path;
    out += ".";
    out += name;
    out += "'\")";
    return out;
}

} // namespace

bool asset_forge_add_spine_atlas(const UObjectCopyForgeRequest& request,
                                 const wchar_t* json_path,
                                 const UObjectCopyForgeRequest& texture_request) noexcept {
    try {
        if (!uobject_copy_forge_add(request)) {
            log_line("[Lilac2LilyMod][spine_atlas_forge] copy construction failed\n");
            return false;
        }

        UObject* atlas = find_target_object(request);
        if (!valid_object(atlas)) {
            log_line("[Lilac2LilyMod][spine_atlas_forge] skipped: target unavailable\n");
            return false;
        }
        if (!class_name_is(atlas, L"SpineAtlasAsset")) {
            log_line("[Lilac2LilyMod][spine_atlas_forge] skipped: target is not SpineAtlasAsset\n");
            return false;
        }

        std::string buf = slurp(std::filesystem::path{json_path});
        if (buf.empty()) {
            log_line("[Lilac2LilyMod][spine_atlas_forge] failed: JSON missing or empty\n");
            return false;
        }

        SpineAtlasDumpJson dump{};
        if (auto err = glz::read_json(dump, buf); err) {
            const std::string detail = glz::format_error(err, buf);
            char msg[768];
            std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_atlas_forge] failed: JSON parse failed: %.640s\n", detail.c_str());
            log_line(msg);
            return false;
        }

        bool ok = true;
        ok = import_if_present(atlas, dump, "atlasFileName", L"atlasFileName") && ok;
        ok = import_if_present(atlas, dump, "rawData", L"rawData") && ok;

        const std::string atlas_pages = object_export_text(texture_request, "/Script/Engine.Texture2D");
        if (atlas_pages.empty() || !import_text_property(atlas, L"atlasPages", atlas_pages)) {
            log_line("[Lilac2LilyMod][spine_atlas_forge] atlasPages import failed\n");
            ok = false;
        }

        // ImportText writes rawData directly and does not call USpineAtlasAsset::SetRawData(),
        // so any native Atlas pointer copied from the template or built during a previous run
        // must be discarded before the plugin lazily rebuilds it from the new rawData.
        ok = clear_native_atlas_cache(atlas) && ok;

        if (ok) log_line("[Lilac2LilyMod][spine_atlas_forge] values imported\n");
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][spine_atlas_forge] failed\n");
        return false;
    }
}
