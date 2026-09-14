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
| 每帧内存增长系数 | 0.25 | 每渲染一帧,RAM 里的写盘积压增加 1/4 个帧缓冲 |
| 每帧显存增长系数 | 0.5 | 每渲染一帧,显存增长 1/2 个帧缓冲 |
| 每段最长帧数 | 1000 | 用户上限 |

两个"使用上限"在面板的 **参数 → 使用上限** 一行里可以直接调;
三个增长/上限系数目前只能从 Python / Blueprint 改(见下方 `MRQSegmentRequest`)。

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

打开面板的三种方式:

- 关卡编辑器工具栏上的 **「MRQ 分段」** 按钮
- 顶部菜单 **工具 → MRQ 分段**
- 控制台 `MRQAutoSegment.Open`

面板从上到下:

1. **硬件空闲容量** —— 内存 / 显存 / 显卡,点「重新探测硬件」刷新
2. **参数** —— 序列、输出目录、文件名格式、输出格式、分辨率、帧范围、分段方式、使用上限
3. **分段预设** —— 每条约束算出的帧数(`★` 是限制项)、总结、逐段列表(帧范围 + 文件名)
4. **生成渲染队列** —— 把分段写进影片渲染队列

点「从序列读取」会用所选关卡序列的播放范围填充帧范围。

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
    plan, "D:/Renders", "{sequence_name}", unreal.IntPoint(3840, 2160))
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

4. 只在 **UE 5.8 / Windows** 上开发验证。

---

## 相关

同仓库的 [`HEVC10Output`](../HEVC10Output) 提供 10-bit HEVC MP4 输出节点,可以在面板的
**输出格式** 下拉里直接选中,让每个分段任务都输出 HEVC MP4。
