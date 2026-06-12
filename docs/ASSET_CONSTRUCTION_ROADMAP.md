# Lilac2LilyMod 资源管线方案编排

本文重新定义 [`Lilac2LilyMod`](../CMakeLists.txt) 的资源管线。核心问题是：如何让游戏在运行时获得 Lily 所需的 `UTexture2D`、`PaperSprite`、`SpineAtlasAsset`、`SpineSkeletonDataAsset` 和 `DT_ItemCostumes` 行，同时尽量降低迭代成本、跨版本维护成本和崩溃风险。

当前完成体属于 **方案 E：模板 UObject 复制 + 运行时局部 forge + 真实 DataTable 行注入**。

## 一、原始静态管线的痛点

传统静态资源路线需要：

| # | 环节 | 内容 | 代价 |
|---|---|---|---|
| ① | 对齐工程/插件环境 | 补齐 Ender Magnolia 使用的 UE/SpinePlugin 环境，并确保字段布局兼容 | 高；游戏更新后可能重做 |
| ② | 导入原始资源 | Editor 导入 `.png`、`.atlas`、`.skel` 为 `.uasset` | 中 |
| ③ | cook | 生成目标平台资源布局、BulkData 等 | 中高 |
| ④ | 打包 | 生成 `.pak/.utoc/.ucas` 三件套 | 中 |
| ⑤ | 挂载/加载 | 让游戏通过 pak loader / UE4SS 等加载资源 | 中 |

一次改资源或改动画，完整迭代常常需要重新导入、cook、打包和部署。当前项目的长期目标，就是把这些步骤拆成可替换的若干方案，按需求选择“最稳”“最高产品感”“最便携”或“最快能用”的路线。

## 二、方案总览

| 方案 | 名称 | 资源来源 | 是否高 UI 产品感 | 是否依赖 C++ runtime | 当前状态 |
|---|---|---|---|---|---|
| A | 静态方案 | Editor cook 后的 `.uasset/.pak/.utoc/.ucas` | 高 | 低/可选 | 历史基线 |
| B | C++ 全复刻动态构造 | DLL 从原始 `.png/.atlas/.skel` 构造完整 UObject | 高 | 强 | 未采用，理论上限最高 |
| C | 蓝图函数动态构造 | 先把“构造函数能力”做进 `.uasset` 蓝图，再跨游戏加载调用 | 中高 | 低到中 | 新增研究方向 |
| D | 最小内存读写方案 | 复用游戏现有对象，直接改关键字段/内存 | 低到中 | 强 | 可作为兜底 |
| E | 当前方案 | 复制模板 UObject，局部导入数据，真实注入 row | 高 | 强 | 当前完成体 |

## 三、方案 A：静态方案

### 定义

方案 A 是传统 UE mod 资源路线：先在匹配的 UE/插件工程里把 Lily 资源做成 `.uasset`，再 cook 和打包成 `.pak/.utoc/.ucas`，最后让游戏挂载这些包。

典型资源包括：

- `Content/_Zion/Characters/p0101_Lily/Spine/*`
- `Content/Mods/Lilac2LilyMod/UI/Costumes/SPR_Img_Costume_p0101_Lily.uasset`
- `Content/Mods/Lilac2LilyMod/UI/Costumes/T_Img_Costume_p0101_Lily.uasset`

### 优点

- 资产是 UE 原生 cook 产物，运行时风险最低。
- `SoftObjectPath`、DataTable、UMG、SpinePlugin 消费路径都最自然。
- 菜单里出现独立 Lily 条目，产品感强。

### 硬需求：SpinePlugin 头文件必须与目标游戏对齐

方案 A 的静态 cook 依赖工程内 SpinePlugin 反射定义与目标游戏使用的插件布局一致，尤其是这类头文件必须对齐：`GameProject/Plugins/SpinePlugin/Source/SpinePlugin/Public/SpineSkeletonDataAsset.h`。

从当前头文件可见，Ender Magnolia 制作组新增/魔改字段包括：`FMeshUpdateSettings`、`MeshUpdateSettings`、`ScaleFactor`、`DefaultSkins`、`FootIKAnimations`、`BreakSkins`。如果这些字段缺失、顺序错误或类型错误，Editor cook 出来的 `.uasset` 可能在游戏运行时被按错误布局解释，导致 skeleton 配置失效、动画异常、资源显示错误或崩溃。

### 缺点

- 迭代慢。
- 依赖 Editor、插件和 cook 环境。
- 强依赖 SpinePlugin 头文件与目标游戏二进制布局对齐。
- 游戏更新后插件字段/ABI 对齐可能再次成为成本。

### 适用场景

需要最稳的发行包，且不在意每次资源迭代成本时使用。

## 四、方案 B：C++ 全复刻动态构造方案

### 定义

方案 B 是纯 C++ runtime 从原始字节构造完整 UObject：DLL 直接读取 `.png/.atlas/.skel`，自己创建 `UTexture2D`、`SpineAtlasAsset`、`SpineSkeletonDataAsset` 等对象，并把它们注册到游戏可查找的对象路径。

