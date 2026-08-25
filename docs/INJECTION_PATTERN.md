# Injection Pattern · 当前项目实际范式

本文描述当前 [`Lilac2LilyMod`](../CMakeLists.txt) 完全体实际采用的注入范式。旧版文档强调“深拷贝 + Rename + 虚拟/独立路径”，但当前代码落地的是更具体的混合范式：

> **配置驱动定位 → 复制模板 UObject/Row → 局部导入运行时数据 → 注入真实 DataTable 行 → Hook 消费方返回值/刷新点 → 生命周期清理**

## 一、当前范式概览

当前项目有两类注入：

| 类型 | 当前实现 | 代表代码 |
|---|---|---|
| UObject 资产伪造 | 用模板 UObject 作为 `Template` 调 `StaticConstructObject`，创建 `/Engine/Transient.<Name>` 对象，再导入 JSON/PNG/SKEL 数据 | [`uobject_copy_forge_add()`](../src/asset_forge/uobject_copy_forge.cpp) |
| DataTable 行注入 | 用 `DT_ItemCostumes` 的 `p0000` 行为模板，真实 `AddRow("p0101")`，再覆写软路径和文本 | [`add_lily_row()`](../src/costume.cpp) |

当前没有实现：

- `StaticDuplicateObject` 深拷贝整对象树。
- `Rename` 到 `/Game/.../p0101_Lily/...` 包路径。
- 完全虚拟的 DataTable row。
- hook `FSoftObjectPath::TryLoad`。

## 二、UObject 资产伪造范式

### 1. 配置驱动定位

所有模板、输出名字和路径都来自 [`UObjectPaths.json`](../config/UObjectPaths.json)：

- `objects.ModForgeOuter = /Engine/Transient`
- `objects.CostumeTexture2DTemplate`
- `objects.CostumePaperSpriteTemplate`
- `objects.LilySpineTexture2DTemplate`
- `objects.LilySpineAtlasTemplate`
- `objects.LilySpineSkeletonDataTemplate`
- `names.*ForgeName`

### 2. 复制模板 UObject

[`uobject_copy_forge_add()`](../src/asset_forge/uobject_copy_forge.cpp) 的流程：

1. 查找 outer：通常是 `/Engine/Transient`。
2. 若目标对象已存在，则 root 后复用。
3. 查找模板对象；若没加载，则走 AssetRegistry 反射加载。
4. 调 `StaticConstructObject`，设置 `Class`、`Outer`、`Name`、`Template`、`RF_Public | RF_Standalone | RF_MarkAsRootSet` 和 `RootSet`。
5. root 新对象。

这不是 `StaticDuplicateObject`，但达成了“以游戏已加载/可加载模板提供合法默认字段”的目标。

### 3. 局部导入差异

不同资产类型有不同 forge：

| forge | 差异数据 | 关键处理 |
|---|---|---|
| [`texture2d_forge.cpp`](../src/asset_forge/texture2d_forge.cpp) | JSON 反射字段 + PNG | WIC 解码、手写 platform data、调用 `Texture_UpdateResource` |
| [`paper_sprite_forge.cpp`](../src/asset_forge/paper_sprite_forge.cpp) | JSON 反射字段 | 修正 `BakedSourceTexture` 指针 |
| [`spine_atlas_forge.cpp`](../src/asset_forge/spine_atlas_forge.cpp) | JSON `atlasFileName` / `rawData` | 写 `atlasPages` 指向 forged texture，并清 native atlas cache |
| [`spine_skeleton_data_forge.cpp`](../src/asset_forge/spine_skeleton_data_forge.cpp) | JSON 字段 + `.skel` bytes | 写 `rawData` TArray |

核心原则：只改必要字段，其余字段继承模板。

### 4. GC/生命周期

当前 forged UObject 被设置为 public/standalone/root set，生命周期基本覆盖进程。对手工分配的 Texture2D platform data，项目自行持有并在退出/关闭窗口前释放：

- 注册清理：[`rt_add_cleanup()`](../src/lifecycle.cpp)。
- 释放函数：[`asset_forge_release_texture2d_platform_data()`](../src/asset_forge/texture2d_forge.cpp)。

## 三、DataTable 行注入范式

[`add_lily_row()`](../src/costume.cpp) 的实际流程：

1. 确认在 game thread。
2. 找到 `DT_ItemCostumes`。
3. 获取 `RowStruct`。
4. 用 `p0000` 作为模板行。
5. `AddRow("p0101", template_row, row_struct)`。
6. 找到新 row 后覆写 `AtlasAsset`、`SkeletonDataAsset`、`Icon`。
7. 写文本 `Name`、`Description`。
8. 启用 UI filter，使 `p0101` 显示/拥有判断为 true。

## 四、消费方 Hook 范式

当前不是让所有系统自然刷新，而是在最小消费边界打补丁：

| 消费方 | Hook | 目的 |
|---|---|---|
| 生命周期 | `ProcessEvent` pre | 找进入/离开 session 的安全窗口 |
| UI 显示 | local script post | `CanDisplayCostumeData(p0101)=true` |
| UI 拥有 | local script post | `IsCostumeOwned(p0101)=true` |
| 换装 | MinHook `ReplaceSpineData` | Lily skeleton 前置配置、启用动画映射 |
| 动画 | MinHook `SetAnimation/AddAnimation` | Lilac 动画名映射到 Lily |
| 贴图 GPU 刷新 | 直接 RVA 调用 | 让手写 platform data 显示 |
| 退出 | window proc hook | 在 UE 析构前释放自持内存 |

## 五、当前清理策略

### 返回标题

[`leave_session_for_title()`](../src/lifecycle.cpp)：

- `costume_remove(DestroyRow)`：禁用 filter，调用 `UDataTable::RemoveRow` 删除 `p0101`。
- `spine_remove()`：禁用 Spine hooks 和 tick callback。

### 退出/窗口关闭

[`cleanup_for_exit_event()`](../src/lifecycle.cpp)：

- `spine_remove()`。
- `costume_remove(ForgetRow)`：禁用 filter，直接 `RowMap.Remove`，避免退出阶段 `RemoveRow` 析构风险。
- `cleanup_forged_assets_for_exit()`：释放 runtime Texture2D platform data。

## 六、新增注入模块 checklist

1. 是否能从 [`UObjectPaths.json`](../config/UObjectPaths.json) 或新增配置中取得所有路径？
2. 是否有合法模板对象/模板 row？
3. 模板对象未加载时是否需要 AssetRegistry 兜底？
4. 差异字段是否能用 `FProperty` / `ImportText` / TArray 写入？
5. 是否需要直接 RVA 调用刷新 native/GPU cache？如果需要，RVA 找不到时如何失败？
6. 是否有最小消费方 hook，而不是扩大影响面？
7. 返回标题和进程退出分别如何清理？
8. 是否有自持内存或 root 对象需要释放/解绑？

未回答这些问题，不应新增注入模块。
