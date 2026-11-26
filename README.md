# WelcomeToFlorida

在 Windows x64 上用 Kelvaroq CLI 和 cool_fox 渲染后端运行 Xbox Series 游戏。`Play-3D.cmd` 以 Series S 配置启动，并应用 cool_fox 的 `pareto-residency` 渲染参数。

## 构建和启动

准备 Windows 10 或 11 x64、支持 AVX2 的 CPU、支持 Shader Model 6.6 和 Mesh Shader 的 Direct3D 12 GPU，以及包含 `game_scarlett_master.exe` 的完整本地游戏目录。构建需要 Rustup、MSVC C++ 构建工具、Windows SDK、Python 3 和 .NET Framework 4.8。

在仓库根目录打开 PowerShell，按顺序构建后端和 CLI，再将后端放到 CLI 的 `native` 目录。

```powershell
Push-Location .\cool_fox
cargo build --locked --profile diag -p victory --features diagnostics
Pop-Location

cargo +1.98.1 build --locked --release --bin kelvaroq --manifest-path .\kelvaroq\Cargo.toml
$cliDir = Join-Path $PWD 'kelvaroq\target\release'
$nativeDir = Join-Path $cliDir 'native'
New-Item -ItemType Directory -Path $nativeDir -Force | Out-Null
Copy-Item -LiteralPath '.\cool_fox\target\diag\victory.exe' -Destination (Join-Path $nativeDir 'kelvaroq-engine.exe')
```

将下面的示例路径替换成自己的游戏目录。

```powershell
.\Play-3D.cmd 'D:\Games\My Xbox Game'
```

已有装入 cool_fox 后端的 Kelvaroq 便携包时，也可传入该目录。目录中应有 `kelvaroq-cli.exe` 和 `native\kelvaroq-engine.exe`。

```powershell
.\Play-3D.cmd 'D:\Games\My Xbox Game' 'D:\Tools\Kelvaroq-Florida'
```

默认存档路径为 `%LOCALAPPDATA%\WelcomeToFlorida\data`，着色器缓存路径为 `%LOCALAPPDATA%\WelcomeToFlorida\cache`。启动前设置 `VICTORY_DATA_DIR` 或 `VICTORY_CACHE_DIR` 可复用已有的本地目录。首次运行需要生成着色器缓存。

## 渲染验证

2026 年 11 月 26 日，在 RTX 5060 Laptop（8 GB 显存、驱动 `621.01`）上用该后端和 Series S 配置呈现了序章的 3D 人物、建筑和水面。该次序章约 1–3 FPS，仍出现人物脸部缺失和光照异常。日志检测到宿主 wave 宽度为 32，部分 Xbox Wave64 着色器未能转译。

## 源码与历史

| 路径 | 用途与版本 |
| --- | --- |
| [`kelvaroq/`](kelvaroq/) | Kelvaroq 启动器和参考实现，来自 [v0.2.0](https://github.com/fablinoo/kelvaroq/tree/v0.2.0)（`0a3c30d`）。 |
| [`cool_fox/`](cool_fox/) | 实测用的 VIctory 分支与原生渲染后端，版本 `f4b48ad`。该提交和父提交来自 [cool-fox-codec 镜像](https://github.com/ComputerScape04/cool-fox-codec/tree/f4b48ad766ddbec1a1867022cd42f7645b0dd55e)，源码树已与实测副本核对。 |
| [原有兼容性探针](docs/legacy-probe.md) | 本仓库中的 Scarlett 调试与启动实验记录。 |

两套源码均保留了提交及其祖先。可以从仓库根目录用 `git log kelvaroq-v0.2.0` 或 `git log pareto-residency-37a410b1` 查看相应历史。根目录的旧探针提交继续保留在 `main`。

## 许可证

根目录的 [LICENSE](LICENSE) 将自有兼容性探针和文档置于 Unlicense 下，[LICENSE-NOTICES.md](LICENSE-NOTICES.md) 说明各导入组件的授权边界。Kelvaroq 主体、cool_fox 和改编自 cool_fox 的 Play-3D.cmd 适用 GPL-3.0-or-later。[Kelvaroq 的研究模块](kelvaroq/research/LICENSE) 适用 GPL-2.0-or-later。

分发 GPL 组件的二进制时，请按对应版本的 GPL 提供完整对应源码，并保留许可证和版权声明。Kelvaroq 的名称和标识另有[上游说明](kelvaroq/README.md#name-and-logo)。

Microsoft DirectStorage SDK 适用 [随附条款和声明](vendor/directstorage-1.3.0/README.md)。BCPack 常量表的权利说明见 [cool_fox 的资产政策](cool_fox/docs/asset-policy.md#the-one-exception)。仓库原有的 runs/ 目录含日志、截帧和运行时文件，其中的第三方内容适用各自的权利条款。
