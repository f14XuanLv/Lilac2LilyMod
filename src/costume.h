#pragma once
//
// Costume session module.
//

enum class CostumeRemoveMode {
    DestroyRow,
    ForgetRow,
};

bool costume_add(const wchar_t* mod_dll_dir) noexcept;
bool costume_remove(CostumeRemoveMode mode) noexcept;
