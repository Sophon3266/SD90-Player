# SD90-Player

基于 **SD-8 / XV（`SD-8 XS_TISR.dll`）** 采样引擎与音色库开发的 Windows 64 位 **VST2.4 音源插件与独立 MIDI 播放器**，支持 GM / GS / GM2 / SD-90 Native 音色播放标准 MIDI 文件，并可直接被 **VST MIDI Driver (x64)** 或常见 DAW 加载。

> **合规说明**：本仓库源码与 Release 编译产物（`SD90Player.dll`、`sd90play.exe`、`sd90render.exe`、`vsthost_test.exe`）**仅包含自研代码与音色映射逻辑，不内嵌、也不分发任何 Roland 原插件、音色库或 ROM 采样数据**。运行时需由用户在本机自行准备合法持有的原版 `SD-8 XS_TISR.dll` 与 `A01..A11.bin`，软件仅将其作为本地 PE 数据文件读取（仅解析 `.rsrc` WROM 资源与 `.rdata` 表，`SD90Player.dll` 不会对其调用 `LoadLibrary`）。

---

## 核心特性

- **双采样引擎（8-bit RDAC DPCM 解码为浮点采样）**：
  1. **InternalA WROM**（`WROM/INTERNALA` + `.rdata` 采样表）：`3111` 组波形、`1083` 个 Multisample。
  2. **SRX / DMZ07 WROM**（`WROM/SRX`）：`2212` 组波形、`589` 个 Multisample（覆盖 10 通道鼓组与主要旋律音色）。
- **KoaBankFile 音色库解析（`A01..A11.bin`）**：
  - 完整读取 Patch Common、TMT（键位/力度分层）、Tone 1–4 参数（Wave Group/ID、左右波形、粗/细调音、声像、TVF 滤波与包络、4 段 TVA 包络、LFO1/LFO2），以及 `A10.bin` 88 键节奏鼓组。
- **多通道与 MIDI / SysEx 协议支持**：
  - 16 通道 GS / GM2 多音色复音合成；通道 10 鼓组（支持 Mute Group 与单键调音/TVA）；
  - 支持 Bank Select（CC 0 / CC 32）、Program Change、Pitch Bend、CC 1/7/10/11/64/91/93、RPN 0/1/2（弯音范围、粗调、细调实时生效）、GS Reset、GM System On、All Notes Off、Reset All Controllers；
  - 支持 **SD-90 Native DT1 SysEx**（`10 00 2p 00` 分部参数、`11..14 pp xx 00` 临时 Patch/Tone 数据块、`13 30/31 xx 00` 临时鼓组键位），以及 **GS Master Tune**（`F0 41 <dev> 42 12 40 00 00 ...`）与 **GM2 Universal Master Fine/Coarse Tune**；
  - 内置 4 点三次插值、前向循环与 Ping-Pong 乒乓循环展开、声道级立体声 Chorus 与 Reverb，以及 VST2.4 宿主双端口独立实例化隔离（兼容 `vstbridgeapp64.exe` Port A / Port B 混音架构）。
- **交互式 VST2 编辑器界面与轨道调音**：
  - 内置双缓冲 GDI 编辑窗口（`670×412`），实时显示 16 轨 Bank (MSB/LSB)、Program、实际 Patch 名称、音量（VOL）、表情（EXP）、声像（PAN）、音高偏置（`TUNE(st)`）与声部电平条；
  - 支持直接在插件界面中点击每轨 **`[-]` / `[+]`** 进行轨道独立移调与微调（左键 `±1.0` 半音，右键或 `Shift + 左键` `±0.10` 半音，点击中间数值复位），以及右上角 **`A4`** 基准音高快速切换（`440.0 / 432.0 / 452.0 Hz`）。

---

## 发行包与产物说明

