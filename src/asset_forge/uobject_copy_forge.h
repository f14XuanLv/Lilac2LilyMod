#pragma once

struct UObjectCopyForgeRequest {
    const wchar_t* outer_path;
    const wchar_t* template_object_path;
    const wchar_t* object_name;
};

bool uobject_copy_forge_add(const UObjectCopyForgeRequest& request) noexcept;
