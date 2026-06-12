# 当前 Hook 清单

本文只记录当前 [`Lilac2LilyMod`](../CMakeLists.txt) 完全体实际安装/使用的 Hook。旧文档里的 `spine::AnimationState::setCurrent`、旧目录 `src/hooks/...`、以及若干调研 RVA 已不属于当前代码。

## 配置来源

所有 native RVA 都从 [`RVA.json`](../config/RVA.json) 读取，所有反射路径都从 [`UObjectPaths.json`](../config/UObjectPaths.json) 读取。代码不再硬编码大部分路径/RVA，便于游戏更新后集中修正配置。

## Native / MinHook

### `UnrealWindow_WndProcThunk`

- 当前 RVA：`0x0130F6E0`。
- 安装位置：[`install_window_close_hook()`](../src/lifecycle.cpp)。
- detour：[`window_proc_detour()`](../src/lifecycle.cpp)。
- 触发：`WM_CLOSE` 或 `WM_SYSCOMMAND/SC_CLOSE`。
- 用途：窗口右上角关闭时执行一次 `WindowClose` 清理。
- 放行：调用原始 window proc trampoline。

### `SpineSkeletonAnimationExComponent::ReplaceSpineData`

- 当前 RVA：`0x04528710`。
- 安装位置：[`create_replace_hook()`](../src/spine.cpp)。
- 启用时机：进入 session 后 [`spine_add()`](../src/spine.cpp)。
- detour：[`replace_spine_detour()`](../src/spine.cpp)。
- 用途：检测是否切到 Lily skeleton，并在原函数前后做修正。
- 前置：Lily skeleton 先写 `ScaleFactor`、`DefaultMix`、`DefaultSkins`。
- 后置：启用动画名映射 hook、安排视觉翻转、补种 idle；离开 Lily 时禁用二级 hook 并恢复 scale。
- 重要限制：[`RVA.json`](../config/RVA.json) 中该 wrapper 校验期望是 `wrapper_should_call_native=false`，说明反射 wrapper 不一定 call 到此 native body；当前 hook 依赖真实换装 direct path。

### `SpineSkeletonAnimationComponent::SetAnimation`

- 当前 RVA：`0x043F2710`。
- 安装位置：[`create_component_hooks()`](../src/spine.cpp)。
- detour：[`component_set_detour()`](../src/spine.cpp)。
- 启用时机：只有确认当前 skeleton 是 Lily 后，由 [`enable_secondary_hooks()`](../src/spine.cpp) 启用。
- 用途：把 Lilac 动画名映射为 Lily 动画名。
- 命中范围：owner actor class 必须是 `BP_p0000_Lilac_C`。

### `SpineSkeletonAnimationComponent::AddAnimation`

- 当前 RVA：`0x043E8B90`。
- 安装位置：[`create_component_hooks()`](../src/spine.cpp)。
- detour：[`component_add_detour()`](../src/spine.cpp)。
- 用途：同上，覆盖排队动画。

### `SpineWidget::SetAnimation`

- 当前 RVA：`0x043F2B40`。
- 安装位置：[`create_widget_hooks()`](../src/spine.cpp)。
- detour：[`widget_set_detour()`](../src/spine.cpp)。
- 命中范围：widget 的 `Atlas` 对象路径中角色段为 `p0101_Lily`。
- 用途：覆盖 UI Spine widget 的动画名。

### `SpineWidget::AddAnimation`

- 当前 RVA：`0x043E8FD0`。
- 安装位置：[`create_widget_hooks()`](../src/spine.cpp)。
- detour：[`widget_add_detour()`](../src/spine.cpp)。
- 用途：同上，覆盖 UI Spine widget 的排队动画。

## UE4SS Callback Hooks

### Lifecycle `ProcessEvent` pre callback

- 安装位置：[`install_process_event_hook()`](../src/lifecycle.cpp)。
- HookName：`LifecycleProcessEvent`。
- 回调类型：`RegisterProcessEventPreCallback`。
- 识别函数：
  - `/Script/Engine.PlayerController:ClientRestart`
  - `/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnBackToTitleConfirm`
  - `/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnExitConfirm`
  - `/Game/_Zion/UI/Title/WBP_Title.WBP_Title_C:...Exit...OnPressed...`
- 用途：驱动 `Cold -> Armed -> Session -> Armed/Shutdown` 生命周期。

### Costume filter local script post callback

- 安装位置：[`install_filter_callback()`](../src/costume.cpp)。
- HookName：`CostumeFilter`。
- 回调类型：`RegisterProcessLocalScriptFunctionPostCallback`。
- 识别函数：
  - `WBP_Extra_Costume_C:CanDisplayCostumeData`
  - `WBP_Extra_CostumeEntry_C:IsCostumeOwned`
