#pragma once

#include <cstdint>
#include <cstddef>

struct RtVerifyRule {
    const wchar_t* ufunction_path;
    uintptr_t native_rva;
    bool wrapper_should_call_native;
    const char* label;
};

bool rt_config_load(const wchar_t* dll_dir) noexcept;
const wchar_t* rt_path(const char* group, const char* key) noexcept;
uintptr_t rt_rva(const char* key) noexcept;
size_t rt_verify_rule_count() noexcept;
RtVerifyRule rt_verify_rule(size_t index) noexcept;

