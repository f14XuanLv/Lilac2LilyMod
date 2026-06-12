#include "asset_forge/texture2d_forge.h"

#include "rt_config.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FField.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <glaze/glaze.hpp>
#include <Windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Texture2DDumpJson {
    glz::raw_json Meta;
    std::unordered_map<std::string, std::string> Values;
};

namespace {

using namespace RC;
using namespace RC::Unreal;

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
};

struct RuntimeTArrayPtr {
    void** data = nullptr;
    int32_t num = 0;
    int32_t max = 0;
};

struct RuntimeFOptTexturePlatformData {
    uint32_t ext_data = 0;
    uint32_t num_mips_in_tail = 0;
};

struct RuntimeFTexturePlatformData {
    int32_t size_x = 0;
    int32_t size_y = 0;
    uint32_t packed_data = 1;
    uint8_t pixel_format = 2;
    uint8_t pad0[3]{};
    RuntimeFOptTexturePlatformData opt_data{};
    RuntimeTArrayPtr mips{};
    void* vt_data = nullptr;
    void* cpu_copy = nullptr;
};

struct RuntimeFDerivedData {
    uint64_t chunk_offset = 0;
    uint64_t chunk_size = 0;
    uint8_t chunk_id[12]{};
    uint32_t flags = 0;
};

struct RuntimeFBulkData {
    void* data_allocation = nullptr;
    uint8_t meta[16]{};
    uint8_t chunk_id[12]{};
    uint8_t pad0[4]{};
};

struct RuntimeFTexture2DMipMap {
    RuntimeFDerivedData derived_data{};
    RuntimeFBulkData bulk_data{};
    uint16_t size_x = 0;
    uint16_t size_y = 0;
    uint16_t size_z = 1;
    uint16_t pad0 = 0;
};

struct OwnedRuntimePlatformData {
    RuntimeFTexturePlatformData* pd = nullptr;
    RuntimeFTexture2DMipMap* mip = nullptr;
    void** mip_slots = nullptr;
    uint8_t* pixels = nullptr;
};

static_assert(sizeof(RuntimeFTexturePlatformData) == 56);
static_assert(sizeof(RuntimeFDerivedData) == 32);
static_assert(sizeof(RuntimeFBulkData) == 40);
static_assert(sizeof(RuntimeFTexture2DMipMap) == 80);

constexpr uint8_t kPixelFormatB8G8R8A8 = 2;
constexpr uint32_t kBulkDataForceInlinePayload = 1u << 6;
constexpr int32_t kTexturePrivateResourceOffset = 272;
constexpr int32_t kTexturePrivateResourceRenderThreadOffset = 280;

std::unordered_map<UObject*, OwnedRuntimePlatformData> g_owned_platform_data;

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

std::string to_ascii(StringType const& text) {
    std::string out;
    out.reserve(text.size());
    for (auto c : text) out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?');
    return out;
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

std::wstring utf8_to_utf16(const std::string& s) noexcept {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void log_object(const char* tag, UObject* obj) noexcept {
    char msg[768];
    if (!obj) {
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][texture2d_forge] %s ptr=null\n", tag ? tag : "object");
        log_line(msg);
        return;
    }

    std::string full;
    try { full = to_ascii(obj->GetFullName()); } catch (...) { full = "<GetFullName failed>"; }
    std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][texture2d_forge] %s ptr=%p full=%s\n",
                  tag ? tag : "object", obj, full.c_str());
    log_line(msg);
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

void root_object(UObject* object) noexcept {
    if (!object) return;
    object->SetFlags(static_cast<EObjectFlags>(RF_Public | RF_Standalone | RF_MarkAsRootSet));
    if (!object->IsRootSet()) object->SetRootSet();
}

std::wstring make_object_path(const UObjectCopyForgeRequest& request) {
    std::wstring path{request.outer_path};
    path.push_back(L'.');
    path.append(request.object_name);
    return path;
}

UObject* static_find_safe(const wchar_t* path, UObject* outer = nullptr) noexcept {
    if (!path) return nullptr;
    __try {
        return UObjectGlobals::StaticFindObject<UObject*>(nullptr, outer, path);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

UObject* find_texture_object(const UObjectCopyForgeRequest& request) noexcept {
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

bool import_text_property(UObject* object, const wchar_t* property_name, const wchar_t* text) noexcept {
    if (!object || !property_name || !text) return false;
    FProperty* prop = find_property_by_name(object->GetClassPrivate(), property_name);
    if (!prop) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][texture2d_forge] property missing: %ls\n", property_name);
        log_line(msg);
        return false;
    }

    const wchar_t* end = prop->ImportText_InContainer(text, object, object, PPF_None, nullptr);
    if (!end) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][texture2d_forge] property import failed: %ls\n", property_name);
        log_line(msg);
        return false;
    }
    return true;
}

