# MRQ Auto Segment

给 UE 5.8 的编辑器插件。探测本机**空闲内存**和**空闲显存**,据此把一次长镜头渲染切成若干段,
并在**影片渲染队列**里为每段生成一个任务。每个任务的输出文件名里带自己的帧范围。

```
空闲显存 14.18 GiB  ×  80%  =  11.35 GiB  →  每段 734 帧  →  6150 帧切成 9 段

Segment 0000-0733   →  NewLevelSequence_0000-0733.mp4
Segment 0734-1467   →  NewLevelSequence_0734-1467.mp4
Segment 1468-2201   →  NewLevelSequence_1468-2201.mp4
...
Segment 5872-6149   →  NewLevelSequence_5872-6149.mp4
```

---

## 为什么要分段

唯一诚实的理由是:**不要在一次崩溃里丢掉整晚的渲染**。

围绕这一点还有一个真实存在的机制,它是"内存决定段长"的依据:

- 渲染出的帧会**堆在图像写入 / 编码管线里**。只要渲染器比写盘快,积压就待在 RAM 里,
  积压多深取决于盘和编码器跟不跟得上。
- 视频输出节点(比如同仓库的 `HEVC10Output`)要把整段编码进一个文件,编码器的前瞻队列
  随段长增长。
- 显存侧的峰值由输出分辨率 × 数据格式决定,和帧数无关;但**长时间渲染中某些系统
  (Niagara、Chaos、路径追踪)会缓慢增长**。

所以本插件把"每帧增长"建模成**一个帧缓冲的若干分之一**,而不是假装能精确算出来。

**只看内存和显存。** 磁盘容量不参与计算——它既不决定单段多长,也不在面板里出现。

---

## 算法

```
帧缓冲字节数 = 宽 × 高 × 每像素字节

每段帧数 = min(
    内存允许 =  空闲内存 × 内存使用上限  ÷ (帧缓冲 × 每帧内存增长系数)
    显存允许 =  空闲显存 × 显存使用上限  ÷ (帧缓冲 × 每帧显存增长系数)
    用户上限 =  每段最长帧数
)
段数 = ceil(总帧数 / 每段帧数)
```

**使用上限默认都是 80%,在面板里可以直接改。** 也就是只占用空闲容量的一部分,
其余留给系统、其他程序和编辑器自身。

**这套数字是可见、可调、可质疑的。** 面板会逐条列出每个约束算出来的帧数,给真正决定
结果的那一条打 `★`,并把算式(空闲的百分之多少 = 多少字节 ÷ 每帧多少)一起显示出来:

```
  系统内存: 3878   空闲的 80% = 29.96 GiB ÷ 每帧约 7.91 MiB
★ 显存:      734   空闲的 80% = 11.35 GiB ÷ 每帧约 15.82 MiB
  长度上限: 1000   用户设置每段最长 1000 帧
```

| 参数 | 默认 | 含义 |
|---|---|---|
| **内存使用上限** | **80%** | 最多占用多少空闲内存 |
| **显存使用上限** | **80%** | 最多占用多少空闲显存 |
| **每像素字节** | **自动** | 跟随所选输出格式,见下节 |
| 每帧内存增长系数 | 0.25 | 每渲染一帧,RAM 里的写盘积压增加 1/4 个帧缓冲 |
| 每帧显存增长系数 | 0.5 | 每渲染一帧,显存增长 1/2 个帧缓冲 |
| 每段最长帧数 | 1000 | 用户上限 |

两个"使用上限"和"每像素字节"在面板里可以直接调;
三个增长/上限系数目前只能从 Python / Blueprint 改(见下方 `MRQSegmentRequest`)。

### 每像素字节为什么重要

它是 `帧缓冲 = 宽 × 高 × 每像素字节` 里的最后一项,而帧缓冲是所有内存估算的基数 ——
**填小一半,分段就会长一倍**。

而它由**输出格式的位深**决定,不是由分辨率决定:

| 输出格式 | 每通道 | 每像素字节 |
|---|---|---|
| JPEG / BMP | 8 bit, 无 alpha | 3 |
| PNG / TGA / 引擎自带 MP4 | 8 bit | 4 |
| **HEVC 10-bit**(本仓库的 `HEVC10Output`) | 16 bit 半精度 | **8** |
| EXR | 16 bit 半精度 | 8 |