- 用途：当 row name 为 `p0101` 时强制 Blueprint bool 输出为 true。
- 参数处理：Blueprint bool 返回值按 `CPF_Parm | CPF_OutParm` 且非 `CPF_ReferenceParm` 查找，不按 `CPF_ReturnParm` 查找。

### Visual scale tick callback

- 安装位置：[`install_tick_callback()`](../src/spine.cpp)。
- HookName：`VisualPivotScaleWrite`。
- 回调类型：`RegisterEngineTickPreCallback`。
- 用途：延迟 8 帧写 `VisualPivotSceneComponent.RelativeScale3D`，避免刚换装时被游戏后续刷新覆盖。

## RVA wrapper 校验

[`rt_verify_rva_wrappers()`](../src/rva_check.cpp) 在首次进入 session 时执行一次。它读取 [`RVA.json`](../config/RVA.json) 的 `verify` 节，检查 UFunction wrapper 前 `0x200` 字节内是否有 `call` 指向配置的 native RVA。

当前期望：

| RVA 键 | wrapper 是否应 call native |
|---|---|
| `SpineSkeletonAnimationComponent_SetAnimation` | true |
| `SpineSkeletonAnimationComponent_AddAnimation` | true |
| `SpineWidget_SetAnimation` | true |
| `SpineWidget_AddAnimation` | true |
| `SpineSkeletonAnimationExComponent_ReplaceSpineData` | false |

这只是日志/健康检查，不会自动修复 RVA。

## 游戏更新后重新定位 `SpineSkeletonAnimationExComponent::ReplaceSpineData`

更新后重新锁定 `SpineSkeletonAnimationExComponent::ReplaceSpineData`，不要只从 `/Script/SpineExtension.SpineSkeletonAnimationExComponent:ReplaceSpineData` 的反射 wrapper 入手。当前版本已经确认该 wrapper 不一定会 `call` 真正的 direct native body；[`RVA.json`](../config/RVA.json) 中也把它标为 `wrapper_should_call_native=false`。

更稳的入口是先从 `UPlayerCostumeComponent` 的换装链路锁定 direct native call。

### 当前版本参考点

#### `UPlayerCostumeComponent::SetPendingCostumeID`

- `.h` 路径推测：`Source/Zion/Public/PlayerCostumeComponent.h`
- `.cpp` 路径推测：`Source/Zion/Private/PlayerCostumeComponent.cpp`
- UE4SS Dump 抓取情况：能看到 `UFUNCTION` 声明；`.cpp` 空实现只是 dump 生成桩，不代表真实源码为空。
- IDA native：`UPlayerCostumeComponent_SetPendingCostumeID`
- 当前 VA/RVA：`0x1447538E0` / `0x47538E0`
- 行为：只写 `this + 184` 的 pending costume id，不调用 `ApplyCostumeInternal`，也不会直接触发 `ReplaceSpineData`。

#### `UPlayerCostumeComponent::ApplyPendingCostume`

- `.h` 路径推测：`Source/Zion/Public/PlayerCostumeComponent.h`
- `.cpp` 路径推测：`Source/Zion/Private/PlayerCostumeComponent.cpp`
- UE4SS Dump 抓取情况：能看到 `UFUNCTION` 声明；`.cpp` 空实现只是 dump 生成桩。
- IDA native：`UPlayerCostumeComponent_ApplyPendingCostume`
- 当前 VA/RVA：`0x144732890` / `0x4732890`
- 行为：如果 `this + 184` 的 pending costume id 非空且不同于 `this + 176` 的 equipped costume id，则写入 equipped costume id，打印 `Equipping costume: %s`，随后调用 `UPlayerCostumeComponent_ApplyCostumeInternal`。

#### `UPlayerCostumeComponent::EquipCostume`

- `.h` 路径推测：`Source/Zion/Public/PlayerCostumeComponent.h`
- `.cpp` 路径推测：`Source/Zion/Private/PlayerCostumeComponent.cpp`
- UE4SS Dump 抓取情况：能看到 `UFUNCTION` 声明；`.cpp` 空实现只是 dump 生成桩。
- IDA native：`UPlayerCostumeComponent_EquipCostume`
- 当前 VA/RVA：`0x14473D870` / `0x473D870`
- 行为：直接把传入 costume id 写入 `this + 176` 的 equipped costume id，打印 `Equipping costume: %s`，随后调用 `UPlayerCostumeComponent_ApplyCostumeInternal`。

#### `UPlayerCostumeComponent::ApplyCostumeInternal`

