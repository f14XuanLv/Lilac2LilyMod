# Lily Spine 卡顿态修复结论

本文记录当前 [`spine.cpp`](../src/spine.cpp) 中 Lily Spine 切换稳定性的关键修复。旧版结论仍基本成立，但路径和函数名已更新。

## 核心结论

`p0101` 的 `SpineSkeletonDataAsset` 必须在游戏原始 `ReplaceSpineData` 执行之前完成 Lily 专用配置。

当前关键代码位于 [`replace_spine_detour()`](../src/spine.cpp)：

```cpp
void __fastcall replace_spine_detour(void* self, void* atlas, void* skel, void* notify) {
    bool to_lily = is_lily_skeleton(skel);
    if (to_lily) configure_lily_skeleton(skel);
    if (g_replace_original) g_replace_original(self, atlas, skel, notify);
    after_replace_spine_data(skel);
}
```

也就是：

```text
configure_lily_skeleton(skel)  // 前置写 Lily 配置
g_replace_original(...)        // 游戏原始 ReplaceSpineData 重建 Spine 状态
after_replace_spine_data(skel) // 后置启用动画映射/视觉翻转/idle
```

## 需要提前写入的配置

[`configure_lily_skeleton()`](../src/spine.cpp) 写入：

```text
ScaleFactor = 0.26f
DefaultMix = 0.1f
DefaultSkins = ["_common", "_Meat_Head_0", "p0101_Lily"]
```

这些值不能只在 `ReplaceSpineData` 返回后写。原始 native 函数内部会消费 `SkeletonDataAsset` 并创建/刷新 native Spine 状态；如果第一次消费时配置还是模板值，就可能出现卡顿态或 skin/state 不正确。

## 为什么前置写有效

当前 forged `SpineSkeletonDataAsset` 是从 Lilac 模板复制出来，再由 [`spine_skeleton_data_forge.cpp`](../src/asset_forge/spine_skeleton_data_forge.cpp) 导入 Lily 的 `.skel` rawData。模板字段不一定完全适合 Lily，特别是：

- `ScaleFactor`
- `DefaultMix`
- `DefaultSkins`

原始 `ReplaceSpineData` / Spine component 刷新链路会基于这些字段构建 native `spine::SkeletonData`、native `spine::AnimationStateData` 和 custom skin / setup pose 相关状态。

## 后置处理仍然需要

[`after_replace_spine_data()`](../src/spine.cpp) 在原函数返回后执行：

- 如果是 Lily skeleton：
  - 再次 `configure_lily_skeleton(skel)`，作为保险。
  - [`enable_secondary_hooks()`](../src/spine.cpp) 启用 component/widget 动画映射。
  - [`schedule_visual_scale(-1, 1, 1)`](../src/spine.cpp) 延迟 8 帧翻转 VisualPivot。
  - [`seed_idle_animation()`](../src/spine.cpp) 用反射调用 `SetAnimation("idle")`。
- 如果不是 Lily skeleton：
  - 禁用动画映射 hook。
  - 延迟恢复 VisualPivot scale `(1, 1, 1)`。

## 卡顿态的本质

错误顺序：

```text
游戏原始 ReplaceSpineData 先重建 native Spine 状态
之后 mod 才写 Lily SkeletonDataAsset 配置
```

此时 UObject 字段后续看起来正确，但当前正在使用的 native `Skeleton` / `AnimationState` / custom skin 可能已经按旧配置创建。

正确顺序：

```text
mod 先写 Lily SkeletonDataAsset 配置
游戏原始 ReplaceSpineData 用正确配置重建 native Spine 状态
mod 后置启用动画映射和视觉修正
```

## 当前不再使用的旧策略

- 没有热键刷新路径。
- 没有 `spine::AnimationState::setCurrent` hook。
- 没有调用 `USpineSkeletonDataAsset::SetRawData`；当前 rawData 由 forge 写 TArray，切换稳定性主要靠前置配置和原始换装刷新链路。

## 相关文件

- Hook 与动画映射：[`spine.cpp`](../src/spine.cpp)
- 生命周期启用/禁用：[`lifecycle.cpp`](../src/lifecycle.cpp)
- SkeletonData 伪造：[`spine_skeleton_data_forge.cpp`](../src/asset_forge/spine_skeleton_data_forge.cpp)
- RVA 配置：[`RVA.json`](../config/RVA.json)
- 动画名映射表：[`Lilac2LilyAnimationMap.json`](../config/Lilac2LilyAnimationMap.json)