所以面板默认 **勾选「跟随输出格式」**,你选什么输出节点,这个值就自动变成对应的数字,
旁边的灰字会写出推断理由,例如:

```
☑ 跟随输出格式    8    HEVC 10-bit 按半精度 RGBA 走管线，每通道 2 字节
```

需要手工指定时取消勾选即可。

> **为什么是"推断"而不是"读取"**:Movie Graph 的输出节点不把位深暴露成反射属性 ——
> `UMoviePipelineVideoOutputBase` 是在函数参数里接位深,图像序列节点的 `EImageFormat`
> 是普通 C++ 成员(不是 `UPROPERTY`)。所以这里按类名匹配已知格式。**推断理由会显示在
> 面板上**,所以猜错了你能直接看见并手动改掉,而不是悄悄算错。

---

## 安装

纯源码插件,需要跟工程一起编译。

```
YourProject/
├── YourProject.uproject
└── Plugins/
    └── MRQAutoSegment/
```

`.uproject` 里启用:

```json
"Plugins": [
    { "Name": "MRQAutoSegment", "Enabled": true }
]
```

首次打开编辑器提示重新编译模块时点「是」。

---

## 用法

打开面板的四种方式:

- **Sequencer 工具栏上的「MRQ 分段」按钮** —— 最直接,它就在你正在编辑的序列旁边。
  点开后面板会自动选中**这个 Sequencer 里打开的序列**,并把帧范围填成它的播放范围,
  通常只要再选个输出格式就能直接生成。同时开多个 Sequencer 时,每个按钮对应自己那一个。
- 关卡编辑器工具栏上的 **「MRQ 分段」** 按钮
- 顶部菜单 **工具 → MRQ 分段**
- 控制台 `MRQAutoSegment.Open`

面板从上到下:

1. **硬件空闲容量** —— 内存 / 显存 / 显卡,点「重新探测硬件」刷新
2. **参数** —— 序列、输出目录、文件名格式、输出格式、分辨率、每像素字节、时间采样、帧范围、分段方式、使用上限
3. **分段预设** —— 每条约束算出的帧数(`★` 是限制项)、总结、逐段列表(帧范围 + 文件名)
4. **生成渲染队列** —— 把分段写进影片渲染队列

**分辨率只有预设,没有自定义。** 下拉列表直接读项目的命名分辨率
(`UMovieGraphProjectSettings::DefaultNamedResolutions`,也就是 Movie Graph 输出节点里
看到的那一份),选中的那一条会**原样**写到任务上。

想加一个列表里没有的尺寸,**去项目设置里加**,不要指望面板:
**项目设置 → Movie Render Pipeline → Named Resolutions**,加完回面板点一下刷新就有了。

为什么不做面板内自定义:插件的任务用的是 Basic 配置模式,如果往任务上写一个自己拼的
`Custom` 分辨率条目,渲染队列会**一直重复第一个任务**而不往下走。用引擎自己的命名预设
就没这个问题。

**输出目录默认是 `{project_dir}/Saved/MovieRenders`** —— 和**引擎自己的默认值**一致,但写成**项目相对
形式**,不是解析后的绝对路径。两个好处:换机器不用改,存进预设也不会把本机的工程位置带出去。
该字段支持 `{project_dir}` 等 token,渲染时由引擎展开。

点「从序列读取」会用所选关卡序列的播放范围填充帧范围。

**「重新探测硬件」不会动你已经选好的配置。** 序列、输出格式、分辨率、每像素字节、帧范围、
分段方式、使用上限全部保留,只有硬件数字和分段结果会更新。

### 每段一个输出文件夹

点「生成渲染队列」后,每一段的输出目录是 **`<输出目录>/<帧范围>/`**,例如输出目录填
`{project_dir}/Saved/MovieRenders`、计划里第一段是 `0000-0724`,那么这一段就写进:

```
{project_dir}/Saved/MovieRenders/0000-0724/<序列名>_0000-0724.mp4
```

这样各段互不干扰,也不会出现"这一段把上一段覆盖掉"或者几段的帧混在一个目录里分不清的情况。
文件名里仍然带帧范围,所以就算你把某个文件夹里的东西挪出来也认得出是哪一段。

