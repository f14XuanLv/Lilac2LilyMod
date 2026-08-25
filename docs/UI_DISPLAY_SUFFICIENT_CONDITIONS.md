# 拓展皮肤 UI 显示的充要条件

本文描述当前完全体中，让 `p0101` Lily 在 Rest Menu Costume tab 中出现、可选、可 Apply 的实际条件。旧文档中的 `src/hooks/reflection/Costume.cpp`、`data/DT_ItemCostumes_Extension.json` 等路径已过时。

## TL;DR

当前充要条件是：

| 条件 | 当前实现 | 位置 |
|---|---|---|
| C1：`DT_ItemCostumes` 运行时存在真实 row `p0101` | 以 `p0000` 为模板 `AddRow("p0101")` | [`add_lily_row()`](../src/costume.cpp) |
| C2：`p0101` 指向 forged Lily 资产 | 覆写 `AtlasAsset`、`SkeletonDataAsset`、`Icon` soft path | [`add_lily_row()`](../src/costume.cpp), [`UObjectPaths.json`](../config/UObjectPaths.json) |
| C3：名称/描述按 culture 覆写 | 读取 [`DT_ItemCostumes_Extension.json`](../config/DT_ItemCostumes_Extension.json)，写 `Name` / `Description` | [`apply_text_overrides()`](../src/costume.cpp) |
| C4：UI 显示判断返回 true | `CanDisplayCostumeData(p0101)` post callback 强制 bool out = true | [`filter_post_callback()`](../src/costume.cpp) |
| C5：Entry 拥有判断返回 true | `IsCostumeOwned(p0101)` post callback 强制 bool out = true | [`filter_post_callback()`](../src/costume.cpp) |

满足 C1-C5 后，UI 能显示 Lily entry、文本和图标正确、entry 不被灰掉，并能走游戏原生 Apply 换装链路。

## 不需要的东西

当前方案不需要：

- 写 `InventoryComponent::InventoryItems`。
- 写 `.sav`。
- hook native `InventoryComponent::HasItem`。
- hook `GetDataTableRowNames`。
- hook `GetItemCostumeData`。

## 生命周期位置

```text
Cold
  -> on_unreal_init
  -> Armed
  -> ClientRestart + 找到 BP_p0000_Lilac_C
  -> Session
       1. ensure_forged_assets
       2. costume_add
            2.1 load_text_table
            2.2 add_lily_row
            2.3 apply_text_overrides
            2.4 enable_session_filter
       3. spine_add
```

对应文件：

- 生命周期入口：[`lifecycle.cpp`](../src/lifecycle.cpp)
- UI/Costume 行注入：[`costume.cpp`](../src/costume.cpp)
- 资产伪造：[`asset_forge`](../src/asset_forge)
- 配置：[`UObjectPaths.json`](../config/UObjectPaths.json)

## DataTable 行注入细节

[`add_lily_row()`](../src/costume.cpp) 当前流程：

1. 确认在 game thread。
2. `StaticFindObject` 找到 `DT_ItemCostumes`。
3. 获取 `RowStruct`。
4. `FName("p0101", FNAME_Add)`。
5. 如果 `p0101` 已存在，直接视为成功。
6. 查找模板 row `p0000`。
7. `dt->AddRow(lily, template_row, row_struct)`。
8. 查找新 row。
9. 用反射字段名定位：
   - `AtlasAsset`
   - `SkeletonDataAsset`
   - `Icon`
10. 对 soft object path 内部 `FName` 槽写入 forged asset 路径：
    - `AtlasAsset` -> `/Engine/Transient.p0101_Lily-atlas`
    - `SkeletonDataAsset` -> `/Engine/Transient.p0101_Lily-data`
    - `Icon` -> `/Engine/Transient.SPR_Img_Costume_p0101`

当前 `Icon` 使用 forged PaperSprite，不再依赖静态 mod uasset 路线；旧静态路径只保留在代码注释中作为快速回滚参考。

## 文本覆写细节

文本来源：[`DT_ItemCostumes_Extension.json`](../config/DT_ItemCostumes_Extension.json)。

[`load_text_table()`](../src/costume.cpp) 读取所有 culture bucket，[`apply_text_overrides()`](../src/costume.cpp) 当前流程：

1. 确认在 game thread 且 `p0101` 已注入。
2. 调 `/Script/Engine.KismetInternationalizationLibrary:GetCurrentCulture`。
3. 查找：
   - `p0101.Name`
   - `p0101.Description`
4. fallback 顺序：
   - 精确 culture。
   - 去掉 `-` 后缀的 culture 前缀。
   - `en-US`。
   - 任意包含该 key 的 bucket。
5. 构造 `FText replacement{FString{value}}` 并赋值到 row 字段。

## Blueprint bool 输出参数

`CanDisplayCostumeData` 与 `IsCostumeOwned` 是 Blueprint local script 函数。当前代码不按 `CPF_ReturnParm` 找返回值，而是查找唯一的 bool out 参数：

```text
BoolProperty
+ CPF_Parm
+ CPF_OutParm
- CPF_ReferenceParm
```

`CanDisplayCostumeData` 的 handle 可能是引用参数，当前代码会优先从 `FOutParmRec` 取真实地址；`IsCostumeOwned` 则从 entry 实例字段 `CostumeHandle` 读取 row name。

## 为什么不写存档

当前只在运行时改变：

- `DT_ItemCostumes` 内存态 `RowMap`。
- Blueprint VM 当前调用栈上的 bool out 参数。
- 当前 row 的 `FText` 字段。

它不直接写 inventory，也不主动保存。玩家 Apply 后，游戏自己的当前装备字段是否进入存档属于原版 Apply 逻辑副作用，不是 mod 直接写入。

## 清理

返回标题：[`leave_session_for_title()`](../src/lifecycle.cpp)

- `costume_remove(DestroyRow)`：禁用 filter，调用 `RemoveRow` 删除 `p0101`。

退出/窗口关闭：[`cleanup_for_exit_event()`](../src/lifecycle.cpp)

- `costume_remove(ForgetRow)`：禁用 filter，直接从 `RowMap` 移除，避免退出阶段 `RemoveRow` 析构风险。

## 相关文件

- [`costume.cpp`](../src/costume.cpp)
- [`lifecycle.cpp`](../src/lifecycle.cpp)
- [`UObjectPaths.json`](../config/UObjectPaths.json)
- [`DT_ItemCostumes_Extension.json`](../config/DT_ItemCostumes_Extension.json)
- [`INJECTION_PATTERN.md`](INJECTION_PATTERN.md)
