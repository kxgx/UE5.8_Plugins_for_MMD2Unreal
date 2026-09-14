# Unreal Engine 5.8 Plugins

给 UE 5.8 用的插件，都是从实际项目里长出来的，解决的是引擎本身没覆盖的问题。

| 插件 | 作用 |
|---|---|
| **[HEVC10Output](Plugins/HEVC10Output)** | 给 Movie Render Graph 加一个 **10-bit H.265 (HEVC) MP4** 输出节点。渲染帧通过 stdin 管道直喂 ffmpeg，**无中间图像序列文件**，编码与渲染并行。 |
| **[MMDSequencerPreview](Plugins/MMDSequencerPreview)** | Sequencer 打开时，自动驱动 **MMD2Unreal** 导入的角色动画在编辑器视口里按时间轴回放。 |
| **[MRQAutoSegment](Plugins/MRQAutoSegment)** | 探测空闲内存 / 显存（只用其中 80%，可调），把长镜头切成多个渲染任务，每个任务的输出文件名带自己的帧范围。 |

引擎版本：**UE 5.8**（Windows）
构建工具：Visual Studio 2022 + C++ 工具链

---

## 安装

两个插件都是**纯源码**，需要跟你的工程一起编译。

```bash
# 把 Plugins 目录拷到你的工程根目录（和 .uproject 同级）
YourProject/
├── YourProject.uproject
├── Content/
└── Plugins/
    ├── HEVC10Output/
    └── MMDSequencerPreview/
```

然后在 `.uproject` 里启用（或直接开编辑器时会提示启用）：

```json
"Plugins": [
    { "Name": "HEVC10Output",       "Enabled": true },
    { "Name": "MMDSequencerPreview","Enabled": true }
]
```

**首次打开编辑器时会提示重新编译模块**，点「是」即可。或者手动编译：

```bash
# <UE> = 你的 UE 5.8 安装目录
"<UE>\Engine\Build\BatchFiles\Build.bat" ^
    YourProjectEditor Win64 Development ^
    -Project="<你的工程>\YourProject.uproject" -WaitMutex
```

> 如果你只想用其中一个，单独拷对应目录即可，三个插件互不依赖。

---

## HEVC10Output

### 为什么需要它

UE 5.8 自带的 MP4 输出是 **H.264 8-bit**，而且 `UMovieGraphMP4EncoderNode` 的默认码率写死为
`AverageBitrateInMbps = 8`（1080p 的取值）。拿去做 4K/60fps 会糊成一片。

### 实现方式

```
MRQ 16-bit 量化 → FFloat16Color (半精度 RGBA, 8B/px)
   → ffmpeg stdin: -f rawvideo -pix_fmt rgbaf16le
   → hevc_nvenc -profile:v main10 -pix_fmt p010le  →  out.mp4
```

关键点：**帧通过 stdin 管道直接喂给 ffmpeg**。对比传统的 EXR 中间格式方案
（4K 整片约 90 GB 中间文件 + 渲染结束后串行编码，总时长 +30%），这个方案**磁盘占用为 0，
编码与渲染并行**。

### 用法

1. 影片渲染队列 → 让 Job 使用 **Graph 配置**（自定义节点只出现在 Movie Render Graph 里）
2. 添加节点 **`H.265 HEVC 10-bit MP4`**（分类 `Output Type`）
3. 接线：`Output Settings → H.265 HEVC 10-bit MP4 → Output 节点的 Globals 引脚`
4. `Quality` 建议 **19**（16 ≈ 近无损，19 高，23 中）

### 依赖

- **ffmpeg**（含 `hevc_nvenc` 或 `libx265`），需要能从 `PATH` 找到。找不到时，在节点的
  **FFmpeg Path** 里点 📁 选一个就行——留空才表示"自己去 `PATH` 找"
- 硬件编码需要支持 HEVC Main10 的 GPU（NVIDIA Pascal 及以后 / AMD / Intel 均可）

详细说明见 [插件 README](Plugins/HEVC10Output/README.md)。

---

## MMDSequencerPreview

### 为什么需要它

MMD2Unreal 把 VMD 动作导入成 `AnimSequence`，并把角色的 `SkeletalMeshComponent` 设为
**「动画单一节点」（AnimationSingleNode）** 模式。这种组件**只在游戏世界 tick 时才求值动画**：

- MRQ 渲染时 → 世界在 tick → 舞蹈正常播放
- **编辑器视口里 → 不动**

### 实现方式

插件监听 `ISequencerModule::OnSequencerCreated`，在 Level Sequence 打开期间：

1. 打开 MMD 组件的 **「更新编辑器中的动画」**
2. **把播放速率设为 0** —— 这一步很关键，见下
3. 每帧把动画位置设为 Sequencer 的当前时间
4. 关闭序列 / 退出编辑器时逐项还原

### 两个值得一提的坑

**为什么必须冻结播放速率**

早期版本让动画保持 `playRate = 1` 播放着，同时每帧 `SetPosition(时间轴时间)` 拉回来。
问题是**渲染发生在两次 tick 之间**，那期间组件的 tick 会让动画自己往前走一点，
于是渲染出来的姿势比设定的晚一帧 —— 表现就是"进度条停住后，动作仍会随鼠标移动而变化"。
`playRate = 0` 之后姿势完全确定。

**为什么还原要挂在 `OnEditorPreExit`**