bool import_text_property_utf8(UObject* object, const wchar_t* property_name, const std::string& text) noexcept {
    const std::wstring wide = utf8_to_utf16(text);
    return import_text_property(object, property_name, wide.c_str());
}

bool import_if_present(UObject* object, const Texture2DDumpJson& dump, const char* json_key, const wchar_t* property_name) noexcept {
    auto it = dump.Values.find(json_key);
    if (it == dump.Values.end()) return true;
    return import_text_property_utf8(object, property_name, it->second);
}

size_t align_up(size_t value, size_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool private_platform_data_offset(UObject* texture, int32_t& out_offset) noexcept {
    out_offset = -1;
    if (!texture) return false;

    FProperty* imported_size = find_property_by_name(texture->GetClassPrivate(), L"ImportedSize");
    if (!imported_size) {
        log_line("[Lilac2LilyMod][texture2d_forge] ImportedSize property missing; cannot infer PrivatePlatformData offset\n");
        return false;
    }

    const int32_t imported_offset = imported_size->GetOffset_Internal();
    if (imported_offset < 0) return false;

    out_offset = static_cast<int32_t>(align_up(static_cast<size_t>(imported_offset) + 8u, alignof(void*)));
    return true;
}

void* read_pointer_field(UObject* object, int32_t offset) noexcept {
    if (!object || offset < 0) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool write_pointer_field(UObject* object, int32_t offset, void* value) noexcept {
    if (!object || offset < 0) return false;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void store_u40(uint8_t* dst, uint64_t value) noexcept {
    for (int i = 0; i < 5; ++i) dst[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
}

void store_u32(uint8_t* dst, uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) dst[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFFu);
}

void fill_bulk_meta(RuntimeFBulkData& bulk, uint64_t byte_count) noexcept {
    void* data_allocation = bulk.data_allocation;
    std::memset(&bulk, 0, sizeof(bulk));
    bulk.data_allocation = data_allocation;
    store_u40(&bulk.meta[0], byte_count);
    store_u40(&bulk.meta[5], UINT64_MAX);
    bulk.meta[10] = 0;
    bulk.meta[11] = 0;
    store_u32(&bulk.meta[12], kBulkDataForceInlinePayload);
}

bool attach_decoded_platform_data(UObject* texture, const DecodedImage& image) noexcept {
    if (!valid_object(texture) || image.width == 0 || image.height == 0 || image.bgra.empty()) return false;
    if (image.width > UINT16_MAX || image.height > UINT16_MAX) {
        log_line("[Lilac2LilyMod][texture2d_forge] image too large for FTexture2DMipMap uint16 dimensions\n");
        return false;
    }

    int32_t platform_data_offset = -1;
    if (!private_platform_data_offset(texture, platform_data_offset)) return false;

    const size_t byte_count = image.bgra.size();
    std::unique_ptr<RuntimeFTexturePlatformData> pd{new (std::nothrow) RuntimeFTexturePlatformData{}};
    std::unique_ptr<RuntimeFTexture2DMipMap> mip{new (std::nothrow) RuntimeFTexture2DMipMap{}};
    std::unique_ptr<void*[]> mip_slots{new (std::nothrow) void*[1]{}};
    std::unique_ptr<uint8_t[]> pixels{new (std::nothrow) uint8_t[byte_count]{}};
    if (!pd || !mip || !mip_slots || !pixels) {
        log_line("[Lilac2LilyMod][texture2d_forge] platform data allocation failed\n");
        return false;
    }

    std::memcpy(pixels.get(), image.bgra.data(), byte_count);

    pd->size_x = static_cast<int32_t>(image.width);
    pd->size_y = static_cast<int32_t>(image.height);
    pd->packed_data = 1;
    pd->pixel_format = kPixelFormatB8G8R8A8;
    pd->mips.data = mip_slots.get();
    pd->mips.num = 1;
    pd->mips.max = 1;
    mip_slots[0] = mip.get();

    mip->size_x = static_cast<uint16_t>(image.width);
    mip->size_y = static_cast<uint16_t>(image.height);
    mip->size_z = 1;
    mip->bulk_data.data_allocation = pixels.get();
    fill_bulk_meta(mip->bulk_data, static_cast<uint64_t>(byte_count));

    void* old_platform_data = read_pointer_field(texture, platform_data_offset);
    if (!write_pointer_field(texture, platform_data_offset, pd.get())) {
        log_line("[Lilac2LilyMod][texture2d_forge] failed to write decoded PrivatePlatformData pointer\n");
        return false;
    }

    auto* raw_pd = pd.release();
    auto* raw_mip = mip.release();
    auto* raw_mip_slots = mip_slots.release();
    auto* raw_pixels = pixels.release();
    try {
        g_owned_platform_data[texture] = OwnedRuntimePlatformData{raw_pd, raw_mip, raw_mip_slots, raw_pixels};
    } catch (...) {
    }

    char msg[384];
    std::snprintf(msg, sizeof(msg),
                  "[Lilac2LilyMod][texture2d_forge] decoded PrivatePlatformData attached offset=%d old=%p pd=%p mip=%p pixels=%p %ux%u bytes=%zu pf=PF_B8G8R8A8\n",
                  platform_data_offset, old_platform_data, raw_pd, raw_mip, raw_pixels, image.width, image.height, byte_count);
    log_line(msg);
    return true;
}

bool texture_has_runtime_platform_data(UObject* texture) noexcept {
    int32_t platform_data_offset = -1;
    if (!private_platform_data_offset(texture, platform_data_offset)) return false;
    void* platform_data = read_pointer_field(texture, platform_data_offset);
    void* resource = read_pointer_field(texture, kTexturePrivateResourceOffset);
    return platform_data && resource;
}

bool call_texture_update_resource(UObject* texture) noexcept {
    if (!valid_object(texture)) return false;

    const uintptr_t rva = rt_rva("Texture_UpdateResource");
    if (!rva) {
        log_line("[Lilac2LilyMod][texture2d_forge] Texture_UpdateResource RVA missing\n");
        return false;
    }

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) {
        log_line("[Lilac2LilyMod][texture2d_forge] main module unavailable for Texture_UpdateResource\n");
        return false;
    }

    using UpdateResourceFn = void(__fastcall*)(UObject*);
    auto* fn = reinterpret_cast<UpdateResourceFn>(reinterpret_cast<uintptr_t>(module) + rva);

    void* before_resource = read_pointer_field(texture, kTexturePrivateResourceOffset);
    void* before_render_resource = read_pointer_field(texture, kTexturePrivateResourceRenderThreadOffset);
    __try {
        fn(texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[Lilac2LilyMod][texture2d_forge] Texture_UpdateResource raised exception\n");
        return false;
    }

    void* after_resource = read_pointer_field(texture, kTexturePrivateResourceOffset);
    void* after_render_resource = read_pointer_field(texture, kTexturePrivateResourceRenderThreadOffset);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "[Lilac2LilyMod][texture2d_forge] Texture_UpdateResource called rva=0x%zx target=%p resource %p->%p rt %p->%p\n",
                  static_cast<size_t>(rva), texture, before_resource, after_resource, before_render_resource, after_render_resource);
    log_line(msg);
    return true;
}

template <typename T>
void safe_release(T*& ptr) noexcept {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool decode_png_bgra8(const wchar_t* png_path, DecodedImage& out) noexcept {
    out = {};
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        log_line("[Lilac2LilyMod][texture2d_forge] WIC factory unavailable\n");
        return false;
    }

    hr = factory->CreateDecoderFromFilename(png_path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        safe_release(converter);
        safe_release(frame);
        safe_release(decoder);
        safe_release(factory);
        log_line("[Lilac2LilyMod][texture2d_forge] PNG decoder failed\n");
        return false;
    }

    hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&out.width, &out.height);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }

    if (SUCCEEDED(hr) && out.width > 0 && out.height > 0) {
        const uint64_t size = static_cast<uint64_t>(out.width) * static_cast<uint64_t>(out.height) * 4u;
        if (size <= 0x7FFFFFFFull) {
            out.bgra.resize(static_cast<size_t>(size));
            hr = converter->CopyPixels(nullptr, out.width * 4u, static_cast<UINT>(out.bgra.size()), out.bgra.data());
        } else {
            hr = E_OUTOFMEMORY;
        }
    }

    safe_release(converter);
    safe_release(frame);
    safe_release(decoder);
    safe_release(factory);

    if (FAILED(hr) || out.bgra.empty()) {
        log_line("[Lilac2LilyMod][texture2d_forge] PNG decode failed\n");
        out = {};
        return false;
    }

    const size_t pixel_count = out.width * static_cast<size_t>(out.height);
    size_t solid_count = 0;
    for (size_t i = 0; i < pixel_count; ++i) {
        if (out.bgra[i * 4u + 3] != 0) ++solid_count;
    }

    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "[Lilac2LilyMod][texture2d_forge] PNG decoded %ux%u bytes=%zu nonzeroA=%zu\n",
                  out.width, out.height, out.bgra.size(), solid_count);
    log_line(msg);
    return true;
}

} // namespace

