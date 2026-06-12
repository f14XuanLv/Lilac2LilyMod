#pragma once

#include "asset_forge/uobject_copy_forge.h"

bool asset_forge_add_texture2d(const UObjectCopyForgeRequest& request, const wchar_t* json_path, const wchar_t* png_path) noexcept;
void asset_forge_release_texture2d_platform_data(const UObjectCopyForgeRequest& request) noexcept;
