# 第三方代码与许可说明（THIRD-PARTY NOTICES）

本仓库 `libmcmod.so` 的源码由本项目原创部分与下列第三方开源组件组成。下表的「版本」是仓库内实际 vendored 的版本（不是「最新版」），升级时请以本表为基准同步。

## 一、第三方组件清单

| 组件 | 上游仓库 | 仓库内版本 / 标识 | 许可 | 可否商用 |
| --- | --- | --- | --- | --- |
| Dear ImGui | https://github.com/ocornut/imgui | **1.93.1 WIP**（`IMGUI_VERSION_NUM 19303`），目录 `jni/imgui/` | MIT | ✅ 可商用，保留版权与许可声明 |
| Dobby | https://github.com/jmpews/Dobby | **commit `0932d69c320e786672361ab53825ba8f4245e9d3`**（该提交之后上游无法直接编译，见 `jni/Dobby/README.md`）；四个架构的 `libdobby.a` 为预编译静态库 | Apache-2.0 | ✅ 可商用，需保留 NOTICE |
| asmjit | https://github.com/asmjit/asmjit | **1.13.0**（`ASMJIT_LIBRARY_VERSION 1,13,0`）；`libasmjit.a` 预编译 | Zlib | ✅ 可商用，不得用作者名义背书 |
| nlohmann/json | https://github.com/nlohmann/json | **3.11.3**（单头 `json.hpp`） | MIT | ✅ 可商用 |
| xdl | https://github.com/hexhacking/xdl | 源码 `jni/Il2cpp/xdl/`（HexHacking Team, 2020-2021 版） | MIT | ✅ 可商用 |
| KittyMemory / KittyMemoryEx | https://github.com/MJx0/KittyMemoryEx | `jni/KittyMemory/`（KittyMemory、MemoryPatch、MemoryBackup、KittyUtils） | MIT | ✅ 可商用 |
| obfuscate.h | https://github.com/adamyaxley/Obfuscate | `jni/Includes/obfuscate.h`（Adam Yaxley） | MIT | ✅ 可商用 |
| il2cpp 结构定义 | https://github.com/Perfare/Il2CppDumper 、https://github.com/AndnixSH/Il2CppInspector 生态 | `jni/Il2cpp/il2cpp-*.h`、`il2cpp-class.*`（`Il2CppDumper` 的 `il2cpp-class.h`/`tabledefs.h` 派生） | MIT（上游为 MIT，头文件已注明来源） | ✅ 可商用 |
| Roboto 字体 | https://github.com/googlefonts/roboto | `jni/Includes/Roboto-Regular.h`（字体数据以 C 数组内嵌） | Apache-2.0 | ✅ 可商用 |
| Android NDK 头文件/STL | https://developer.android.com/ndk | 构建期链接（`APP_STL := c++_static`） | Apache-2.0 / LLVM 例外条款 | ✅ 可商用 |

## 二、本项目原创部分

`jni/Mod/`（McMod.cpp、Aim.c、Blood.c、VmHook.c、VmHookAsm.S）、`jni/Menu/`（ImGui.cpp、BlurBg.cpp、island.h）、`jni/Tool/`、`jni/Unity/`、`jni/Main.cpp`、`jni/Includes/{Logger,Utils,Macros,circular_buffer}.*` —— 由 ATEXWONB 编写，按本仓库根目录 `LICENSE`（MIT）发布。

## 三、商用结论

- **代码许可层面**：上表全部组件均为 MIT / Apache-2.0 / Zlib，**允许商业使用**，前提是保留各自的版权声明与许可文本（MIT/Zlib 要求保留声明；Apache-2.0 要求保留 NOTICE 并声明修改）。
- **实际使用层面**：本模组是注入《我的世界》基岩版客户端的内存模组。把它用于**商业外挂分发、联机作弊、代练/售卖功能**会违反 Microsoft/Mojang 的《Minecraft 使用条款》与服务条款，这与开源许可无关——开源许可只授予你「使用/修改/再分发这份代码」的权利，不授予你「违反游戏 EULA」的权利。仓库按 MIT 发布不等于背书这类用法。
- 若你要把本项目打包进商业产品，请自行确认：① 保留全部第三方许可文本；② 不删除源码中的版权头；③ 不宣称原作者为你的产品背书。

## 四、许可文本位置

- MIT：各文件头部；`LICENSE`
- Apache-2.0（Dobby、Roboto）：见 `jni/Dobby/README.md` 指向的上游仓库
- Zlib（asmjit）：见 `jni/asmjit/` 上游
