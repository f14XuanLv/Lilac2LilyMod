#include "rt_config.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <glaze/glaze.hpp>
#include <Windows.h>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct VerifyJson {
    std::string ufunction;
    bool wrapper_should_call_native = false;
};

struct RvaJson {
    int schema = 0;
    std::string module;
    std::string image_base_note;
    std::unordered_map<std::string, std::string> rva;
    std::unordered_map<std::string, VerifyJson> verify;
};

struct UObjectPathsJson {
    int schema = 0;
    std::unordered_map<std::string, std::string> ufunctions;
    std::unordered_map<std::string, std::string> classes;
    std::unordered_map<std::string, std::string> objects;
    std::unordered_map<std::string, std::string> assets;
    std::unordered_map<std::string, std::string> names;
};

namespace {

std::unordered_map<std::string, uintptr_t> g_rva;
std::unordered_map<std::string, std::unordered_map<std::string, std::wstring>> g_paths;
std::vector<RtVerifyRule> g_verify;

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

uintptr_t parse_hex(const std::string& s) noexcept {
    std::string_view v{s};
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v.remove_prefix(2);
    uintptr_t value = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), value, 16);
    if (ec != std::errc{} || ptr != v.data() + v.size()) return 0;
    return value;
}

void store_group(const char* name, const std::unordered_map<std::string, std::string>& src) {
    auto& dst = g_paths[name];
    dst.clear();
    for (const auto& [k, v] : src) {
        dst.emplace(k, utf8_to_utf16(v));
    }
}

void log_line(const char* msg) noexcept {
    if (msg) OutputDebugStringA(msg);
}

} // namespace

bool rt_config_load(const wchar_t* dll_dir) noexcept {
    g_rva.clear();
    g_paths.clear();
    g_verify.clear();
    if (!dll_dir) return false;

    const std::filesystem::path base{dll_dir};
    std::string rva_buf = slurp(base / L"config" / L"RVA.json");
    std::string path_buf = slurp(base / L"config" / L"UObjectPaths.json");
    if (rva_buf.empty() || path_buf.empty()) {
        log_line("[Lilac2LilyMod][config] missing config/RVA.json or config/UObjectPaths.json\n");
        return false;
    }

    RvaJson rva_json{};
    UObjectPathsJson path_json{};
    if (auto err = glz::read_json(rva_json, rva_buf); err) {
        log_line("[Lilac2LilyMod][config] failed to parse RVA.json\n");
        return false;
    }
    if (auto err = glz::read_json(path_json, path_buf); err) {
        log_line("[Lilac2LilyMod][config] failed to parse UObjectPaths.json\n");
        return false;
    }

    for (const auto& [k, v] : rva_json.rva) {
        const uintptr_t parsed = parse_hex(v);
        if (parsed) g_rva.emplace(k, parsed);
    }

    store_group("ufunctions", path_json.ufunctions);
    store_group("classes", path_json.classes);
    store_group("objects", path_json.objects);
    store_group("assets", path_json.assets);
    store_group("names", path_json.names);

    for (const auto& [label, rule] : rva_json.verify) {
        const auto rva_it = g_rva.find(label);
        const wchar_t* fn_path = rt_path("ufunctions", rule.ufunction.c_str());
        if (rva_it == g_rva.end()) {
            log_line("[Lilac2LilyMod][config] verify skipped: missing rva key\n");
            continue;
        }
        if (!fn_path) {
            log_line("[Lilac2LilyMod][config] verify skipped: missing ufunction key\n");
            continue;
        }
        g_verify.push_back(RtVerifyRule{
            fn_path,
            rva_it->second,
            rule.wrapper_should_call_native,
            rva_it->first.c_str()
        });
    }

    OutputDebugStringA("[Lilac2LilyMod][config] loaded\n");
    return true;
}

const wchar_t* rt_path(const char* group, const char* key) noexcept {
    if (!group || !key) return nullptr;
    auto git = g_paths.find(group);
    if (git == g_paths.end()) return nullptr;
    auto kit = git->second.find(key);
    if (kit == git->second.end()) return nullptr;
    return kit->second.c_str();
}

uintptr_t rt_rva(const char* key) noexcept {
    if (!key) return 0;
    auto it = g_rva.find(key);
    return it == g_rva.end() ? 0 : it->second;
}

size_t rt_verify_rule_count() noexcept {
    return g_verify.size();
}

RtVerifyRule rt_verify_rule(size_t index) noexcept {
    if (index >= g_verify.size()) return RtVerifyRule{};
    return g_verify[index];
}
