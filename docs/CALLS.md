# 游戏/Unreal/插件函数调用清单

本文记录当前 [`Lilac2LilyMod`](../CMakeLists.txt) 主动调用、反射调用、或通过 MinHook trampoline 放行的游戏/Unreal/插件函数。旧版 [`CALLS.md`](CALLS.md) 曾有乱码和过时路径，本文以当前 [`src`](../src) 为准。

不把普通 C++ 容器/SDK 辅助 API 视作“游戏函数调用”，例如 `UDataTable::AddRow`、`UDataTable::RemoveRow`、`FindRowUnchecked`、`GetValuePtrByPropertyNameInChain`；它们在“直接对象/属性修改”章节单独说明。

## 1. 反射 `ProcessEvent` 主动调用

### `/Script/Engine.KismetInternationalizationLibrary:GetCurrentCulture`

- 配置键：`ufunctions.Kismet_GetCurrentCulture`，见 [`UObjectPaths.json`](../config/UObjectPaths.json)。
- 调用位置：[`current_culture()`](../src/costume.cpp)。
- 调用对象：`/Script/Engine.Default__KismetInternationalizationLibrary`。
- 调用方式：`self->ProcessEvent(fn, &params)`。
- 用途：读取当前 culture，用于选择 `p0101.Name` / `p0101.Description`。
- 失败处理：回退到 `en-US`。

### `/Script/SpinePlugin.SpineSkeletonAnimationComponent:SetAnimation`

- 配置键：`ufunctions.SpineSkeletonAnimationComponent_SetAnimation`。
- 调用位置：[`seed_idle_animation()`](../src/spine.cpp)。
- 调用对象：当前 `BP_p0000_Lilac_C` actor 的 `SpineAnimationComponent`。
- 调用方式：反射遍历参数槽后写入：
  - `trackIndex = 0`
  - `animationName = "idle"`
  - `loop = true`
- 用途：切到 Lily skeleton 后补种 0 号轨道 idle，避免替换后没有有效初始动画。
- 返回值：忽略。

### AssetRegistry 相关反射调用

[`uobject_copy_forge_add()`](../src/asset_forge/uobject_copy_forge.cpp) 在模板对象尚未加载时，会通过 AssetRegistry 反射路径尝试加载模板：

| 配置键 | 路径 | 用途 |
|---|---|---|
| `AssetRegistry_GetAssetRegistry` | `/Script/AssetRegistry.AssetRegistryHelpers:GetAssetRegistry` | 获取 registry 对象 |
| `AssetRegistry_GetAssetByObjectPath` | `/Script/AssetRegistry.AssetRegistry:GetAssetByObjectPath` | 由模板路径查询 `FAssetData` |
| `AssetRegistry_GetAsset` | `/Script/AssetRegistry.AssetRegistryHelpers:GetAsset` | 由 `FAssetData` 加载 UObject |

这些调用只发生在 `StaticFindObject(template)` 找不到模板时，是当前运行时伪造资产链路的兜底加载策略。

## 2. 直接 RVA 调用

### `Texture_UpdateResource`

- 配置键：`rva.Texture_UpdateResource`，见 [`RVA.json`](../config/RVA.json)。
- 当前 RVA：`0x03C20A80`。
- 调用位置：[`call_texture_update_resource()`](../src/asset_forge/texture2d_forge.cpp)。
- 调用签名：`void(__fastcall*)(UObject*)`。
- 用途：在手动构造 `Texture2D` runtime `PrivatePlatformData` 后，创建/刷新 GPU resource。
- 风险：该 RVA 不是通过稳定 UFunction wrapper 解析出来的；游戏更新后可能找不到或特征变化，需要按 [`RVA_RELOCATION.md`](RVA_RELOCATION.md) 重新定位。

## 3. MinHook detour trampoline 放行

这些调用不是业务额外调用游戏逻辑，而是 detour 拦截后显式继续执行原函数。

### `SpineSkeletonAnimationComponent::SetAnimation`

- 配置键：`rva.SpineSkeletonAnimationComponent_SetAnimation`。
- 当前 RVA：`0x043F2710`。
- detour：[`component_set_detour()`](../src/spine.cpp)。
- 放行：`g_component_set_original(self, track, name, loop)`。
- 额外逻辑：若 `self` 属于当前 Lilac actor 的 component，则按 [`Lilac2LilyAnimationMap.json`](../config/Lilac2LilyAnimationMap.json) 改写动画名。

### `SpineSkeletonAnimationComponent::AddAnimation`

- 配置键：`rva.SpineSkeletonAnimationComponent_AddAnimation`。
- 当前 RVA：`0x043E8B90`。
- detour：[`component_add_detour()`](../src/spine.cpp)。
- 放行：`g_component_add_original(self, track, name, loop, delay)`。
- 额外逻辑：同上，改写 component 排队动画名。

### `SpineWidget::SetAnimation`

