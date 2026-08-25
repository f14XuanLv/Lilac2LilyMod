#include "asset_forge/spine_skeleton_data_forge.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/Core/Containers/Array.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <glaze/glaze.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct SpineSkeletonDataDumpJson {
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

std::vector<uint8_t> slurp_bytes(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return {};
        f.seekg(0, std::ios::end);
        std::streamoff size = f.tellg();
        if (size <= 0 || size > 0x7fffffff) return {};
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> out(static_cast<size_t>(size));
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (!f) return {};
        return out;
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
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_skeleton_data_forge] property missing: %ls\n", property_name);
        log_line(msg);
        return false;
    }

    const std::wstring wide = utf8_to_utf16(text);
    const wchar_t* end = prop->ImportText_InContainer(wide.c_str(), object, object, PPF_None, nullptr);
    if (!end) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_skeleton_data_forge] property import failed: %ls\n", property_name);
        log_line(msg);
        return false;
    }
    return true;
}

bool import_if_present(UObject* object, const SpineSkeletonDataDumpJson& dump, const char* json_key, const wchar_t* property_name) noexcept {
    auto it = dump.Values.find(json_key);
    if (it == dump.Values.end()) return true;
    return import_text_property(object, property_name, it->second);
}

bool set_raw_data_from_skel(UObject* skeleton, const wchar_t* skel_path) noexcept {
    if (!skeleton || !skel_path || !skel_path[0]) return false;

    FProperty* raw_data_prop = find_property_by_name(skeleton->GetClassPrivate(), L"rawData");
    if (!raw_data_prop) {
        log_line("[Lilac2LilyMod][spine_skeleton_data_forge] rawData property missing\n");
        return false;
    }

    std::vector<uint8_t> bytes = slurp_bytes(std::filesystem::path{skel_path});
    if (bytes.empty()) {
        log_line("[Lilac2LilyMod][spine_skeleton_data_forge] .skel missing or empty\n");
        return false;
    }

    auto* raw_data = reinterpret_cast<TArray<uint8_t>*>(reinterpret_cast<uint8_t*>(skeleton) + raw_data_prop->GetOffset_Internal());
    raw_data->Empty(bytes.size());
    raw_data->Append(bytes.data(), static_cast<int32_t>(bytes.size()));

    char msg[192];
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_skeleton_data_forge] rawData loaded bytes=%zu\n", bytes.size());
    log_line(msg);
    return true;
}

} // namespace

bool asset_forge_add_spine_skeleton_data(const UObjectCopyForgeRequest& request,
                                         const wchar_t* json_path,
                                         const wchar_t* skel_path) noexcept {
    try {
        if (!uobject_copy_forge_add(request)) {
            log_line("[Lilac2LilyMod][spine_skeleton_data_forge] copy construction failed\n");
            return false;
        }

        UObject* skeleton = find_target_object(request);
        if (!valid_object(skeleton)) {
            log_line("[Lilac2LilyMod][spine_skeleton_data_forge] skipped: target unavailable\n");
            return false;
        }
        if (!class_name_is(skeleton, L"SpineSkeletonDataAsset")) {
            log_line("[Lilac2LilyMod][spine_skeleton_data_forge] skipped: target is not SpineSkeletonDataAsset\n");
            return false;
        }

        std::string buf = slurp(std::filesystem::path{json_path});
        if (buf.empty()) {
            log_line("[Lilac2LilyMod][spine_skeleton_data_forge] failed: JSON missing or empty\n");
            return false;
        }

        SpineSkeletonDataDumpJson dump{};
        if (auto err = glz::read_json(dump, buf); err) {
            const std::string detail = glz::format_error(err, buf);
            char msg[768];
            std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][spine_skeleton_data_forge] failed: JSON parse failed: %.640s\n", detail.c_str());
            log_line(msg);
            return false;
        }

        bool ok = true;
        ok = import_if_present(skeleton, dump, "BreakSkins", L"BreakSkins") && ok;
        ok = import_if_present(skeleton, dump, "DefaultMix", L"DefaultMix") && ok;
        ok = import_if_present(skeleton, dump, "DefaultSkins", L"DefaultSkins") && ok;
        ok = import_if_present(skeleton, dump, "FootIKAnimations", L"FootIKAnimations") && ok;
        ok = import_if_present(skeleton, dump, "MeshUpdateSettings", L"MeshUpdateSettings") && ok;
        ok = import_if_present(skeleton, dump, "MixData", L"MixData") && ok;
        ok = import_if_present(skeleton, dump, "ScaleFactor", L"ScaleFactor") && ok;
        ok = import_if_present(skeleton, dump, "skeletonDataFileName", L"skeletonDataFileName") && ok;
        ok = set_raw_data_from_skel(skeleton, skel_path) && ok;

        if (ok) log_line("[Lilac2LilyMod][spine_skeleton_data_forge] values imported\n");
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][spine_skeleton_data_forge] failed\n");
        return false;
    }
}
