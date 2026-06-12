# RVA RELOCATION

本文集中记录 [`Lilac2LilyMod`](../CMakeLists.txt) 依赖 RVA 的重新定位方法。原则：游戏更新后如果 RVA 找不到，不要写猜测值；应让对应功能失败并输出日志，再按本文重新定位。

## 当前 RVA 配置

当前 [`RVA.json`](../config/RVA.json) 记录：

```json
{
  "UnrealWindow_WndProcThunk": "0x0130F6E0",
  "SpineSkeletonAnimationComponent_SetAnimation": "0x043F2710",
  "SpineSkeletonAnimationComponent_AddAnimation": "0x043E8B90",
  "SpineWidget_SetAnimation": "0x043F2B40",
  "SpineWidget_AddAnimation": "0x043E8FD0",
  "SpineSkeletonAnimationExComponent_ReplaceSpineData": "0x04528710",
  "Texture_UpdateResource": "0x03C20A80"
}
```

模块：`EnderMagnoliaSteam-Win64-Shipping.exe`  
当前 ImageBase 记录：`0x140000000`

## 一、`Texture_UpdateResource`

### 当前确认结果

当前版本中：

```json
{
  "Texture_UpdateResource": "0x03C20A80"
}
```

对应 IDA VA：

```text
0x143C20A80
```

计算关系：

```text
0x143C20A80 - 0x140000000 = 0x03C20A80
```

### 该函数是什么

`0x03C20A80` 更适合命名为 `Texture_UpdateResource`。它更接近 Unreal Engine 的 `UTexture::UpdateResource()`，而不是 `UTexture2D::UpdateResource()` 的薄 override。

原因：该函数是单参数函数：

```cpp
__int64 __fastcall sub_143C20A80(__int64 a1)
```

它会执行创建/绑定 GPU resource 的核心流程，包括：

- 检查渲染环境是否可用，例如 `nullrhi`。
- 跳过 `RF_ClassDefaultObject`。
- 虚调用 `CreateResource()`。
- 设置 texture/resource 指针。
- enqueue render-thread command。
- 设置 `TextureReference`。
- 处理 block-compressed texture 的尺寸合法性。

这正是“从 CPU-side texture/platform data 生成 GPU resource”的入口。

### 推测对应 Unreal Engine 源码位置

参考 UE5.4 语义，源码调用链应记录为：

```text
Texture2D.cpp:716  UTexture2D::UpdateResource()
  -> Texture2D.cpp:720  WaitForPendingInitOrStreaming()
  -> Texture2D.cpp:739-761  shipping/runtime OnlyFirstMip mip 裁剪
  -> Texture2D.cpp:764  Super::UpdateResource()
       -> Texture.cpp  UTexture::UpdateResource()  // 当前二进制 RVA 0x03C20A80
```

`0x03C20A80` 的伪代码特征是“单参数、检查 `nullrhi`、跳过 `RF_ClassDefaultObject`、虚调用 `CreateResource()`、写入 resource 指针、enqueue render-thread 初始化”。这些特征对应父类 `UTexture::UpdateResource()` 的实际资源重建逻辑。

### 为什么不是直接找 `Texture2D.cpp` 字符串

当前二进制中没有找到：

```text
T:\Engine\Source\Runtime\Engine\Private\Texture2D.cpp
Texture2D.cpp
```

但能找到一些相邻/相关路径，例如：

```text
T:\Engine\Source\Runtime\Engine\Private\Rendering\Texture2DArrayResource.cpp
T:\Engine\Source\Runtime\Engine\Classes\Engine\Texture.h
```

说明该 shipping binary 保留了部分源码路径，但没有保留 `Texture2D.cpp` 路径。所以更新后不能依赖 `Texture2D.cpp` 字符串定位。

### 重新定位方法 A：从 `Texture.h` 字符串和 `UTexture` vtable 附近定位

1. 在 IDA 字符串窗口搜索：

```text
T:\Engine\Source\Runtime\Engine\Classes\Engine\Texture.h
```

2. 找到该字符串后，查看它附近的 `UTexture` 相关字符串，例如：

```text
UTexture::GetTextureClass
UTexture::CreateResource
```

3. 这些字符串附近通常就是 `UTexture` 的 vtable / RTTI / assert stub 区域。
4. 在附近数据区寻找 `UTexture` vtable 函数指针数组。
5. 找到包含大量 `0x143C....` 这类函数地址的表后，重点找类似当前版本 `0x143C20A80` 的函数。

它在当前版本中的特征是：

- 位于 `UTexture` 相关函数集中。
- 函数较长。
- 单参数。
- 内部引用 `nullrhi`。
- 内部引用 `SetTextureReference`。
- 内部引用 `SetResourceRenderThread`。

### 重新定位方法 B：直接按字符串交叉引用定位

在 IDA 搜索字符串：

```text
SetTextureReference
```

然后查看 xrefs。

目标函数应同时满足：

- 包含 `SetTextureReference`。
- 包含 `SetResourceRenderThread`。
- 包含 `nullrhi` 检查或附近初始化 `nullrhi` 静态变量。
- 有对 `this + 272` 和 `this + 280` 的写入。
- 有虚调用 `(*vtable + 824)`，疑似 `CreateResource()`。
- 有 render-thread direct path 和 enqueue path 两套逻辑。

