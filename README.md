# MCPE-ImGuiMod (`libmcmod.so`)

《我的世界》基岩版（Bedrock，`com.mojang.minecraftpe`）运行期模组：以独立的 `.so` 形式注入游戏进程，用 **Dear ImGui + OpenGL ES 3** 直接在游戏画面上绘制菜单，不修改原 APK、不重签。

配套启动器：[直装分身](https://github.com/ATEXWONB/AppLauncher-Dual) —— 启动器会在 guest 进程里 `System.load` 本模组。

- 作者：**BY: ATEXWONB**
- 目标版本：Bedrock 1.26.62（arm64-v8a）
- 产物：`libmcmod.so`（约 4.3 MB，release + strip）

## 能做什么

| 模块 | 说明 |
| --- | --- |
| ImGui 菜单 | `Menu/ImGui.cpp` + `BlurBg.cpp`：悬浮窗、高斯模糊背景、灵动岛式状态条，防触摸穿透 |
| 内存读写 | `KittyMemory/`：`KittyMemory`、`MemoryPatch`、`MemoryBackup`，配合 Dobby inline hook |
| 类/符号解析 | `Il2cpp/`：从 `libminecraftpe.so` 的 il2cpp 元数据里定位类、字段、方法 |
| 原生 NBT | 读已完整可用；JSON→Tag 树构造已实测正确，可整体替换手持物品的 NBT |
| 瞄准 / 血液特效 | `Mod/Aim.c`（视角与目标计算）、`Mod/Blood.c` |
| VM Hook | `Mod/VmHook.c` + `VmHookAsm.S`：ARM64 汇编跳板，做函数级拦截 |
| 日志 | 写到 `/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.log` |

`MOD_NOTES.md` 是开发笔记（锚点、偏移、失败过的方案都在里面），`能力锚点-全量类索引.md` 是已确认的类/字段清单，改功能前先看这两份。

## 目录

```
jni/
  Android.mk  Application.mk  Main.cpp
  Mod/         McMod.cpp  Aim.c  Blood.c  VmHook.c  VmHookAsm.S
  Menu/        ImGui.cpp  BlurBg.cpp  island.h
  KittyMemory/ 内存读写与 patch
  Il2cpp/      il2cpp 元数据解析
  Dobby/       {arm64-v8a,armeabi-v7a,x86,x86_64}/libdobby.a（预编译静态库）
  asmjit/      汇编器（x86/arm 两套后端）
  nlohmann/    JSON
  imgui/       Dear ImGui + backends(android / opengl3)
prebuilt/{arm64-v8a,armeabi-v7a,x86,x86_64}/libmcmod.so   已编译好的四架构成品，可直接用
```

## 第三方代码与许可

本项目的第三方组件（Dear ImGui 1.93.1 WIP、Dobby `0932d69`、asmjit 1.13.0、nlohmann/json 3.11.3、xdl、KittyMemoryEx、obfuscate.h、il2cpp 头文件、Roboto 字体）的**上游版本、许可、能否商用**逐条列在 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。一句话：这些库都是 MIT / Apache-2.0 / Zlib，代码层面允许商用，保留声明即可；但把它当商业外挂卖违反 Minecraft EULA，与开源许可无关。

## 编译

```bash
export NDK=/path/to/android-ndk-r26d      # 实测 r26d
$NDK/ndk-build -j4 \
  NDK_PROJECT_PATH=. \
  APP_BUILD_SCRIPT=jni/Android.mk \
  NDK_APPLICATION_MK=jni/Application.mk
# 产物: libs/arm64-v8a/libmcmod.so
```

四个架构都能编（`APP_ABI := arm64-v8a` 改成 `armeabi-v7a` / `x86` / `x86_64` 即可）：

- **arm64-v8a**：完整功能，正常使用。
- **armeabi-v7a / x86 / x86_64**：可以编译、可以加载，但**功能未适配**。原因有两个，`Android.mk` 与 `McMod.cpp` 里已做条件编译：
  1. `VmHookAsm.S` 是纯 AArch64 汇编跳板（`stp x0,x1` / `br x16`），只有 arm64-v8a 参与编译；其它架构不编译 `VmHook.c`，虚表 hook 功能不可用。
  2. `McMod.cpp` 的 `CallClone` 用 ARM64 内联寄存器约束（`x0`/`x8`/`blr`），已用 `#if defined(__aarch64__)` 包起来，其它架构退化为普通 C 调用。
  3. 更关键的是**所有偏移/虚表地址都是针对 1.26.62 的 arm64 `libminecraftpe.so` 反推的**，32 位或 x86 版游戏里这些硬编码偏移全部不对，所以这些产物只是「架构正确的空壳」，能加载、不会有实际效果。要真正支持需要按对应架构的 so 重新找锚点。

`prebuilt/` 下四个架构的成品都在，验证过 ELF `e_machine`：arm64=0xB7、arm32=0x28、x86_64=0x3E、x86=0x03。

## 加载方式

1. 有 root：把 `libmcmod.so` 放到 `/data/local/tmp/libmcmod.so`，由注入器在游戏进程里 `System.load`。
2. 无 root（推荐）：用配套「直装分身」启动器 —— 启动器把 `libmcmod.so` 放在自己 assets 里，进入 guest 进程后按 ELF 架构校验、再 `System.load`。启动器会读 ELF `e_machine`，架构不匹配就跳过，所以全架构 APK 里塞一个 ARM64 的 so 也不会把 32 位进程搞崩。

## 免责

仅供学习研究、单机/私人服务器自用。**不要**用于联机作弊、商业外挂分发。使用本模组导致的封号、数据损坏等后果由使用者自行承担。

## 许可

MIT，见 `LICENSE`。Dear ImGui（MIT）、Dobby（Apache-2.0）、asmjit（Zlib）、nlohmann/json（MIT）、KittyMemory（MIT）各自遵循其原始许可。
