# HEVC10Output — UE 5.8 10-bit H.265 MP4 输出插件

## 它解决什么问题

UE 5.8 自带的 MP4 输出是 **H.264 8-bit**，而且 `UMovieGraphMP4EncoderNode` 的默认码率写死为
`AverageBitrateInMbps = 8`（1080p 的取值），拿去做 4K60 会糊成一片。

这个插件给 Movie Render Graph 加一个 **H.265 HEVC Main 10** 输出节点：

```
MRQ 16-bit 量化 → FFloat16Color (半精度 RGBA, 8B/px)
   → ffmpeg stdin: -f rawvideo -pix_fmt rgbaf16le
   → hevc_nvenc -profile:v main10 -pix_fmt p010le  →  out.mp4
```

关键点：**帧通过 stdin 管道直接喂给 ffmpeg，渲染的同时编码，没有任何中间图像序列文件。**
对比 EXR 中间格式方案（4K 整片约 90 GB 中间文件 + 渲染结束后串行编码 +30% 时长），
这个方案磁盘占用为 0，编码与渲染并行。

---

## 怎么用

1. 打开 **影片渲染队列**，选中你的 Job
2. 让它使用 **Graph 配置**（`ConfigMode = Graph`），而不是 Basic 模式
   —— 自定义节点只出现在 Movie Render Graph 里
3. 在 Graph 里添加节点 **`H.265 HEVC 10-bit MP4`**（分类 `Output Type`）
4. 接线：`Output Settings → H.265 HEVC 10-bit MP4 → Output 节点的 Globals 引脚`
5. 节点参数：

| 参数 | 说明 |
|---|---|
| `Encoder` | `NVENC`（GPU，快）或 `X265`（CPU，同码率画质更好但慢很多） |
| `Quality` | 越低越好越大。NVENC 走 `-cq`，x265 走 `-crf`。**16 ≈ 近无损，19 高，23 中** |
| `FFmpeg Path` | **留空即查 `PATH`**（正常安装这样就够）。自动找不到时点右边的 📁 选文件，或填绝对路径 |
| `ExtraArguments` | 附加原始 ffmpeg 参数，一般留空 |

> `FFmpeg Path` 是 `FFilePath` 类型，所以细节面板里有**文件选择按钮**，不用凭记忆手打路径。
> 留空是有意义的——它表示"自己去 `PATH` 上找 `ffmpeg.exe`"。

### 实测数据（RTX 4070 Ti SUPER）

| 分辨率 | 渲染速度 | 说明 |
|---|---|---|
| 4K (3840×2160) | 2.33 fps | 瓶颈在渲染，不在编码 |
| 320×180 | ~40 fps | 6214 帧 / 2 分 36 秒 |

---

## ⚠️ 已知限制：**没有音频**

`IsAudioSupported()` 返回 `false`，输出是**无声视频**。
因为把音频接进来需要在插件里多写一条 PCM 管道，而改插件要重新编译 —— 见下面的"重编"说明。

**解决办法**：MRQ 里同时加一个 `.wav` 输出，然后用 ffmpeg 合一下（秒完成，不重编码视频）：

```powershell
ffmpeg -i "NewLevelSequence.beauty.mp4" -i "NewLevelSequence.wav" `
  -c:v copy -c:a aac -b:a 320k -shortest "final_10bit.mp4"
```

`Tools\ToHEVC10.ps1` 是备用的转码脚本（ProRes/任意输入 → 10-bit MP4）。

---

## 🔧 重编插件（重要）

**当前工程是纯蓝图工程，插件以预编译二进制形式加载。**
这么做是**被迫的**：`MMD2Unreal` 是只发二进制、不带源码的插件（`Plugins/MMD2Unreal/Source/`
里只有 `Build.cs`，没有实现代码）。一旦把工程变成 C++ 工程，UBT 就会把它当源码模块
**重新编译成一个 48 KB 的空壳 DLL**，覆盖掉原本 446 KB / 3306 KB 的真货，导致插件初始化失败。

原始二进制备份在工程之外（例如 `%USERPROFILE%\MMD2Unreal\Binaries\Win64\`）。

### 如果要改插件代码并重编

```powershell
# 以下命令都从工程根目录执行
cd <你的工程目录>

# 1. 停用 MMD2Unreal 的 Source，避免被编成空壳
Rename-Item "Plugins\MMD2Unreal\Source" "Source.disabled"

# 2. 恢复工程 C++ 骨架（见下）和插件源码
Rename-Item "Plugins\HEVC10Output\Source.disabled" "Source"

# 3. 编译（<UE> 指你的 UE 5.8 安装目录）
& "<UE>\Engine\Build\BatchFiles\Build.bat" `
    YourProjectEditor Win64 Development -Project="<你的工程>\YourProject.uproject" -WaitMutex

# 4. 还原：插件源码改名回去、恢复 MMD2Unreal 的 Source
Rename-Item "Plugins\HEVC10Output\Source" "Source.disabled"
Rename-Item "Plugins\MMD2Unreal\Source.disabled" "Source"
```

> 本工程里 `Tools\build_env.ps1` 已经把上面 1、2、4 步和工程骨架都包好了，
> 用 `-Mode enable` / `-Mode disable` 一条命令完成。

第 2 步的"工程 C++ 骨架"指：

- `MMD_test.uproject` 里加回 `"Modules"` 段（`MMD_test`，Runtime/Default）
- 建 `Source/MMD_test.Target.cs`、`Source/MMD_testEditor.Target.cs`、`Source/MMD_test/`（空模块）

编译完再从 `.uproject` 移除 `Modules` 并删掉 `Source/`，工程就回到纯蓝图状态。
（这套骨架本次已经验证可用，需要时可以让我重新生成。）

---

## 文件结构

```
Plugins/HEVC10Output/
  HEVC10Output.uplugin                      插件描述（Installed: true = 二进制插件）
  Binaries/Win64/UnrealEditor-HEVC10Output.dll   ← 实际加载的东西
  Source.disabled/                          源码（改名即可参与编译）
    HEVC10Output/HEVC10Output.Build.cs
    HEVC10Output/Public/HEVC10FFmpegPipe.h          ffmpeg 进程 + stdin 管道封装
    HEVC10Output/Private/HEVC10FFmpegPipe.cpp
    HEVC10Output/Public/Graph/MovieGraphHEVC10Node.h    MRG 输出节点
    HEVC10Output/Private/Graph/MovieGraphHEVC10Node.cpp
```

核心实现只有两处：

- `FHEVC10FFmpegPipe` — 用 `FPlatformProcess::CreatePipe(Read, Write, /*bWritePipeLocal=*/true)`
  建管道，父进程持 WritePipe、子进程继承 ReadPipe 作为 stdin；分块写入并处理短写。
- `UMovieGraphHEVC10Node::WriteFrame_EncodeThread` — 把帧
  `QuantizeImagePixelDataToBitDepth(InPixelData, /*TargetBitDepth=*/16, ...)` 量化到
  `FFloat16Color`，再写进管道。**内置 MP4 节点在这一行用的是 8。**

生成的日志都带 `LogHEVC10` 前缀，方便排查。
