//
// Native RVA wrapper check module.
//

#include "rva_check.h"

#include "rt_config.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using namespace RC;
using namespace RC::Unreal;

void log_ascii(const char* msg) noexcept {
    if (!msg) return;
    OutputDebugStringA(msg);
    try {
        StringType wide;
        for (const char* p = msg; *p && wide.size() < 1024; ++p) {
            wide.push_back(static_cast<CharType>(static_cast<unsigned char>(*p)));
        }
        Output::send(STR("{}"), wide);
    } catch (...) {
    }
}

bool wrapper_calls_rva(void* wrapper, uintptr_t module_base, uintptr_t expected_rva) noexcept {
    if (!wrapper || !module_base || !expected_rva) return false;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(wrapper);
    uint8_t buf[0x200]{};
    __try {
        std::memcpy(buf, code, sizeof(buf));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    uint64_t expected_va = static_cast<uint64_t>(module_base + expected_rva);
    uint64_t wrapper_va = reinterpret_cast<uint64_t>(wrapper);
    for (size_t i = 0; i + 5 <= sizeof(buf); ++i) {
        if (buf[i] != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, buf + i + 1, sizeof(rel));
        uint64_t target = wrapper_va + i + 5 + static_cast<int64_t>(rel);
        if (target == expected_va) return true;
    }
    return false;
}

bool verify_one(const wchar_t* function_path, uintptr_t rva, const char* label) noexcept {
    try {
        auto* fn = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, function_path);
        if (!fn) {
            log_ascii("[Lilac2LilyMod][rva] function path not found\n");
            return false;
        }
        void* wrapper = reinterpret_cast<void*>(fn->GetFuncPtr());
        uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        bool ok = wrapper_calls_rva(wrapper, base, rva);
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "[Lilac2LilyMod][rva] %s rva=0x%llx match=%s\n",
                    label ? label : "unknown",
                    static_cast<unsigned long long>(rva),
                    ok ? "YES" : "NO");
        log_ascii(line);
        return ok;
    } catch (...) {
        return false;
    }
}

} // namespace

void rt_verify_rva_wrappers() noexcept {
    if (!IsInGameThread()) {
        log_ascii("[Lilac2LilyMod][rva] skipped: not game thread\n");
        return;
    }
    const size_t count = rt_verify_rule_count();
    for (size_t i = 0; i < count; ++i) {
        RtVerifyRule rule = rt_verify_rule(i);
        bool matched = verify_one(rule.ufunction_path, rule.native_rva, rule.label);
        if (matched != rule.wrapper_should_call_native) {
            log_ascii("[Lilac2LilyMod][rva] wrapper expectation mismatch\n");
        }
    }
}
