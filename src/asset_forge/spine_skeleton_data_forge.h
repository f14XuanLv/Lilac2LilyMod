#pragma once

#include "asset_forge/uobject_copy_forge.h"

bool asset_forge_add_spine_skeleton_data(const UObjectCopyForgeRequest& request,
                                         const wchar_t* json_path,
                                         const wchar_t* skel_path) noexcept;
