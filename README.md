# Unreal Engine 5.8 Plugins

给 UE 5.8 用的插件，都是从实际项目里长出来的，解决的是引擎本身没覆盖的问题。

| 插件 | 作用 |
|---|---|
| **[HEVC10Output](Plugins/HEVC10Output)** | 给 Movie Render Graph 加一个 **10-bit H.265 (HEVC) MP4** 输出节点。渲染帧通过 stdin 管道直喂 ffmpeg，**无中间图像序列文件**，编码与渲染并行。 |
| **[MMDSequencerPreview](Plugins/MMDSequencerPreview)** | Sequencer 打开时，自动驱动 **MMD2Unreal** 导入的角色动画在编辑器视口里按时间轴回放。 |

**两种用法，按需选一种：**

| 你想 | 用哪份 | 需要 C++ 工程 |
|---|---|---|
| 丢进去就用 | [`Prebuilt/`](Prebuilt)（预编译二进制） | ❌ 不需要 |
| 自己改代码 / 自己编 | [`Plugins/`](Plugins)（纯源码） | ✅ 需要 |

引擎版本：**UE 5.8**（Windows）
构建工具：Visual Studio 2022 + C++ 工具链

---

## 安装

[`Plugins/`](Plugins) 里是**纯源码**，需要跟你的工程一起编译。

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

> 如果你只想用其中一个，单独拷对应目录即可，两个插件互不依赖。
>
> 不想碰 C++ 工程的话，直接用 [`Prebuilt/`](Prebuilt)：拷进 `Plugins/` 重启编辑器即可，
> 但要求引擎 `BuildId` 是 `55116800`（就是 UE 5.8.2 的 `CompatibleChangelist`）。

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
**插件不识别、也不触碰任何相机 Actor**（早先版本会强制相机 tick，那是持久化的 UPROPERTY，
会给渲染里引入一个没有材质的球体，这段代码已经删掉）。

### 控制台命令

| 命令 | 说明 |
|---|---|
| `MMDSequencerPreview.Enable 1/0` | 启用 / 停用（停用会立即还原） |
| `MMDSequencerPreview.DriveInPIE 1/0` | PIE 里序列播放时是否同步，**默认开**（MRQ 用 PIE 渲染时要它） |
| `MMDSequencerPreview.Apply` | 立即手动同步一次 |

日志类别：`LogMMDSequencerPreview`

详细说明见 [插件 README](Plugins/MMDSequencerPreview/README.md)。

---

## 已知边界

- **MMDSequencerPreview 只处理「动画单一节点」模式的组件。** 用 AnimationBlueprint 的
  （比如场景网格）不会被碰。
- **MMDSequencerPreview 输出没有音频。** HEVC10Output 是纯视频输出，音频需要另外合：
  ```bash
  ffmpeg -i video.mp4 -i audio.wav -c:v copy -c:a aac -b:a 320k -shortest out.mp4
  ```
- **MMDSequencerPreview 只在「序列真的在跑」时同步。** 普通 Play-In-Editor（世界里没有序列
  播放器）不碰，免得干扰正常游戏测试；MRQ 渲染走的是 PIE 或独立 `-game` 进程，两条路都已覆盖
  （`-game` 那路是靠序列播放器自己 tick 出来的时间驱动的）。
- 两个插件都在 **UE 5.8 / Windows** 上开发验证，未在其他版本或平台测试。

---

## 附带工具

[`Tools/`](Tools) 里是配套脚本，跟插件本体无关，按需使用：

| 脚本 | 用途 |
|---|---|
| [`ToHEVC10.ps1`](Tools/ToHEVC10.ps1) | 把已有视频转成 10-bit HEVC MP4。默认 `hevc_nvenc`（GPU，快），`-Encoder x265` 走 CPU（慢但同码率更小）。渲染时没走插件、事后想补转就用它。 |
| [`ResetUECaches.ps1`](Tools/ResetUECaches.ps1) | 重置 UE 5.8 的 DDC / Intermediate / 着色器缓存。**默认只预览**，加 `-Apply` 才真删。 |

```powershell
# 看看会删什么（什么都不动）
.\Tools\ResetUECaches.ps1 -ProjectPath "D:\MyProject"

# 工程已经删了 / 只想清引擎级缓存
.\Tools\ResetUECaches.ps1 -Scope Engine -Apply
```

`ResetUECaches.ps1` 对 `Binaries`、`Content`、`Config`、`Source`、`Logs`、`MovieRenders`
这类东西有硬性保护，路径命中就直接跳过；编辑器开着时会拒绝执行（除非只是预览）。

---

## 许可

[MIT](LICENSE) © 2026 kxgx

可自由使用、修改、分发，包括商业用途，保留版权声明即可。