- 配置键：`rva.SpineWidget_SetAnimation`。
- 当前 RVA：`0x043F2B40`。
- detour：[`widget_set_detour()`](../src/spine.cpp)。
- 放行：`g_widget_set_original(self, track, name, loop)`。
- 额外逻辑：若 widget 的 `Atlas` 路径标识为 `p0101_Lily`，则改写 UI Spine 动画名。

### `SpineWidget::AddAnimation`

- 配置键：`rva.SpineWidget_AddAnimation`。
- 当前 RVA：`0x043E8FD0`。
- detour：[`widget_add_detour()`](../src/spine.cpp)。
- 放行：`g_widget_add_original(self, track, name, loop, delay)`。
- 额外逻辑：同上，改写 widget 排队动画名。

### `SpineSkeletonAnimationExComponent::ReplaceSpineData`

- 配置键：`rva.SpineSkeletonAnimationExComponent_ReplaceSpineData`。
- 当前 RVA：`0x04528710`。
- detour：[`replace_spine_detour()`](../src/spine.cpp)。
- 放行：`g_replace_original(self, atlas, skel, notify)`。
- 前置逻辑：如果目标 skeleton 是 Lily，先写 `ScaleFactor`、`DefaultMix`、`DefaultSkins`。
- 后置逻辑：启用/禁用动画映射 hook、安排 `VisualPivotSceneComponent.RelativeScale3D` 延迟写入、补种 idle。
- 风险：[`RVA.json`](../config/RVA.json) 的 wrapper 验证期望为 `false`，表示反射 wrapper 不一定 call 到该 native body；当前 hook 覆盖真实换装 direct native path，但不能假设覆盖所有 `ProcessEvent` 路径。

### `UnrealWindow_WndProcThunk`

- 配置键：`rva.UnrealWindow_WndProcThunk`。
- 当前 RVA：`0x0130F6E0`。
- detour：[`window_proc_detour()`](../src/lifecycle.cpp)。
- 放行：`g_window_original(hwnd, msg, wparam, lparam)`。
- 用途：捕获 `WM_CLOSE` / `SC_CLOSE`，在窗口右上角关闭前做运行时资源清理。

## 4. UE4SS Hook callback，不是直接 native call

### 全局 `ProcessEvent` pre callback

- 安装位置：[`install_process_event_hook()`](../src/lifecycle.cpp)。
- 用途：识别生命周期函数：
  - `/Script/Engine.PlayerController:ClientRestart`
  - `WBP_Pause:OnBackToTitleConfirm`
  - `WBP_Pause:OnExitConfirm`
  - `WBP_Title:...Exit...OnPressed`
- 行为：进入 session、返回标题、退出清理。

### Local Script Function post callback

- 安装位置：[`install_filter_callback()`](../src/costume.cpp)。
- 用途：修改 UI filter 结果：
  - `WBP_Extra_Costume_C:CanDisplayCostumeData`
  - `WBP_Extra_CostumeEntry_C:IsCostumeOwned`
- 行为：当 handle/entry 的 row name 是 `p0101` 时，把 Blueprint bool out 参数强制写成 true。

### Engine Tick pre callback

- 安装位置：[`install_tick_callback()`](../src/spine.cpp)。
- 用途：延迟 8 帧写 `VisualPivotSceneComponent.RelativeScale3D`。
- 原因：换装后组件/actor 状态会在后续帧被游戏刷新，延迟写更稳定。

## 5. 直接对象/属性修改

### `DT_ItemCostumes` 行注入

- 位置：[`add_lily_row()`](../src/costume.cpp)。
- 对象：`/Game/_Zion/Gameplay/Data/DT_ItemCostumes.DT_ItemCostumes`。
- 操作：以 `p0000` 行为模板 `AddRow("p0101")`，再覆写 `AtlasAsset`、`SkeletonDataAsset`、`Icon` 的 soft path。
- `p0101` 会真实进入运行时 `RowMap`。

### `DT_ItemCostumes` 文本覆写

- 位置：[`apply_text_overrides()`](../src/costume.cpp)。
- 数据：[`DT_ItemCostumes_Extension.json`](../config/DT_ItemCostumes_Extension.json)。
- 字段：`Name`、`Description`。

### Lily skeleton 配置

- 位置：[`configure_lily_skeleton()`](../src/spine.cpp)。
- 字段：`ScaleFactor = 0.26f`、`DefaultMix = 0.1f`、`DefaultSkins = ["_common", "_Meat_Head_0", "p0101_Lily"]`。

### Visual scale

- 位置：[`write_visual_scale()`](../src/spine.cpp)。
- 字段：当前 Lilac actor 的 `VisualPivotSceneComponent.RelativeScale3D`。
- Lily：`(-1, 1, 1)`；普通：`(1, 1, 1)`。

### 运行时 Texture2D platform data

- 位置：[`attach_decoded_platform_data()`](../src/asset_forge/texture2d_forge.cpp)。
- 操作：手动构造 POD-compatible `FTexturePlatformData` / mip / bulk data，并写入推断出的 `PrivatePlatformData` 指针。
- 清理：[`asset_forge_release_texture2d_platform_data()`](../src/asset_forge/texture2d_forge.cpp)。