放在模块的 `ShutdownModule()` 里太晚了 —— 那时世界已经销毁，拿不到组件，
会留下脏状态并**导致编辑器退出时崩溃**。

### 怎么识别"哪些是 MMD2Unreal 导入的"

只认一个精确标记 —— MMD2Unreal 给每个 VMD 导入序列打的 AssetUserData 戳：

```
AnimSequence.AssetUserData 中存在类名为 "MMDVmdAssetUserData" 的条目
```

**插件不链接、不调用、不依赖 MMD2Unreal**，只做只读的标记判断，所以不会冲突。
另外会识别类名含 `MMDCineCameraActor` 的 Actor（VMD 镜头数据）。

### 控制台命令

| 命令 | 说明 |
|---|---|
| `MMDSequencerPreview.Enable 1/0` | 启用 / 停用（停用会立即还原） |
| `MMDSequencerPreview.Apply` | 立即手动同步一次 |

日志类别：`LogMMDSequencerPreview`

详细说明见 [插件 README](Plugins/MMDSequencerPreview/README.md)。

---

## MRQAutoSegment

### 为什么需要它

一次几千帧的长镜头渲染，最大的风险不是慢，而是**跑到 90% 崩了，一晚白干**。
但"切多长一段"没有标准答案——它取决于这台机器还剩多少内存和显存。

### 实现方式

探测 `FPlatformMemory::GetStats()` 和 `RHIGetTextureMemoryStats()`，然后：

```
每段帧数 = min(
    空闲内存 × 内存使用上限 ÷ (帧缓冲 × 每帧增长系数),
    空闲显存 × 显存使用上限 ÷ (帧缓冲 × 每帧增长系数),
    用户设的每段上限
)
```

**使用上限默认 80%，在面板里可调**；只看内存和显存，磁盘容量不参与计算。

**每像素字节自动跟随输出格式**：选 HEVC 10-bit 就是 8、选 PNG 就是 4——这个值决定帧缓冲大小，
填错一半分段就会长一倍，而它由输出格式的位深决定、不是由分辨率决定。推断理由会显示在面板上，
需要手工指定时取消勾选即可。

**分辨率是预设下拉 + 自定义**，预设直接读项目的命名分辨率（和 Movie Graph 输出节点里看到的是同一份）。

面板会把**每条约束算出的帧数逐条列出**，给真正决定结果的那条打 `★`，并把算式
（空闲的百分之多少 = 多少字节 ÷ 每帧多少）一起显示出来——数字是可见、可调、可质疑的，不是黑箱。

任务用 **Basic 配置模式**创建。该模式在渲染前**即时生成 `UMovieGraphConfig`**，
所以 `FileNameFormat` / `CustomStartFrame` / `CustomEndFrame` 都只是普通字段，可以逐任务设置，
**既不用复制也不用改动你自己的 Movie Graph 资产**。

### 用法

直接在 **Sequencer 工具栏**点 **「MRQ 分段」**（最省事：面板会自动选中你正在编辑的序列，
并把帧范围填成它的播放范围）。也可以用关卡编辑器工具栏的按钮、菜单 **工具 → MRQ 分段**，
或控制台 `MRQAutoSegment.Open`。调好参数后点「生成渲染队列」。

文件名：帧范围自动追加在你的格式串末尾——
`{sequence_name}` → `{sequence_name}_0000-0733`、`{sequence_name}_0734-1467` …

也完全可以用 Python 驱动，绕开 UI：

```python
import unreal
lib = unreal.MRQAutoSegmentLibrary
req = unreal.MRQSegmentRequest()
req.set_editor_property("range_end", 6149)
req.set_editor_property("ram_use_limit", 0.8)   # 空闲内存的 80%
req.set_editor_property("vram_use_limit", 0.8)  # 空闲显存的 80%
plan = lib.plan_from_hardware(req)
lib.generate_jobs(lib.get_editor_queue(), unreal.load_asset("/Game/MySequence"),
                  "/Game/Main", plan, "{project_dir}/Saved/MovieRenders", "{sequence_name}",
                  unreal.IntPoint(3840, 2160))
```

算法推导过程和已知边界见 [插件 README](Plugins/MRQAutoSegment/README.md)。

---

## 已知边界

- **MMDSequencerPreview 只处理「动画单一节点」模式的组件。** 用 AnimationBlueprint 的
  （比如场景网格）不会被碰。
- **MMDSequencerPreview 输出没有音频。** HEVC10Output 是纯视频输出，音频需要另外合：
  ```bash
  ffmpeg -i video.mp4 -i audio.wav -c:v copy -c:a aac -b:a 320k -shortest out.mp4
  ```
- **MRQAutoSegment 的"空闲显存"只统计本进程。** `RHIGetTextureMemoryStats()` 看得到显存总量和
  UE 自己分配了多少，但**看不到其他程序占用的显存**，所以那是上界，默认只用它的 80% 正是为了
  覆盖这部分。每帧增长系数也是估计值而非实测值——面板把算式显示出来就是为了让你能反驳它。
- **MRQAutoSegment 生成的 Basic 模式任务会即时生成自己的图**，不复用你的 Movie Graph 资产。
  如果你的图里有精细的多分支 / 自定义 Pass 配置，它们不会被带上。
- 三个插件都在 **UE 5.8 / Windows** 上开发验证，未在其他版本或平台测试。

## 许可

[MIT](LICENSE) © 2026 kxgx

可自由使用、修改、分发，包括商业用途，保留版权声明即可。
