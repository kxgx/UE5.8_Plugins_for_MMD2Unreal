# 预编译版 (Prebuilt)

这两个插件的**预编译（二进制）版本**。丢进工程就能用，**不需要工程是 C++ 工程，也不需要编译**。

| 插件 | 作用 |
|---|---|
| `HEVC10Output` | 10-bit H.265 (HEVC) MP4 输出节点，帧直接走管道进 ffmpeg |
| `MMDSequencerPreview` | 让 MMD2Unreal 的 VMD 动作跟随 Sequencer 时间轴（视口 + 渲染） |

## 环境要求

- **Unreal Engine 5.8.2**，且 `BuildId` 必须是 **`55116800`**
  （就是引擎 `Engine/Build/Build.version` 里的 `CompatibleChangelist`）
- Windows x64

二进制插件的 `UnrealEditor.modules` 里记着这个 BuildId。引擎版本对不上时编辑器会拒绝加载，
这一点没法绕开——对不上就请用 [`Plugins/`](../Plugins) 里的源码版自己编。

## 安装

1. 把 `Plugins/<插件名>/` 整个文件夹拷到 **`<你的工程>/Plugins/`** 下
   （没有 `Plugins` 目录就自己建一个）
2. 重启编辑器
3. 如果是手动安装的，去 **编辑 → 插件** 里确认它已启用

拷完长这样：

```
<你的工程>/
  Plugins/
    HEVC10Output/
      HEVC10Output.uplugin
      Binaries/Win64/UnrealEditor-HEVC10Output.dll
      Binaries/Win64/UnrealEditor.modules
    MMDSequencerPreview/
      ...
```

## 每个插件里有什么

| 文件 | 说明 |
|---|---|
| `<插件名>.uplugin` | `"Installed": true`，带 `EngineVersion` 和 `SupportedTargetPlatforms`。这一项就是"预编译"的标志：UBT 不会去编它，编辑器直接加载 DLL |
| `Binaries/Win64/UnrealEditor-<模块>.dll` | 真正的二进制 |
| `Binaries/Win64/UnrealEditor.modules` | 模块清单 + `BuildId`，引擎用它校验 DLL 是否匹配 |
| `README.md` | 该插件的完整说明 |

**没有 `Source/`**：预编译版只放 `Build.cs` 之外的东西。源码在
[`Plugins/`](../Plugins) 里，想改就改那份、自己编。

## 关于 `Source/` 和重新编译

预编译版**故意不带 `Source/`**。这点很重要：

以前这个工程里就因为 MMD2Unreal（同样是只有 `Build.cs`、没有实现源码的二进制插件）被 UBT
当成源码模块重编过，生成一个空壳 DLL 覆盖掉真货，插件直接初始化失败。

所以：**想让工程变回 C++ 工程时，先确认这些插件是 `Installed: true` 且没有被 UBT 扫描到。**
需要重新编译插件本体时，请用 [`Plugins/`](../Plugins) 里的源码，别在预编译版上动手。

## 各插件自己的说明

细节都在各自的 README 里，安装后建议至少看一眼：

- [`HEVC10Output/README.md`](Plugins/HEVC10Output/README.md) —— 需要 ffmpeg，在 PATH 里或者节点上指定路径
- [`MMDSequencerPreview/README.md`](Plugins/MMDSequencerPreview/README.md) —— 控制台变量、什么时候会同步
