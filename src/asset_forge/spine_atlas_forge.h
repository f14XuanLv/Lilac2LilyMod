#pragma once

#include "asset_forge/uobject_copy_forge.h"

bool asset_forge_add_spine_atlas(const UObjectCopyForgeRequest& request,
                                 const wchar_t* json_path,
                                 const UObjectCopyForgeRequest& texture_request) noexcept;