可从 [GitHub Releases](https://github.com/Sophon3266/SD90-Player/releases) 直接下载打包好的 Windows x64 二进制压缩包，或自行编译生成至 `build/`：

| 文件 | 说明 |
|---|---|
| `SD90Player.dll` | 64 位 VST2.4 音源插件（PE32+，导出 `VSTPluginMain`，静态链接 MinGW 运行库） |
| `SD90Player.paths.ini` | 运行时路径配置文件（`rom_dll` 与 `banks` 指向本机原插件目录） |
| `sd90play.exe` | 独立实时 MIDI 播放器（与插件共用同一合成引擎，通过 `waveOut` 直接输出音频） |
| `sd90render.exe` | 独立 MIDI → WAV 离线渲染与 Bank 转储工具 |
| `vsthost_test.exe` | 最小 VST2 宿主自检与离线对比渲染工具 |

---

## 运行时路径配置

在使用前，确保 `SD90Player.paths.ini` 与 `SD90Player.dll`（或 `.exe`）位于同一目录，并将其中的路径修改为本机实际的 **SD-8 XS TISR** 文件位置：

```ini
source=sd8
rom_dll=C:/Path/To/SD-8 XS TISR/SD-8 XS VST File/SD-8 XS_TISR.dll
banks=C:/Path/To/SD-8 XS TISR/Bank Files
```

> 请勿包含 `SRX KEYBOARDS` 或 `SOUND Canvas VA` 作为音源；仅需原版 `SD-8 XS_TISR.dll` 与 `Bank Files/A01.bin` .. `A11.bin`。

---

## 使用方式

### 1. 配合 VST MIDI Driver (x64) 实时播放
1. 下载并解压 Release（或自行编译），按上节配置好 `SD90Player.paths.ini`。
2. 打开 **Configure VST MIDI Driver (x64)**（建议保留个人既有预设），加载 `SD90Player.dll` 并应用。
3. 在 MIDI 播放器中选择 `VST MIDI Driver` 作为 MIDI 输出端口即可；打开 VST Dialog 可实时查看各轨音色并调节各轨移调/A4 基准音高。

> 插件在 `effGetPlugCategory` 返回 `kPlugCategSynth` (`2`)，支持 `effGetProgramNameIndexed` / `effGetProgram`、MIDI / SysEx 事件及独立编辑窗口，并在每次 `VSTPluginMain()` 调用中于 `AEffect::object` 分配独立引擎实例（保留 `AEffect::user` 给宿主）。

### 2. 独立实时播放 MIDI（`sd90play.exe`）
```bat
sd90play.exe <input.mid> [--rate 44100] [--vol 0.8] [--tail 2.5] [--a4 440] [--tune <ch|all>:<semis>] [--quiet]
```

示例：
```bat
REM 以 A4=432Hz 播放，同时将第 1 轨升高 2 个半音
sd90play.exe song.mid --a4 432 --tune 1:+2
```

### 3. 离线渲染 WAV 与 VST2 一致性校验
```bat
sd90render.exe <input.mid> <output.wav> [--a4 <hz>] [--tune <ch|all>:<semis>]
sd90render.exe --dump-banks
vsthost_test.exe --vst C:\Full\Path\To\SD90Player.dll <input.mid> <output_vst.wav>
```

---

## 自行编译

需要 **MinGW-w64 `g++`**（支持 C++17，x86-64，无其他第三方依赖）。在仓库根目录执行：

```bat
build.bat
```

编译产物输出至 `build/` 目录。

---

## 已知限制

- MFX 多重效果器块（`11 00 02 00`）与 Step-LFO 目前仅解析或占位；Reverb / Chorus 使用简化立体声算法，而非 Roland 专有 SRV/RSS DSP。
- 个别带极端低通截止频率的合成器音色，其高次谐波滚降仍可能与原插件硬件建模存在细微听感差异。
- 本项目仅包含播放与合成引擎代码，需用户本机具备原版 `SD-8 XS_TISR.dll` 与 `Bank Files` 方可发声。