理想链路：

```text
DLL 启动 / session ready
  -> 读取 p0101_Lily.png / .atlas / .skel
  -> NewObject 或 StaticConstructObject 创建 Texture2D
  -> 解码 PNG，填充 PlatformData，UpdateResource
  -> 创建 SpineAtlasAsset，写 rawData / atlasPages
  -> 创建 SpineSkeletonDataAsset，写 rawData / DefaultSkins / ScaleFactor
  -> Rename / 注册到目标路径
  -> DT_ItemCostumes 注入 p0101 row
```

### 优点

- 理论上完全省掉 Editor 导入、cook、打包。
- 可完全从外部原始数据驱动，开发迭代最快。
- 仍能保留独立 UObject 和原生 UI 条目。

### 难点

- 需要稳定构造 `UTexture2D` 的 runtime platform data 和 GPU resource。
- 需要理解/复刻 SpinePlugin 资产初始化逻辑。
- 可能需要 `SetRawData`、`UpdateResource`、`StaticConstructObject`、`Rename` 等 native 入口；某些 RVA 可能找不到或不稳定。
- 若使用 typed C++ 访问魔改字段，就重新依赖字段布局对齐。

### 当前状态

未采用为主线。当前代码只采用了其中一部分能力，例如 [`texture2d_forge.cpp`](../src/asset_forge/texture2d_forge.cpp) 手工构造 runtime platform data 并调用 `Texture_UpdateResource`。

## 五、方案 C：蓝图函数动态构造方案

### 定义

方案 C 是新增研究方向：把“动态构造 UObject 的能力”尽量写成纯蓝图函数，并把这个蓝图函数 cook 成 `.uasset`，再打包成 `.pak/.utoc/.ucas`。之后把这组三件套安装到其它同引擎版本游戏中，通过 UE4SS 或 pak loader 加载进内存，让目标游戏直接调用该蓝图函数完成运行时构造。

目标例子：实现一个纯蓝图函数：

```text
LoadExternalPngAsTexture2D(FilePath) -> UTexture2D
```

理想链路：

```text
开发侧 UE 工程
  -> 写蓝图函数库 / 蓝图资产
  -> 蓝图内实现：读外部文件、解析 PNG、构造 Texture2D 或写像素
  -> cook 成 .uasset
  -> 打包成 .pak/.utoc/.ucas

目标游戏
  -> 安装三件套
  -> UE4SS / pak loader 加载蓝图资产
  -> 调用蓝图函数
  -> 从外部 .png 构造 UTexture2D
```

### 核心价值

如果可行，它可能成为一种“跨同引擎版本的能力包”：

- 蓝图逻辑先落到 `.uasset`，避免每个游戏都写一份 C++ DLL。
- 对没有额外 C++ 依赖的 UObject 子类，可能都能用类似方式动态构造或填充。
- 一个能力包可被同 UE 版本、相近 API 暴露情况的多个游戏复用。

### 关键限制

- 纯蓝图是否能读取任意外部文件，取决于目标游戏暴露的 Blueprint API；Shipping 游戏通常不暴露足够文件 IO。
- 纯蓝图是否能从 PNG bytes 解码到像素，取决于是否有可用蓝图节点；没有节点就需要 C++/插件支持。
- 纯蓝图是否能创建并写入 `UTexture2D` 像素，也取决于运行时 API 暴露；很多能力在 C++ 可用但蓝图不可用。
- 若蓝图函数库本身依赖自定义 C++ class，则就不再是“纯蓝图能力包”。

### 适用场景

适合研究“跨游戏复用能力包”。如果目标游戏/引擎版本暴露足够蓝图节点，它比 C++ DLL 更便携；如果缺少节点，则需要退回方案 B 或方案 E。

## 六、方案 D：功能实现能用就好的最小内存读写方案

### 定义

方案 D 不追求高大上的 UI 和独立资产身份，只追求“皮肤就是实现了”。它复用游戏已加载的 Lilac 对象，直接写关键字段、rawData、texture 内存或 hook 关键换装点，让屏幕上表现为 Lily。

典型链路：

```text
找到当前 Lilac SpineAtlasAsset / SpineSkeletonDataAsset / UTexture2D
  -> 写入 Lily .skel / .atlas / .png 数据
  -> 清 cache / 调 UpdateResource / 触发组件重建
  -> 动画名映射
```

### 优点

- 工程量可能最小。
- 不需要构造独立 UObject。
- 如果只想让角色视觉变成 Lily，这是最快可用路线。

### 缺点

- 可能没有原生菜单新增条目。
- 产品感弱，更像“替换补丁”。
- 复用对象容易影响原本 Lilac 状态。
- 清理和切回需要格外小心。

### 适用场景

用于快速验证资源、骨骼、贴图、动画映射是否可用；也可作为其它方案失败时的兜底。

## 七、方案 E：当前方案

### 定义

方案 E 是当前完成体：不完全自己从零构造 UObject，也不走完整静态 cook 路线，而是 **用游戏已有模板 UObject 做壳，运行时复制出新对象，再局部导入 Lily 数据**。

