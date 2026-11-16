# SD90 Player（SD90-like）

基于 **SD-8 / XV（`SD-8 XS_TISR.dll`）** 采样引擎与音色库开发的 Windows 64 位 **VST2.4 音源插件与独立 MIDI 播放器**，支持 GM / GS / GM2 / SD-90 Native 音色播放标准 MIDI 文件，并可直接被本机 **VST MIDI Driver (x64)** 加载。

> **合规说明**：最终产物（`SD90Player.dll`、`sd90play.exe`、`sd90render.exe`、`vsthost_test.exe`）**仅包含自研代码与音色映射逻辑，不内嵌、也不分发任何 Roland 原插件或 ROM 采样数据**。运行时直接把本机原版 `SD-8 XS_TISR.dll` 作为本地 PE 数据文件读取（仅解析 `.rsrc` WROM 资源与 `.rdata` 表，`SD90Player.dll` 不会对其调用 `LoadLibrary`）。

---

## 核心特性

- **双采样引擎（8-bit RDAC DPCM 解码为浮点采样）**：
  1. **InternalA WROM**（`WROM/INTERNALA` + `.rdata` 采样表）：`3111` 组波形、`1083` 个 Multisample。
  2. **SRX / DMZ07 WROM**（`WROM/SRX`）：`2212` 组波形、`589` 个 Multisample（覆盖 10 通道鼓组与大部分旋律 Patch）。
- **KoaBankFile 音色库解析（`A01..A11.bin`）**：
  - 完整读取 Patch Common、TMT（键位/力度分层）、Tone 1–4 参数（Wave Group/ID、左右波形、粗/细调音、声像、TVF 滤波与包络、4 段 TVA 包络、LFO1/LFO2），以及 `A10.bin` 88 键节奏鼓组。
- **多通道与 MIDI 协议支持**：
  - 16 通道 GS / GM2 多音色复音合成；通道 10 鼓组（含 Mute Group 与鼓调音/TVA）；
  - 支持 Bank Select（CC 0 / CC 32）、Program Change、Pitch Bend、CC 1/7/10/11/64/91/93、RPN 0/1/2（弯音范围、粗调、细调）、GS Reset、GM System On、All Notes Off、Reset All Controllers；
  - 支持 **SD-90 Native DT1 SysEx**：`10 00 2p 00` 声部参数、`11..14 pp xx 00` 临时 Patch/Tone 数据块、`13 30/31 xx 00` 临时鼓组键位；
  - 内置 4 点三次插值、前向循环与 Ping-Pong 乒乓循环展开、声道级立体声 Chorus 与 Reverb，以及 VST2.4 宿主双端口独立实例化隔离（兼容 `vstbridgeapp64.exe` Port A / Port B 混音架构）。

---

## 产物目录结构

编译产物位于 `build/`，整理后的独立可运行交付包位于 `dist/`：

| 文件 | 说明 |
|---|---|
| `SD90Player.dll` | 64 位 VST2.4 音源插件（PE32+，导出 `VSTPluginMain`，无外部 MinGW 运行库依赖） |
| `SD90Player.paths.ini` | 运行时路径配置文件（`rom_dll` 与 `banks` 指向本机原插件目录） |
| `sd90play.exe` | 独立实时 MIDI 播放器（与离线渲染共用同一合成引擎，通过 `waveOut` 直接输出） |
| `sd90render.exe` | 独立 MIDI → WAV 离线渲染工具 |
| `vsthost_test.exe` | 最小 VST2 宿主自检与渲染对比工具 |

---

## 编译构建

需要 **MinGW-w64 `g++`**（支持 C++17，x86-64）。在项目根目录执行：

```bat
build.bat
```

编译输出至 `build/` 目录（无需第三方外部依赖库）。

---

## 运行时路径配置

确保 `SD90Player.paths.ini` 与 `SD90Player.dll`（或可执行文件）位于同一目录，配置本机原插件与 Bank 目录路径：

```ini
source=sd8
rom_dll=D:/Desktop/SD90-like/ref/SD-8 XS TISR/SD-8 XS VST File/SD-8 XS_TISR.dll
banks=D:/Desktop/SD90-like/ref/SD-8 XS TISR/Bank Files
```

> 若将 `dist/` 复制到其他路径或机器使用，请按本机实际路径修改 `dist/SD90Player.paths.ini` 中的 `rom_dll=` 与 `banks=`。

---

## 使用方式

### 1. 配合 VST MIDI Driver (x64) 实时播放
1. 打开 **Configure VST MIDI Driver (x64)**（不要覆盖已有个人预设）。
2. 选择 `dist\SD90Player.dll`（或 `build\SD90Player.dll`）并应用。
3. 在 MIDI 播放器中选择 `VST MIDI Driver` 作为 MIDI 输出设备即可。

> 该插件在 `effGetPlugCategory` 返回 `kPlugCategSynth` (`2`)，支持 `receiveVstMidiEvent` / `receiveVstSysexEvent`，并在每次 `VSTPluginMain()` 调用分配独立实例指针（存于 `AEffect::object`，保留 `AEffect::user` 给宿主使用）。

### 2. 独立实时播放 MIDI（`sd90play.exe`）
```bat
dist\sd90play.exe <input.mid> [--rate 44100] [--vol 0.8] [--tail 2.5] [--quiet]
```

### 3. 离线渲染 WAV 与 VST2 一致性校验
```bat
dist\sd90render.exe <input.mid> <output.wav>
dist\sd90render.exe --dump-banks
dist\vsthost_test.exe --vst D:\Desktop\SD90-like\dist\SD90Player.dll <input.mid> <output_vst.wav>
```

---

## 已知限制

- MFX 多重效果器块（`11 00 02 00`）与 Step-LFO 目前仅解析或占位；Reverb / Chorus 使用简化立体声算法，而非 Roland 专有 SRV/RSS DSP。
- 无图形编辑界面（仅暴露 Master / Reverb 两个 VST 参数）。
- 个别带极端低通截止频率的合成器音色（如 `TVF cut=16`）高次谐波滚降可能比原插件更暗，后续可继续微调截止曲线。

---

## 相关文档与工作日志

- [AGENTS.md](AGENTS.md)：项目硬性约定与合规边界
- [docs/program-map.md](docs/program-map.md)：GM2 / Native Mode 音色库与 Bank 映射说明
- [docs/tone-map.md](docs/tone-map.md)：KoaBankFile Tone 参数偏移与字段说明
- [docs/decoder-notes.md](docs/decoder-notes.md)：8-bit RDAC DPCM 与循环逻辑逆向记录
- `worklog/`：阶段性工作进展与验证记录
