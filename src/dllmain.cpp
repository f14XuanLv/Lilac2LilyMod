#include "lifecycle.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <filesystem>
#include <string>

namespace {

void osdbg(const char* msg) noexcept {
    if (msg) OutputDebugStringA(msg);
}

std::wstring this_dll_directory() noexcept {
    HMODULE hmod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&osdbg),
                            &hmod)) {
        return {};
    }
    wchar_t buf[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(hmod, buf, _countof(buf));
    if (n == 0) return {};
    return std::filesystem::path{buf}.parent_path().wstring();
}

} // namespace

class Lilac2LilyMod : public RC::CppUserModBase {
public:
    Lilac2LilyMod() {
        ModName = STR("Lilac2LilyMod");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Flattened Lily costume runtime.");
        ModAuthors = STR("EnderMagnolia modder");
        ModIntendedSDKVersion = STR("3.0");
        osdbg("[Lilac2LilyMod] ctor\n");
    }

    ~Lilac2LilyMod() override {
        osdbg("[Lilac2LilyMod] dtor\n");
        rt_shutdown();
    }

    void on_program_start() override {
        RC::Output::send(STR("[Lilac2LilyMod] on_program_start\n"));
    }

    void on_unreal_init() override {
        osdbg("[Lilac2LilyMod] on_unreal_init\n");
        std::wstring dll_dir = this_dll_directory();
        if (dll_dir.empty()) {
            RC::Output::send(STR("[Lilac2LilyMod] failed to resolve dll dir\n"));
            return;
        }

        rt_on_unreal_init(dll_dir.c_str());
    }
};

#define LILAC2LILYMOD_API __declspec(dllexport)
extern "C" {
LILAC2LILYMOD_API RC::CppUserModBase* start_mod() {
    osdbg("[Lilac2LilyMod] start_mod()\n");
    return new Lilac2LilyMod();
}

LILAC2LILYMOD_API void uninstall_mod(RC::CppUserModBase* mod) {
    osdbg("[Lilac2LilyMod] uninstall_mod()\n");
    rt_shutdown();
    delete mod;
}
}
