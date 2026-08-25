# UE4SS 版本追踪与源码溯源

本文用于从 UE4SS Release Asset 的文件名，定位对应的源码 Commit，并构造源码 URL。

## 适用范围与版本固定原则

本文主要服务于需要固定 UE4SS 产物版本和源码版本的 UE4SS C++ Mod 开发。

对于 UE4SS C++ Mod，建议同时固定：

- UE4SS 的二进制产物；
- 与该产物对应的源码 Commit；
- Release Asset 文件名；
- Asset 的 SHA-256；
- 必要时保存对应的源码归档。

这样可以确保开发、构建、调试和问题复现时使用的是同一版本的 UE4SS 产物与源码。

对于 UE4SS Lua Mod 或蓝图 Mod，UE4SS 产物版本通常具备一定的兼容性和灵活性，一般不需要像 C++ Mod 那样严格追踪或关注 UE4SS 源码版本。不过，若项目依赖特定 UE4SS 功能、行为或修复，仍建议记录实际使用的 UE4SS Release 和 Asset 文件名。

因此，本文后续的版本追踪与源码溯源内容，重点适用于 UE4SS C++ Mod；Lua Mod 和蓝图 Mod 可根据实际依赖程度选择性使用。

## 1. Release 版本区别与使用建议

UE4SS 有两个需要区分的实验版本页面：

- 最新实验版本：<https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/experimental-latest>
- 实验版本归档：<https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/experimental>

### `experimental-latest`

`experimental-latest` 指向当前最新的实验构建。它是滚动版本，后续发布可能会更新其 Tag 和 Assets。

该版本的二进制可能不会长期保留，也不一定会进入 `/tag/experimental` 归档。如果不想自己重新编译 UE4SS，或者担心本地编译环境与官方环境不同、导致最终产物行为不同，应在看到目标版本后及时下载并保存对应的二进制。

### `experimental`

`experimental` 是实验版本归档，用于长期保留某个实验构建。它更适合：

- Mod 开发
- 固定开发环境
- 问题复现
- 二进制版本追踪

注意：归档只代表版本更适合固定和溯源，并不代表它已经成为正式稳定版。

### 选择建议

| 需求 | 建议 |
|---|---|
| 只想测试最新代码 | 使用 `experimental-latest` |
| 需要长期固定版本 | 使用 `experimental` |
| 不想自行编译 | 及时保存 `experimental-latest` 的目标二进制 |
| 需要复现官方二进制行为 | 保存二进制，并记录 Asset 文件名、Commit 和 SHA-256 |

## 2. 理解 Asset ZIP 文件名

打开上述任意 Release 页面，在 **Assets** 中查看 ZIP 文件名即可定位源码；不需要为了定位源码而下载 ZIP。文件名示例：

```text
zDEV-UE4SS_<version>.zip
```

常见文件类型：

| 文件名模式 | 用途 |
|---|---|
| `UE4SS_<version>.zip` | 普通运行时包 |
| `zDEV-UE4SS_<version>.zip` | Mod 开发和调试包 |
| `zCustomGameConfigs.zip` | 自定义游戏配置 |
| `zMapGenBP.zip` | Map Generator 相关文件 |

开发 Mod 通常选择 `zDEV-UE4SS_<version>.zip`。

文件名中的版本部分通常采用 Git describe 格式：

```text
v<tag>-<commits_ahead>-g<short_commit_id>
```

例如：

```text
UE4SS_vX.Y.Z-N-gXXXXXXXX.zip
```

含义是：

```text
vX.Y.Z       基础 Tag
N            相对于该 Tag 的提交数量
gXXXXXXXX    Git 短 Commit ID，其中 g 只是前缀
```

真正的短 Commit ID 是：

```text
XXXXXXXX
```

## 3. 提取短 Commit ID

从 Asset 文件名末尾提取 `g` 后面的部分，并去掉 `g`：

```text
gXXXXXXXX
↓
XXXXXXXX
```

如果文件名为：

```text
zDEV-UE4SS_vX.Y.Z-N-gXXXXXXXX.zip
```

则短 Commit ID 为：

```text
XXXXXXXX
```

这里的短 Commit ID 只是定位线索，不建议作为长期版本记录的唯一标识。

## 4. 获取长 Commit ID

可以直接在 GitHub 中搜索短 ID：

```text
https://github.com/UE4SS-RE/RE-UE4SS/commit/<SHORT_COMMIT_ID>
```

也可以使用 GitHub API：

```text
https://api.github.com/repos/UE4SS-RE/RE-UE4SS/commits/<SHORT_COMMIT_ID>
```

返回 JSON 中的：

```json
{
  "sha": "<FULL_COMMIT_ID>"
}
```

就是完整 Commit ID。

