# MMDSequencerPreview — 播放 Sequencer 时自动预览 MMD 动画

## 解决的问题

MMD2Unreal 把 VMD 动作导入成 `AnimSequence`，并把角色的 `SkeletalMeshComponent` 设为
**「动画单一节点」（AnimationSingleNode）** 模式、指向该动画。这种组件**只在游戏世界 tick 时才求值动画**，所以：

- MRQ 渲染时 → 世界在 tick → 舞蹈正常播放 ✅
- 编辑器视口里 → 不动 ❌

`MMDSequencerPreview` 在 **Sequencer 打开着的时候**，自动驱动这些组件，让视口里能实时看到动作，
并且**位置跟随 Sequencer 时间轴**（拖时间轴也能看到对应姿势）。

## 行为

打开任意 Level Sequence 后：

1. 把 MMD 动作驱动的组件的 **「更新编辑器中的动画」** 打开
2. 确保它们在播放
3. **把播放速率设为 0**，让动画完全不自走（见下面「为什么必须冻结播放速率」）
4. 每帧把动画位置设为 Sequencer 的当前时间
5. 关闭最后一个 Sequencer / 退出编辑器时，**把上述状态逐项还原**（只还原它自己改过的）

### 为什么必须冻结播放速率

早期版本在 `SetPosition(时间轴时间)` 的同时让动画保持 `playRate = 1` 播放着。结果是：
**渲染发生在两次 tick 之间**，那期间组件的 tick 会让动画自己往前走一点
（`当前时间 + deltaTime`），于是渲染出来的姿势比设定的晚一帧。

表现就是"进度条停住后，动作仍会随鼠标移动而变化" —— 鼠标一动视口多刷新几次，
就更容易渲染到那个"往前走了一点"的姿势。

现在把 `playRate` 置 0，动画只能由 `SetPosition` 驱动，姿势完全确定。

### 关机时的还原时机

还原挂在 `FEditorDelegates::OnEditorPreExit` 上，**在世界销毁之前**执行。
如果放到模块的 `ShutdownModule()` 里就太晚了（那时世界已销毁，拿不到组件），
会留下脏状态并可能导致编辑器退出时崩溃。

## 怎么判断"哪些是 MMD2Unreal 导入的"

只认一个精确标记：

```
AnimSequence 的 AssetUserData 中包含 类名为 "MMDVmdAssetUserData" 的条目
```

这是 MMD2Unreal 给每个 VMD 导入序列打的戳。已验证：

| 资产 | 有标记 |
|---|---|
| `MMD动作/克拉蕾/【田中ヒメ】...【ダンス】` | ✅ |
| `MMD动作/洛克茜/【鈴木ヒナ】...【ダンス】` | ✅ |
| SkeletalMesh / Skeleton / 场景网格 | ❌ |

另外还会识别类名含 `MMDCineCameraActor` 的 Actor（VMD 镜头数据），确保它在视口里 tick。

## 不会和 MMD2Unreal 冲突

- **不链接、不调用、不依赖 MMD2Unreal**。检测全靠上面那个字符串标记和类名，是单向的只读判断。
- **不改任何资产、关卡或序列**，也不保存任何东西。只动运行时的组件开关。
- **不改动画模式**，不加轨道，不动绑定。
- **PIE / 打包运行时完全不介入**（`GEditor->PlayWorld != nullptr` 时直接返回），游戏世界自己会播。
- 模块类型是 **Editor**，打包版本里根本不存在。

## 用法

**默认启用，什么都不用做**：打开 Level Sequence，视口里就会动。

### 控制台变量

| 命令 | 说明 |
|---|---|
| `MMDSequencerPreview.Enable 1` | 启用（默认） |
| `MMDSequencerPreview.Enable 0` | 停用，并立即还原所有改动过的开关 |
| `MMDSequencerPreview.Apply` | 立即执行一次同步（正常每帧自动执行，用于手动刷新） |

### 日志

所有输出都在 `LogMMDSequencerPreview` 类别下，过滤这个关键字即可。

## 已知边界

- 动画是**跟随 Sequencer 时间轴**回放的，而不是让组件自己按自己的时钟循环。
  所以 Sequencer 停住时画面也停住——这是刻意的，方便看构图。
- 如果某个模型用 **AnimationBlueprint** 模式而不是单一节点（比如场景网格），插件**不会碰它**。
  那种情况需要别的方案（AnimBP 的编辑器预览）。
- 首次打开序列时，生效时刻是**下一帧**（不是构造序列的瞬间），这是为了避开 UE 在 Sequencer
  构造过程中操作骨架网格的安全问题。想立刻看到可以执行 `MMDSequencerPreview.Apply`。

## 文件结构

```
Plugins/MMDSequencerPreview/
  MMDSequencerPreview.uplugin                     插件描述
  Binaries/Win64/UnrealEditor-MMDSequencerPreview.dll   ← 实际加载的
  Source.disabled/                                源码（改名即可参与编译）
    MMDSequencerPreview/MMDSequencerPreview.Build.cs
    MMDSequencerPreview/Public/MMDSequencerPreviewModule.h
    MMDSequencerPreview/Private/MMDSequencerPreviewModule.cpp
    MMDSequencerPreview/Private/MMDPreviewDriver.h      核心：驱动逻辑
    MMDSequencerPreview/Private/MMDPreviewDriver.cpp
```

核心只有一处：`FMMDPreviewDriver::Tick()` —— 遍历编辑器世界里的
`USkeletalMeshComponent`，筛出 MMD 动作驱动的，把 `SetUpdateAnimationInEditor(true)`
和 `SetPosition(SequencerTime)` 应用上去。

## 重新编译

本仓库是**纯源码**，跟着你的工程一起编译即可。

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" ^
    YourProjectEditor Win64 Development ^
    -Project="C:\Path\To\YourProject.uproject" -WaitMutex
```

或者在编辑器里改完代码后按 **Ctrl+Alt+F11**（Live Coding）。

> ⚠️ **一个坑**：如果你的工程里同时装了**只有 `Build.cs`、没有实现代码的二进制插件**
> （MMD2Unreal 就是这种形态），一旦工程被 UBT 视为 C++ 工程，UBT 会把它当源码模块去编译，
> **生成一个空壳 DLL 覆盖掉原来的真货**，导致那个插件初始化失败。
> 编译期间把那种插件的 `Source` 目录临时改名移开，编完再改回来即可。
>
> 顺带一提：**本插件并不需要 MMD2Unreal 才能编译** —— 它不链接、不依赖 MMD2Unreal，
> 只是运行时通过资产标记识别 MMD2Unreal 导入的数据。