当前版本中这些特征集中在：

```text
VA  0x143C20A80
RVA 0x03C20A80
```

### 重新定位方法 C：按伪代码特征定位

如果字符串被裁剪，可以在 IDA 中搜索/筛选伪代码特征。

目标函数典型伪代码片段：

```cpp
sub_143C1E380();

// nullrhi / can render check
if ((!byte_... || byte_...) && !byte_...)
{
    // RF_ClassDefaultObject check
    if ((*(_DWORD *)(a1 + 8) & 0x10) == 0)
    {
        // virtual call: CreateResource()
        v4 = (*(__int64 (__fastcall **)(__int64))(*(_QWORD *)a1 + 824LL))(a1);

        if (v4)
        {
            // assign game-thread resource
            *(_QWORD *)(a1 + 272) = v4;

            if (IsInRenderingThread())
            {
                // assign render-thread resource
                *(_QWORD *)(a1 + 280) = v4;

                // init resource / set texture reference immediately
                (*(void (__fastcall **)(_QWORD *, __int64))(*v4 + 56LL))(v4, RHICmdList);
            }
            else
            {
                // enqueue SetTextureReference
            }
        }
        else
        {
            *(_QWORD *)(a1 + 272) = 0;

            // enqueue SetResourceRenderThread null
        }
    }
}
```

如果新版本函数仍是 UE5.4 同类构建，以上结构通常非常稳定。

### 快速校验点

找到候选函数后，建议至少确认：

1. 函数签名是单参数 `this`：

```cpp
__int64 __fastcall sub_xxx(__int64 a1)
```

2. 早期有 `nullrhi` 检查。
3. 有对象 flag 检查：

```cpp
(*(_DWORD *)(a1 + 8) & 0x10) == 0
```

4. 有虚调用 `CreateResource()`：

```cpp
(*(__int64 (__fastcall **)(__int64))(*(_QWORD *)a1 + 824LL))(a1)
```

5. 有资源指针写入：

```cpp
*(_QWORD *)(a1 + 272) = v4;
*(_QWORD *)(a1 + 280) = v4;
```

6. 有 render-thread command 名称：

```text
SetTextureReference
SetResourceRenderThread
```

满足这些条件后，基本可以确认它就是新的 `Texture_UpdateResource`。

### 当前代码使用方式

当前调用位置是 [`call_texture_update_resource()`](../src/asset_forge/texture2d_forge.cpp)，由 [`asset_forge_add_texture2d()`](../src/asset_forge/texture2d_forge.cpp) 间接触发：

```text
asset_forge_add_texture2d
  -> uobject_copy_forge_add                 // 用模板 Texture2D 构造 /Engine/Transient.<Name>
  -> validate_texture2d_shell               // 确认对象是 Texture2D，并 root
  -> set_texture2d_from_json                 // 导入反射字段
  -> attach_texture2d_png_platform_data
       -> decode_png_bgra8                  // Windows WIC 解码 PNG 为 BGRA8
       -> attach_decoded_platform_data       // 手工写 RuntimeFTexturePlatformData
       -> call_texture_update_resource       // 调 module_base + 0x03C20A80
```

当前调用签名：

```cpp
using UpdateResourceFn = void(__fastcall*)(UObject*);
```

它对当前运行时伪造链路是必需的：只写 `PrivatePlatformData` 仍可能导致 sprite / spine atlas page 透明或沿用旧 GPU resource；调用后才会创建/刷新可渲染资源。

## 二、`SpineSkeletonAnimationExComponent::ReplaceSpineData`

### 当前确认结果

当前版本中：

```json
{
  "SpineSkeletonAnimationExComponent_ReplaceSpineData": "0x04528710"
}
```

对应 VA：

```text
0x14528710
```

### 重要限制

不要只从 `/Script/SpineExtension.SpineSkeletonAnimationExComponent:ReplaceSpineData` 的反射 wrapper 入手。当前版本已经确认该 wrapper 不一定会 `call` 真正的 direct native body；[`RVA.json`](../config/RVA.json) 中也把它标为：

```json
"wrapper_should_call_native": false
```

因此，`ReplaceSpineData` 更新后更稳的入口是从 `UPlayerCostumeComponent` 的换装链路锁定 direct native call。

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

## 三、`SetAnimation` / `AddAnimation` 系列

这些函数通常可以优先用对应 `/Script/SpinePlugin...` UFunction wrapper 校验 call target。当前 [`RVA.json`](../config/RVA.json) 的 `verify` 节期望：

| RVA 键 | wrapper 是否应 call native |
|---|---|
| `SpineSkeletonAnimationComponent_SetAnimation` | true |
| `SpineSkeletonAnimationComponent_AddAnimation` | true |
| `SpineWidget_SetAnimation` | true |
| `SpineWidget_AddAnimation` | true |
| `SpineSkeletonAnimationExComponent_ReplaceSpineData` | false |

[`rt_verify_rva_wrappers()`](../src/rva_check.cpp) 在首次进入 session 时执行一次。它检查 UFunction wrapper 前 `0x200` 字节内是否有 `call` 指向配置的 native RVA。这只是日志/健康检查，不会自动修复 RVA。