要拼成一个完整视频,按文件夹名(也就是帧范围)排序把它们接起来即可 ——
`ffmpeg -f concat` 或任何剪辑软件都行,不用重新编码。

### 预设

面板底部有「预设」一行:填个名字点 **保存预设**,把当前整份配置存下来;
以后在下拉里选中点 **加载预设** 即可恢复。存的内容包括序列、输出目录、文件名格式、
输出格式、分辨率、每像素字节、帧范围和全部算法参数。

预设是 **JSON,一个预设一个文件**,放在:

```
<工程>/Saved/MRQAutoSegment/Presets/<名字>.json
```

想带到别的机器或放进版本控制,直接拷这个目录即可。名字留空会退回用序列名。

### 时间采样

```
时间采样   [8]
```

生成任务时把它写进每个任务的配置。默认 **8**。

它落在图里的**采样方式节点**(`MovieGraphSamplingMethodNode`,Globals 分支上),
所以**对延迟渲染和路径追踪都生效** —— 插件不会因此强制你换渲染器。

**它不参与分段计算** —— 只落到生成的任务上,所以改动它不会重算分段。该值也会存进预设。

### 文件名

面板里的**文件名格式**是引擎原本的那套 token(`{sequence_name}`、`{date}`、`{shot_name}`…),
插件把帧范围**追加在末尾**:

```
{sequence_name}        →  {sequence_name}_0000-1023
MyShot_{date}          →  MyShot_{date}_0000-1023
```

引擎在渲染时再解析其余 token。帧号补零位数可调(默认 4 位)。

> 序列起始帧为负数时(MMD 镜头数据常见,例如 −65),标签形如 `-0065-1023`。
> 把整体帧范围的起点设成 0 可以避免。

### 控制台命令

| 命令 | 说明 |
|---|---|
| `MRQAutoSegment.Open` | 打开面板 |
| `MRQAutoSegment.Probe` | 打印空闲内存 / 显存 |
| `MRQAutoSegment.Plan` | 用默认参数(0–599 帧、1080p)跑一次分段并打印结果 |

日志类别:`LogMRQAutoSegment`

### Python / Blueprint

面板做的一切都走 `UMRQAutoSegmentLibrary`,可以完全绕开 UI:

```python
import unreal

lib = unreal.MRQAutoSegmentLibrary

req = unreal.MRQSegmentRequest()
req.set_editor_property("range_start", 0)
req.set_editor_property("range_end", 6149)
req.set_editor_property("resolution", unreal.IntPoint(3840, 2160))
req.set_editor_property("bytes_per_pixel", 4)
req.set_editor_property("ram_use_limit", 0.8)    # 空闲内存的 80%
req.set_editor_property("vram_use_limit", 0.8)   # 空闲显存的 80%

plan = lib.plan_from_hardware(req)
unreal.log(plan.get_editor_property("message"))
unreal.log(plan.get_editor_property("frames_per_segment"))

queue = lib.get_editor_queue()
seq = unreal.load_asset("/Game/NewLevelSequence")
created = lib.generate_jobs(
    queue, seq, "/Game/Main",
    plan, "{project_dir}/Saved/MovieRenders", "{sequence_name}", unreal.IntPoint(3840, 2160))
unreal.log("created %d jobs" % created)
```

---

## 实现要点

插件使用的是 **Basic 配置模式** (`UMoviePipelineBasicConfig`),不是图模式也不是旧版预设。

原因:UE 5.8 里每任务的输出设置在 Basic 配置上是**普通字段**
(`FileNameFormat` / `CustomStartFrame` / `CustomEndFrame` / `OutputDirectory`),
而 Basic 模式在渲染前会**即时生成一个 UMovieGraphConfig** —— 也就是说它照样走完整
Movie Graph 管线,但配置是按任务给的。

如果改用图模式,`FileNameFormat` 和播放范围都长在图节点上、`GraphPreset` 又是
`TSoftObjectPtr`,想做到"每个任务一个文件名"就得把用户的图复制 N 份,或者要求用户先在
图里把属性提升成变量。两条路都不好,所以没走。

代价见「已知边界」第 3 条。

### 每段一个带指定帧范围的关卡序列

勾上「每段生成一个关卡序列（带指定帧范围）」（默认开）后，每一段会得到一个**自己的**
关卡序列，帧范围是**写进资产里**的：