- `.h` 路径推测：`Source/Zion/Public/PlayerCostumeComponent.h`
- `.cpp` 路径推测：`Source/Zion/Private/PlayerCostumeComponent.cpp`
- UE4SS Dump 抓取情况：通常抓不到；它不是反射导出的 `UFUNCTION`，但很可能是 `UPlayerCostumeComponent` 的 private/native helper。
- IDA native：`UPlayerCostumeComponent_ApplyCostumeInternal`
- 当前 VA/RVA：`0x144732000` / `0x4732000`
- caller：`UPlayerCostumeComponent_ApplyPendingCostume` 与 `UPlayerCostumeComponent_EquipCostume`
- 关键行为：加载/解析 costume 数据，取出 `NewAtlas`、`NewSkeletonData`、`NewNotifyAsset` 后，在 `0x1447323C2` 直接调用 `USpineSkeletonAnimationExComponent_ReplaceSpineData(SpineComp, NewAtlas, NewSkeletonData, NewNotifyAsset)`。

### 推荐锁定流程

1. 先在 UE4SS dump 出来的源码抓取目录中确认 `Source/Zion/Public/PlayerCostumeComponent.h` 仍有 `SetPendingCostumeID`、`EquipCostume`、`ApplyPendingCostume` 三个 `UFUNCTION`。`.cpp` 空实现不用当真，它只是 dump 生成占位桩；`ApplyCostumeInternal` 不是反射函数，正常情况下不会被抓取到。
2. 在 IDA 中搜索/确认 `UPlayerCostumeComponent_ApplyPendingCostume` 与 `UPlayerCostumeComponent_EquipCostume`。如果函数名丢失，可用字符串 `Equipping costume: %s` 找到二者。
3. 从 `ApplyPendingCostume` 或 `EquipCostume` 的内部 call 跟到共同目标；该共同目标就是新的 `ApplyCostumeInternal`。
4. 在 `ApplyCostumeInternal` 中搜索四参 direct call：`rcx = SpineComp`，`rdx = NewAtlas`，`r8 = NewSkeletonData`，`r9 = NewNotifyAsset`，随后 `call` 某个 SpineExtension 函数。
5. 这个 direct call 的目标就是更新后的 `SpineSkeletonAnimationExComponent::ReplaceSpineData` native body，等价于当前版本的 `0x4528710`。
6. 再用 `/Script/SpineExtension.SpineSkeletonAnimationExComponent:ReplaceSpineData` 的 wrapper 做交叉验证：如果 wrapper `call` 到该 native body，则是标准 wrapper；如果不 `call`，但出现写 `this+0xA0`、`this+0xA8`、`this+0x318`、清理 animation state、调用 vtable `+0x4A8` 等特征，则说明反射 wrapper 仍然内联/复制了 native body。

判断依据：`ApplyCostumeInternal` 是真实游戏换装路径里的内部函数，它在当前版本中没有反射导出，但由 `ApplyPendingCostume` 和 `EquipCostume` 两条公开 `UFUNCTION` 路径共同调用。只要新版本的游戏仍然通过 `UPlayerCostumeComponent` 应用服装，从 `ApplyCostumeInternal` 追踪到的四参 direct call 就比单纯扫描 `ReplaceSpineData` 反射 wrapper 更可靠。

## Reflection 路径清单

当前 lifecycle / costume filter 使用的关键反射路径：

- `/Game/_Zion/UI/RestMenu/Extra/WBP_Extra_Costume.WBP_Extra_Costume_C:CanDisplayCostumeData`
- `/Game/_Zion/UI/RestMenu/Extra/WBP_Extra_CostumeEntry.WBP_Extra_CostumeEntry_C:IsCostumeOwned`
- `/Script/Engine.PlayerController:ClientRestart`
- `/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnBackToTitleConfirm`
- `/Game/_Zion/UI/Pause/WBP_Pause.WBP_Pause_C:OnExitConfirm`
- `/Game/_Zion/UI/Title/WBP_Title.WBP_Title_C:BndEvt__WBP_Title_WBP_Button_Generic_Exit_K2Node_ComponentBoundEvent_0_OnPressed__DelegateSignature`

## 其他更新后重新定位建议

- `SetAnimation` / `AddAnimation`：优先用对应 `/Script/SpinePlugin...` UFunction wrapper 校验 call target。
- `Texture_UpdateResource`：不在本 Hook 文档中安装 hook，但作为直接调用 RVA 使用；见 [`RVA_RELOCATION.md`](RVA_RELOCATION.md)。
- 若某个 RVA 找不到，应在 [`RVA.json`](../config/RVA.json) 中留空/删除并接受对应功能失败日志，而不是写猜测值。
