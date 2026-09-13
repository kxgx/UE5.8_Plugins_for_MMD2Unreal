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
| `FfmpegPath` | 默认 `ffmpeg.exe`（查 PATH）。也可填绝对路径 |
| `ExtraArguments` | 附加原始 ffmpeg 参数，一般留空 |

参考本项目的现成例子：`/Game/HEVC10Test/HEVC10TestConfig`（320×180 测试图）。

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

## 重新编译

本仓库是**纯源码**，跟着你的工程一起编译即可。

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ^
    YourProjectEditor Win64 Development ^
    -Project="C:\Path\To\YourProject.uproject" -WaitMutex
```

或者在编辑器里改完代码后按 **Ctrl+Alt+F11**（Live Coding）。

### 一个坑：不要和"只发二进制的插件"混用

如果你的工程里同时装了**只有 `Build.cs`、没有实现代码的二进制插件**（很多商业插件是这种形态），
一旦工程被 UBT 视为 C++ 工程，UBT 会把那种插件当成源码模块去编译，
**生成一个空壳 DLL 覆盖掉原来的真货**，导致那个插件初始化失败。

规避办法：编译期间把那种插件的 `Source` 目录临时改名移开，编完再改回来。

---

## 文件结构

```
Plugins/HEVC10Output/
  HEVC10Output.uplugin
  Source/HEVC10Output/
    HEVC10Output.Build.cs
    Public/HEVC10FFmpegPipe.h            ffmpeg 进程 + stdin 管道封装
    Private/HEVC10FFmpegPipe.cpp
    Public/Graph/MovieGraphHEVC10Node.h  MRG 输出节点
    Private/Graph/MovieGraphHEVC10Node.cpp
    Public/HEVC10OutputModule.h
    Private/HEVC10OutputModule.cpp
```

核心实现只有两处：

- `FHEVC10FFmpegPipe` — 用 `FPlatformProcess::CreatePipe(Read, Write, /*bWritePipeLocal=*/true)`
  建管道，父进程持 WritePipe、子进程继承 ReadPipe 作为 stdin；分块写入并处理短写。
- `UMovieGraphHEVC10Node::WriteFrame_EncodeThread` — 把帧
  `QuantizeImagePixelDataToBitDepth(InPixelData, /*TargetBitDepth=*/16, ...)` 量化到
  `FFloat16Color`，再写进管道。**内置 MP4 节点在这一行用的是 8。**

生成的日志都带 `LogHEVC10` 前缀，方便排查。
