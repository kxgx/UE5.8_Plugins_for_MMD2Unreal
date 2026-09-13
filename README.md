# Unreal Engine 5.8 Plugins

两个给 UE 5.8 用的插件，都是从实际项目里长出来的，解决的是引擎本身没覆盖的问题。

| 插件 | 作用 |
|---|---|
| **[HEVC10Output](Plugins/HEVC10Output)** | 给 Movie Render Graph 加一个 **10-bit H.265 (HEVC) MP4** 输出节点。渲染帧通过 stdin 管道直喂 ffmpeg，**无中间图像序列文件**，编码与渲染并行。 |
| **[MMDSequencerPreview](Plugins/MMDSequencerPreview)** | Sequencer 打开时，自动驱动 **MMD2Unreal** 导入的角色动画在编辑器视口里按时间轴回放。 |

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
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ^
    YourProjectEditor Win64 Development ^
    -Project="C:\Path\To\YourProject.uproject" -WaitMutex
```

> 如果你只想用其中一个，单独拷对应目录即可，两个插件互不依赖。

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

- **ffmpeg**（含 `hevc_nvenc` 或 `libx265`），需要能从 `PATH` 找到，或在节点上填绝对路径
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

## 已知边界

- **MMDSequencerPreview 只处理「动画单一节点」模式的组件。** 用 AnimationBlueprint 的
  （比如场景网格）不会被碰。
- **MMDSequencerPreview 输出没有音频。** HEVC10Output 是纯视频输出，音频需要另外合：
  ```bash
  ffmpeg -i video.mp4 -i audio.wav -c:v copy -c:a aac -b:a 320k -shortest out.mp4
  ```
- 两个插件都在 **UE 5.8 / Windows** 上开发验证，未在其他版本或平台测试。

## 许可

[MIT](LICENSE) © 2026 kxgx

可自由使用、修改、分发，包括商业用途，保留版权声明即可。