bool validate_texture2d_shell(const UObjectCopyForgeRequest& request) noexcept {
    UObject* texture = find_texture_object(request);
    if (!valid_object(texture)) {
        log_line("[Lilac2LilyMod][texture2d_forge] forged texture unavailable\n");
        return false;
    }
    if (!texture->GetClassPrivate() || texture->GetClassPrivate()->GetName() != StringType{L"Texture2D"}) {
        log_line("[Lilac2LilyMod][texture2d_forge] forged object is not Texture2D\n");
        return false;
    }

    root_object(texture);
    log_object("Texture2D shell ready", texture);
    return true;
}

bool set_texture2d_from_json(const UObjectCopyForgeRequest& request, const wchar_t* json_path) noexcept {
    try {
        if (!valid_request(request) || !json_path || !json_path[0]) {
            log_line("[Lilac2LilyMod][texture2d_forge] JSON fill skipped: request/json path incomplete\n");
            return false;
        }

        UObject* texture = find_texture_object(request);
        if (!valid_object(texture)) {
            log_line("[Lilac2LilyMod][texture2d_forge] JSON fill skipped: texture unavailable\n");
            return false;
        }
        if (!texture->GetClassPrivate() || texture->GetClassPrivate()->GetName() != StringType{L"Texture2D"}) {
            log_line("[Lilac2LilyMod][texture2d_forge] JSON fill skipped: target is not Texture2D\n");
            return false;
        }

        std::string buf = slurp(std::filesystem::path{json_path});
        if (buf.empty()) {
            log_line("[Lilac2LilyMod][texture2d_forge] JSON fill failed: JSON missing or empty\n");
            return false;
        }

        Texture2DDumpJson dump{};
        if (auto err = glz::read_json(dump, buf); err) {
            const std::string detail = glz::format_error(err, buf);
            char msg[768];
            std::snprintf(msg, sizeof(msg), "[Lilac2LilyMod][texture2d_forge] JSON fill failed: JSON parse failed: %.640s\n", detail.c_str());
            log_line(msg);
            return false;
        }

        bool ok = true;
        ok = import_if_present(texture, dump, "AddressX", L"AddressX") && ok;
        ok = import_if_present(texture, dump, "AddressY", L"AddressY") && ok;
        ok = import_if_present(texture, dump, "CompressionSettings", L"CompressionSettings") && ok;
        ok = import_if_present(texture, dump, "CompressionYCoCg", L"CompressionYCoCg") && ok;
        ok = import_if_present(texture, dump, "CookPlatformTilingSettings", L"CookPlatformTilingSettings") && ok;
        ok = import_if_present(texture, dump, "Downscale", L"Downscale") && ok;
        ok = import_if_present(texture, dump, "DownscaleOptions", L"DownscaleOptions") && ok;
        ok = import_if_present(texture, dump, "Filter", L"Filter") && ok;
        ok = import_if_present(texture, dump, "FirstResourceMemMip", L"FirstResourceMemMip") && ok;
        ok = import_if_present(texture, dump, "ForceMipLevelsToBeResidentTimestamp", L"ForceMipLevelsToBeResidentTimestamp") && ok;
        ok = import_if_present(texture, dump, "ImportedSize", L"ImportedSize") && ok;
        ok = import_if_present(texture, dump, "LODBias", L"LODBias") && ok;
        ok = import_if_present(texture, dump, "LODGroup", L"LODGroup") && ok;
        ok = import_if_present(texture, dump, "LevelIndex", L"LevelIndex") && ok;
        ok = import_if_present(texture, dump, "LightingGuid", L"LightingGuid") && ok;
        ok = import_if_present(texture, dump, "MipLoadOptions", L"MipLoadOptions") && ok;
        ok = import_if_present(texture, dump, "NeverStream", L"NeverStream") && ok;
        ok = import_if_present(texture, dump, "NoRefStreamingLODBias", L"NoRefStreamingLODBias") && ok;
        ok = import_if_present(texture, dump, "NumCinematicMipLevels", L"NumCinematicMipLevels") && ok;
        ok = import_if_present(texture, dump, "SRGB", L"SRGB") && ok;
        ok = import_if_present(texture, dump, "StreamingIndex", L"StreamingIndex") && ok;
        ok = import_if_present(texture, dump, "VirtualTextureStreaming", L"VirtualTextureStreaming") && ok;
        ok = import_if_present(texture, dump, "bAsyncResourceReleaseHasBeenStarted", L"bAsyncResourceReleaseHasBeenStarted") && ok;
        ok = import_if_present(texture, dump, "bForceMiplevelsToBeResident", L"bForceMiplevelsToBeResident") && ok;
        ok = import_if_present(texture, dump, "bGlobalForceMipLevelsToBeResident", L"bGlobalForceMipLevelsToBeResident") && ok;
        ok = import_if_present(texture, dump, "bHasStreamingUpdatePending", L"bHasStreamingUpdatePending") && ok;
        ok = import_if_present(texture, dump, "bIgnoreStreamingMipBias", L"bIgnoreStreamingMipBias") && ok;
        ok = import_if_present(texture, dump, "bNoTiling", L"bNoTiling") && ok;
        ok = import_if_present(texture, dump, "bNotOfflineProcessed", L"bNotOfflineProcessed") && ok;
        ok = import_if_present(texture, dump, "bOodlePreserveExtremes", L"bOodlePreserveExtremes") && ok;
        ok = import_if_present(texture, dump, "bTemporarilyDisableStreaming", L"bTemporarilyDisableStreaming") && ok;
        ok = import_if_present(texture, dump, "bUseCinematicMipLevels", L"bUseCinematicMipLevels") && ok;

        if (ok) log_object("Texture2D reflected values imported", texture);
        return ok;
    } catch (...) {
        log_line("[Lilac2LilyMod][texture2d_forge] JSON fill failed\n");
        return false;
    }
}