## 5. 获取 Asset 下载次数并按条件筛选

GitHub Release API 返回的每个 Asset 对象都包含以下字段：

```json
{
  "name": "zDEV-UE4SS_v3.0.1-940-g01e0a584.zip",
  "created_at": "2026-03-06T06:16:24Z",
  "updated_at": "2026-03-06T06:16:24Z",
  "download_count": 40
}
```

其中：

- `name`：Asset 文件名；
- `created_at`：Asset 上传时间，可用于筛选指定时间范围；
- `updated_at`：Asset 元数据最后更新时间，不表示最后下载时间；
- `download_count`：GitHub 统计的累计下载次数。

### 使用 PowerShell 查询

查询 `experimental` Release 下所有 `zDEV-UE4SS` 包，并按照下载量从高到低排序：

```powershell
$release = Invoke-RestMethod `
  -Uri "https://api.github.com/repos/UE4SS-RE/RE-UE4SS/releases/tags/experimental" `
  -Headers @{ "Accept" = "application/vnd.github+json"; "User-Agent" = "UE4SS-version-tracker" }

$release.assets |
  Where-Object { $_.name -like "zDEV-UE4SS_*.zip" } |
  Sort-Object download_count -Descending |
  Select-Object name, download_count, created_at
```

查询指定时间范围内上传的包，并按下载量降序排列。例如，查询 2026 年 3 月 1 日至 2026 年 7 月 1 日（不含 7 月 1 日）的包：

```powershell
$start = [datetime]"2026-03-01T00:00:00Z"
$end   = [datetime]"2026-07-01T00:00:00Z"

$release.assets |
  Where-Object {
    $_.name -like "zDEV-UE4SS_*.zip" -and
    ([datetime]$_.created_at -ge $start) -and
    ([datetime]$_.created_at -lt $end)
  } |
  Sort-Object download_count -Descending |
  Select-Object name, download_count, created_at
```

也可以直接使用 Release API 返回的 `assets` 数组，在 JSON 中查找：

```text
assets[].name
assets[].created_at
assets[].download_count
```

注意，`download_count` 是当前累计值，不是指定时间段内产生的下载量。例如，今天查询某个包得到的次数，包含该包从上传至今的所有统计；API 无法直接提供“某月下载了多少次”的历史分段数据。若需要进行历史比较，应定期保存查询结果和查询时间。

### 下载量高是否代表稳定

不能直接这样判断。下载量高通常说明该包更常被发现、推荐或使用，但不等于它更稳定。下载量会受到以下因素影响：

- 上传时间更早，累计时间更长；
- 是否是默认推荐版本或教程中使用的版本；
- 文件是否曾被重新上传或替换；
- 用户是否反复下载、测试或通过自动化流程下载；
- 新版本是否刚发布，尚未积累足够的下载量。

下载量可以作为“使用广泛程度”的参考指标，但不能作为稳定性的单一证据。选择用于开发或长期固定的 UE4SS 版本时，还应结合：

- 是否能稳定复现自己的游戏和 Mod 场景；
- UE4SS 仓库中的 Issue、修复记录和兼容性反馈；
- 是否存在影响自身功能的回归问题；
- 目标版本与当前游戏、编译器及 Mod API 的兼容性；
- 是否已经保存 Asset、SHA-256 和对应源码 Commit。

## 6. 构造最终 URL

获取完整 Commit ID 后，将它替换到以下 URL 中。

### 源码目录

```text
https://github.com/UE4SS-RE/RE-UE4SS/tree/<FULL_COMMIT_ID>
```

### Commit 页面

```text
https://github.com/UE4SS-RE/RE-UE4SS/commit/<FULL_COMMIT_ID>
```

### 源码 ZIP

```text
https://github.com/UE4SS-RE/RE-UE4SS/archive/<FULL_COMMIT_ID>.zip
```

### 指定源码文件

```text
https://raw.githubusercontent.com/UE4SS-RE/RE-UE4SS/<FULL_COMMIT_ID>/<PATH>
```

例如：

```text
https://raw.githubusercontent.com/UE4SS-RE/RE-UE4SS/<FULL_COMMIT_ID>/ue4ss/README.md
```

## 总结

定位源码不需要下载 Asset ZIP，只需要打开 Release 页面并查看 Asset 文件名：

```text
打开 Release 页面
    ↓
读取 Asset 文件名中的 g<SHORT_COMMIT_ID>
    ↓
通过 GitHub 或 Git 获取 <FULL_COMMIT_ID>
    ↓
构造 tree/commit/archive/raw URL
```

如果需要复现某个二进制版本，项目自己的版本记录中应保存：

```text
Release URL
Asset 文件名
完整 Commit ID
Asset SHA-256
```