- 位置：`/Game/MRQAutoSegment/Segments/<序列名>_<帧范围>`，例如
  `NewLevelSequence_0000-0724`
- **播放范围**和**工作范围**都会被设成这一段的帧范围，然后**存盘**。所以范围是资产的
  一部分，不依赖渲染时再做一次覆盖；「Render (New Process)」另起进程也照样对
- 任务同时把这个范围写在 Basic 配置上，两道保险
- 取消勾选则所有任务共用你选的那个序列，不产生任何资产

名字里的帧范围就是它实际的范围，可以直接核对。生成时的日志也会逐条打印：

```
Segment sequence 'NewLevelSequence_0000-0724': playback 0..725, working 0.000s..12.083s, saved=yes
Segment sequence 'NewLevelSequence_0725-1449': playback 725..1450, working 12.083s..24.167s, saved=yes
```

实测过：**换一个编辑器会话**从磁盘重新读取这 9 个资产，每一个的播放范围都正好是它自己那一段
（`0..725` / `725..1450` / … / `5800..6150`）。工作范围也同样各不相同，所以它们不是"内容
一模一样的副本"。

「清除生成的任务」会把这些序列一起删掉。

顺带一个必须记住的细节：**范围是半开区间。** `CustomStartFrame` / `CustomEndFrame` 会被
直接塞进 `TRange<FFrameNumber>`，所以计划里"到 724 为止"的一段要写成 `724 + 1 = 725`。
写成闭区间的结束帧会让每一段都少渲染最后一帧。

### Sequencer 按钮怎么知道是哪个 Sequencer

Sequencer 构建工具栏时会往 `FToolMenuContext` 里放一个 `USequencerToolMenuContext`,
其中是 `WeakSequencer`。插件的按钮就从这里取 —— 所以同时开多个 Sequencer 时,每个按钮
对应自己那一个,不需要去猜"当前活跃的是哪个"。

工具栏本身走 `UToolMenus::ExtendMenu("Sequencer.MainToolBar")`。`ExtendMenu` 对**尚未注册**
的菜单会自动建一个,而 Sequencer 之后调用 `RegisterMenu` 时会**复用同一个菜单对象**并只改
`MenuType`,已有的 section 全部保留 —— 所以插件在 `PostEngineInit` 阶段注册是安全的,
不必等 Sequencer 先出现。

---

## 已知边界

1. **"空闲显存"只统计本进程。** `RHIGetTextureMemoryStats()` 能给出显存总量和 UE 自己
   已分配的量(streaming + non-streaming),但**看不到其他程序占用的显存**。所以这个值
   是上界,默认只用它的 80% 正是为了覆盖这部分。要看真实空闲显存需要走 DXGI 的显存预算
   接口,当前没做。

2. **每帧增长系数是估计值,不是测出来的。** 面板把算式显示出来就是为了让你能反驳它。
   如果某次渲染确实在中途 OOM,把对应系数调小(或把使用上限调低)即可。

3. **Basic 模式会即时生成自己的图,不复用你的 Movie Graph 资产。** 如果你在 Graph 里做了
   精细的节点配置(多分支、自定义渲染 Pass 等),生成的任务不会带上它们。这种情况下建议
   先用本插件算出分段和帧范围,再手动把设置搬到你的 Graph 任务上。

4. **分辨率只能用项目里已有的命名分辨率。** 面板写到任务上的分辨率必须是
   `DefaultNamedResolutions` 里真实存在的一条;名字对不上就只记一条警告、不改分辨率。
   插件的任务用的是 Basic 配置模式,往任务上写一个自己拼的 `Custom` 分辨率条目会让
   渲染队列一直重复第一个任务。要新尺寸就去项目设置里加。

5. **分段序列会写进你的工程内容目录。** 勾了「每段生成一个关卡序列」时，N 段就在
   `/Game/MRQAutoSegment/Segments/` 下多 N 个序列资产，它们各自带着自己那一段的帧范围。
   原始序列不会被改动。不想要了就点「清除生成的任务」，或者取消勾选那个选项。

6. 只在 **UE 5.8 / Windows** 上开发验证。

---

## 相关

同仓库的 [`HEVC10Output`](../HEVC10Output) 提供 10-bit HEVC MP4 输出节点,可以在面板的
**输出格式** 下拉里直接选中,让每个分段任务都输出 HEVC MP4。