bool attach_texture2d_png_platform_data(const UObjectCopyForgeRequest& request, const wchar_t* png_path) noexcept {
    if (!valid_request(request) || !png_path || !png_path[0]) {
        log_line("[Lilac2LilyMod][texture2d_forge] platform data attach skipped: request/png path incomplete\n");
        return false;
    }

    UObject* texture = find_texture_object(request);
    if (!valid_object(texture)) {
        log_line("[Lilac2LilyMod][texture2d_forge] platform data attach skipped: texture unavailable\n");
        return false;
    }

    if (texture_has_runtime_platform_data(texture)) {
        log_line("[Lilac2LilyMod][texture2d_forge] runtime platform data already initialized\n");
        return true;
    }

    DecodedImage image{};
    if (!decode_png_bgra8(png_path, image)) return false;

    if (!attach_decoded_platform_data(texture, image)) return false;

    // Required for display: PrivatePlatformData alone leaves the sprite transparent until a GPU resource is created.
    if (!call_texture_update_resource(texture)) {
        log_line("[Lilac2LilyMod][texture2d_forge] decoded platform data attached, but required GPU resource refresh failed\n");
        return false;
    }
    return true;
}

void asset_forge_release_texture2d_platform_data(const UObjectCopyForgeRequest& request) noexcept {
    try {
        UObject* texture = find_texture_object(request);
        if (!texture) {
            log_line("[Lilac2LilyMod][texture2d_forge] platform data release skipped: texture unavailable\n");
            return;
        }

        auto it = g_owned_platform_data.find(texture);
        if (it == g_owned_platform_data.end()) {
            log_line("[Lilac2LilyMod][texture2d_forge] platform data release skipped: no owned runtime data\n");
            return;
        }

        int32_t platform_data_offset = -1;
        if (private_platform_data_offset(texture, platform_data_offset)) {
            void* current = read_pointer_field(texture, platform_data_offset);
            if (current == it->second.pd) {
                // UE may destroy the Texture2D later; detach our POD-compatible runtime data first so UE does not own it.
                (void)write_pointer_field(texture, platform_data_offset, nullptr);
            } else {
                log_line("[Lilac2LilyMod][texture2d_forge] platform data release: current pointer is not owned data\n");
            }
        }

        delete[] it->second.pixels;
        delete[] it->second.mip_slots;
        delete it->second.mip;
        delete it->second.pd;
        g_owned_platform_data.erase(it);
        log_line("[Lilac2LilyMod][texture2d_forge] owned runtime platform data released\n");
    } catch (...) {
        log_line("[Lilac2LilyMod][texture2d_forge] platform data release failed\n");
    }
}

bool asset_forge_add_texture2d(const UObjectCopyForgeRequest& request, const wchar_t* json_path, const wchar_t* png_path) noexcept {
    if (!uobject_copy_forge_add(request)) {
        log_line("[Lilac2LilyMod][texture2d_forge] copy construction failed\n");
        return false;
    }
    if (!validate_texture2d_shell(request)) return false;
    if (!set_texture2d_from_json(request, json_path)) return false;
    return attach_texture2d_png_platform_data(request, png_path);
}