当前实现入口：

- 生命周期：[`lifecycle.cpp`](../src/lifecycle.cpp)
- Costume 行注入：[`costume.cpp`](../src/costume.cpp)
- Spine hook：[`spine.cpp`](../src/spine.cpp)
- Texture forge：[`texture2d_forge.cpp`](../src/asset_forge/texture2d_forge.cpp)
- PaperSprite forge：[`paper_sprite_forge.cpp`](../src/asset_forge/paper_sprite_forge.cpp)
- SpineAtlas forge：[`spine_atlas_forge.cpp`](../src/asset_forge/spine_atlas_forge.cpp)
- SpineSkeletonData forge：[`spine_skeleton_data_forge.cpp`](../src/asset_forge/spine_skeleton_data_forge.cpp)
- UObject 模板复制：[`uobject_copy_forge.cpp`](../src/asset_forge/uobject_copy_forge.cpp)

### 当前链路

```text
on_unreal_init
  -> 读取 RVA.json / UObjectPaths.json
  -> 安装 lifecycle ProcessEvent hook
  -> 安装窗口关闭 hook

ClientRestart + 找到 BP_p0000_Lilac_C
  -> ensure_forged_assets
       -> StaticConstructObject(template=Texture2D/PaperSprite/SpineAsset)
       -> 导入 JSON 字段
       -> PNG 解码并写 Texture2D PlatformData
       -> Texture_UpdateResource
       -> Spine atlas rawData / atlasPages
       -> Spine skeleton rawData
  -> costume_add
       -> 读取 DT_ItemCostumes_Extension.json
       -> AddRow("p0101", template=p0000)
       -> patch AtlasAsset / SkeletonDataAsset / Icon
       -> patch Name / Description
       -> CanDisplayCostumeData / IsCostumeOwned 返回 true
  -> spine_add
       -> hook ReplaceSpineData
       -> Lily skeleton 前置配置
       -> hook SetAnimation / AddAnimation 做动画名映射
```

### 当前伪造对象

| 伪造对象 | 模板配置键 | forge 名称 | 用途 |
|---|---|---|---|
| Costume icon `Texture2D` | `CostumeTexture2DTemplate` | `T_Img_Costume_p0101` | UI 图标源贴图 |
| Costume icon `PaperSprite` | `CostumePaperSpriteTemplate` | `SPR_Img_Costume_p0101` | DataTable `Icon` |
| Lily spine `Texture2D` | `LilySpineTexture2DTemplate` | `p0101_Lily` | Spine atlas page |
| Lily `SpineAtlasAsset` | `LilySpineAtlasTemplate` | `p0101_Lily-atlas` | DataTable `AtlasAsset` |
| Lily `SpineSkeletonDataAsset` | `LilySpineSkeletonDataTemplate` | `p0101_Lily-data` | DataTable `SkeletonDataAsset` |

这些对象都创建在 `/Engine/Transient`，由 [`UObjectPaths.json`](../config/UObjectPaths.json) 控制。

### 优点

- 保留原生 UI 新条目和“创意工坊式”产品感。
- 不需要每次资源变更都 Editor cook 和打包。
- 比方案 B 少很多从零构造 UObject 的风险，因为模板对象提供了合法默认字段。
- 比方案 D 更完整，有独立 `p0101` row 和 forged asset 身份。

### 缺点

- 仍依赖 C++ DLL 和若干 RVA。
- `Texture_UpdateResource`、`ReplaceSpineData` 等 RVA 游戏更新后可能需要重新定位。
- forged UObject 放在 `/Engine/Transient`，不是完全等价于静态 `/Game/...` 资产。
- 不是纯蓝图能力包，跨游戏复用性弱于方案 C 的理想形态。

## 八、当前完成状态

当前代码完成了方案 E：

- 启动时读取 [`RVA.json`](../config/RVA.json) 和 [`UObjectPaths.json`](../config/UObjectPaths.json)。
- 进入 session 后运行时 forge Lily 所需对象。
- 真实注入 `DT_ItemCostumes` 的 `p0101` 行。
- 覆写 `p0101` 的 atlas、skeleton、icon、名称、描述。
- Hook UI 显示/拥有判断，让 `p0101` 可见且可用。
- Hook Spine 换装、动画播放和窗口关闭，保证视觉正确并安全清理。

## 九、RVA 风险说明

[`RVA.json`](../config/RVA.json) 当前记录：

- `UnrealWindow_WndProcThunk`
- `SpineSkeletonAnimationComponent_SetAnimation`
- `SpineSkeletonAnimationComponent_AddAnimation`
- `SpineWidget_SetAnimation`
- `SpineWidget_AddAnimation`
- `SpineSkeletonAnimationExComponent_ReplaceSpineData`
- `Texture_UpdateResource`

这些 RVA 是当前二进制版本的结果。游戏更新后可能失效。尤其 `Texture_UpdateResource` 和 `ReplaceSpineData` 一类非反射稳定入口，可能需要重新定位；如果某个 RVA 找不到，应明确记录“可能找不到/本版本不可用”，不要填猜测值。
