#include "asset_forge/paper_sprite_forge.h"

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
#include <glaze/glaze.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

struct PaperSpriteDumpJson {
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

std::string to_ascii(StringType const& text) {
    std::string out;
    out.reserve(text.size());
    for (auto c : text) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
}

void log_object(const char* tag, UObject* obj) noexcept {
    char msg[768];
    if (!obj) {
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] %s ptr=null\n", tag ? tag : "object");
        log_line(msg);
        return;
    }

    std::string full;
    try { full = to_ascii(obj->GetFullName()); } catch (...) { full = "<GetFullName failed>"; }
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] %s ptr=%p full=%s\n",
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
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] property missing: %ls\n", property_name);
        log_line(msg);
        return false;
    }

    const std::wstring wide = utf8_to_utf16(text);
    const wchar_t* end = prop->ImportText_InContainer(wide.c_str(), object, object, PPF_None, nullptr);
    if (!end) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] property import failed: %ls\n", property_name);
        log_line(msg);
        return false;
    }
    return true;
}

bool import_if_present(UObject* object, const PaperSpriteDumpJson& dump, const char* json_key, const wchar_t* property_name) noexcept {
    auto it = dump.Values.find(json_key);
    if (it == dump.Values.end()) return true;
    return import_text_property(object, property_name, it->second);
}

std::wstring object_path_from_export_text(const std::string& text) noexcept {
    const size_t space = text.find(' ');
    std::string path = (space == std::string::npos) ? text : text.substr(space + 1);
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
        path = path.substr(1, path.size() - 2);
    }
    return utf8_to_utf16(path);
}

UObject* find_target_object(const UObjectCopyForgeRequest& request) noexcept {
    if (!valid_request(request)) return nullptr;
    const std::wstring path = make_object_path(request);
    return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
}

UObject* static_find_object_safe(const wchar_t* path) noexcept {
    if (!path || !path[0]) return nullptr;
    __try {
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

UObject* find_object_by_export_text(const std::string& text) noexcept {
    const std::wstring path = object_path_from_export_text(text);
    if (path.empty()) return nullptr;
    return static_find_object_safe(path.c_str());
}

void* read_pointer_field(UObject* object, int32_t offset) noexcept {
    if (!object || offset < 0) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool write_pointer_field(UObject* object, int32_t offset, void* value) noexcept {
    if (!object || offset < 0) return false;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool set_object_pointer_from_export_text(UObject* object, const PaperSpriteDumpJson& dump, const char* json_key, const wchar_t* property_name) noexcept {
    auto it = dump.Values.find(json_key);
    if (it == dump.Values.end()) return true;

    FProperty* prop = find_property_by_name(object->GetClassPrivate(), property_name);
    if (!prop) return false;

    UObject* value = find_object_by_export_text(it->second);
    if (!valid_object(value)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] object pointer resolve skipped: %s\n", it->second.c_str());
        log_line(msg);
        return true;
    }

    if (!write_pointer_field(object, prop->GetOffset_Internal(), value)) {
        log_line("[Lilac2LilyMod][paper_sprite_forge] object pointer write failed\n");
        return false;
    }

    char msg[512];
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] object pointer set %ls=%p\n", property_name, value);
    log_line(msg);
    return true;
}

} // namespace

bool set_paper_sprite_from_json(const UObjectCopyForgeRequest& request, const wchar_t* json_path) noexcept {
    try {
        if (!valid_request(request) || !json_path || !json_path[0]) {
            log_line("[Lilac2LilyMod][paper_sprite_forge] skipped: request/json path incomplete\n");
            return false;
        }

        UObject* object = find_target_object(request);
        if (!valid_object(object)) {
            log_line("[Lilac2LilyMod][paper_sprite_forge] skipped: target unavailable\n");
            return false;
        }
        if (!class_name_is(object, L"PaperSprite")) {
            log_line("[Lilac2LilyMod][paper_sprite_forge] skipped: target is not PaperSprite\n");
            return false;
        }

        std::string buf = slurp(std::filesystem::path{json_path});
        if (buf.empty()) {
            log_line("[Lilac2LilyMod][paper_sprite_forge] failed: JSON missing or empty\n");
            return false;
        }

        PaperSpriteDumpJson dump{};
        if (auto err = glz::read_json(dump, buf); err) {
            const std::string detail = glz::format_error(err, buf);
            char msg[768];
            std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][paper_sprite_forge] failed: JSON parse failed: %.640s\n", detail.c_str());
            log_line(msg);
            return false;
        }

        bool ok = true;
        ok = set_object_pointer_from_export_text(object, dump, "BakedSourceTexture", L"BakedSourceTexture") && ok;
        ok = import_if_present(object, dump, "BakedRenderData", L"BakedRenderData") && ok;
        ok = import_if_present(object, dump, "BakedSourceDimension", L"BakedSourceDimension") && ok;
        ok = import_if_present(object, dump, "BakedSourceUV", L"BakedSourceUV") && ok;
        ok = import_if_present(object, dump, "DefaultMaterial", L"DefaultMaterial") && ok;
        ok = import_if_present(object, dump, "AlternateMaterial", L"AlternateMaterial") && ok;
        ok = import_if_present(object, dump, "AlternateMaterialSplitIndex", L"AlternateMaterialSplitIndex") && ok;
        ok = import_if_present(object, dump, "PixelsPerUnrealUnit", L"PixelsPerUnrealUnit") && ok;
        ok = import_if_present(object, dump, "BodySetup", L"BodySetup") && ok;
        ok = import_if_present(object, dump, "SpriteCollisionDomain", L"SpriteCollisionDomain") && ok;
        ok = import_if_present(object, dump, "Sockets", L"Sockets") && ok;
        ok = import_if_present(object, dump, "AdditionalSourceTextures", L"AdditionalSourceTextures") && ok;

        if (ok) log_object("values imported", object);
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][paper_sprite_forge] failed\n");
        return false;
    }
}

bool asset_forge_add_paper_sprite(const UObjectCopyForgeRequest& request, const wchar_t* json_path) noexcept {
    if (!uobject_copy_forge_add(request)) {
        log_line("[Lilac2LilyMod][paper_sprite_forge] copy construction failed\n");
        return false;
    }
    return set_paper_sprite_from_json(request, json_path);
}
