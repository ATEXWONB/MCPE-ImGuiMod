# 《我的世界》基岩版运行期模组 libmcmod.so —— 开发笔记

> 维护：Doubao（每次有新增发现都追加到对应章节，**不要新建文件**）
> 最后更新：2026-11-21
> 项目根：`/mnt/500G/tmp/mcpe/imgui/`（源码 `jni/`，产物 `libs/arm64-v8a/libmcmod.so`）

---

## 0. 一句话现状

原生 NBT **读**已 100% 可用；原生 NBT **构造**（JSON→Tag 树）已实测正确；
`NJ <json路径>` 指令已能把手持物品的 tag 整个替换成 JSON 描述的新树。
**UI（输入框 + 文件夹管理器 + 导入按钮）尚未开始。**

---

## 1. 目标设备与关键路径

| 项 | 值 |
|---|---|
| 平板 | 小米 6S Pro，IP `192.168.4.243`，Root = KernelSU + KPatch-Next |
| 包名 | `com.mojang.minecraftpe`（Bedrock 1.26.62.01） |
| 游戏 so | `/data/app/~~oun3bBJ-JwAjnsw79TPuRw==/com.mojang.minecraftpe-5RenPG9i-V_g9E8WIFw1sg==/lib/arm64/libminecraftpe.so`（342,231,392 B） |
| 模组 so 加载路径 | **`/data/local/tmp/libmcmod.so`**（注入器固定读这个路径） |
| 模组日志 | `/storage/emulated/0/Android/data/com.mojang.minecraftpe/mcmod.log` |
| 指令通道 | `/sdcard/act.txt`（= `/storage/emulated/0/act.txt`） |
| 配置 | `/sdcard/mcmod.cfg` |
| MCP（平板 Termux） | `http://192.168.4.243:65534/mcp`，本机调用 `C:\Users\admin\mcp2.ps1 -Tool shell_exec` |
| HTTP 分发（服务器） | `http://192.168.4.132:8898/<file>` 对应 `/opt/dist/`（往平板传文件的**唯一通道**） |

---

## 2. 构建

```bash
cd /mnt/500G/tmp/mcpe/imgui
/opt/android-sdk/ndk/android-ndk-r26d/ndk-build -j4 \
  NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
# 产物 → libs/arm64-v8a/libmcmod.so，~4.29 MB，单次增量构建约 20s
```

- 只改了 `Mod/McMod.cpp` 时是增量编译，20s 内出结果，**不会撞服务器 60s 硬超时**。
- NDK 有三个：`21.4.7075529`、`26.3.11579264`、`android-ndk-r26d`。用的是 **r26d**。
- `Application.mk`：`APP_ABI=arm64-v8a`、`APP_PLATFORM=android-29`、`APP_STL=c++_static`、release。

---

## 3. 部署与注入（最容易卡住的一步）

### 3.1 模组不是自动加载的！

- `libminecraftpe.so` 的 `DT_NEEDED` 里**没有** libmcmod.so（已验证 `readelf -d`）。
- APK lib 目录里虽然也放了 libmcmod.so，但那不是加载入口。
- 真正入口：**每次启动游戏后用 `/data/tinjector` 手动注入 `/data/local/tmp/libmcmod.so`**。
- 判断是否已在运行：看 `mcmod.log` 是否在增长 + `act.txt` 是否被消费；`/proc/<pid>/maps` 里 grep mcmod 只有用非 `--hide` 注入时才有名字。

### 3.2 注入器用法（`/data/tinjector`，338,256 B）

```bash
/data/tinjector                     # 打印用法
/data/tinjector -n com.mojang.minecraftpe /data/local/tmp/libmcmod.so   # ← 用这个
```

| 参数 | 结果 |
|---|---|
| `-n <包名> <so>` | **可用**，会打印「注入成功。」 |
| `-P <pid> <so>` | **直接段错误**（tombstone 里 Cmdline 就是 tinjector 自己） |
| `--hide` / `--hide1` | 注入后**崩溃**，且最后报「获取模块基址失败」 |
| `-f -p <包名> <so>` | 孵化新进程，本次未验证 |

**结论：只用 `-n <包名> <so>`，不要加 `--hide`。**

### 3.3 更新流程（完整、按顺序）

```bash
# 1) 服务器：产物拷到分发目录
cp libs/arm64-v8a/libmcmod.so /opt/dist/libmcmod_new.so

# 2) 平板：下载 + 强制停游戏 + 覆盖 + 启动
su -c "curl -sS -o /data/local/tmp/libmcmod_new.so http://192.168.4.132:8898/libmcmod_new.so; \
  am force-stop com.mojang.minecraftpe; sleep 2; \
  cp /data/local/tmp/libmcmod_new.so /data/local/tmp/libmcmod.so; chmod 644 /data/local/tmp/libmcmod.so; \
  monkey -p com.mojang.minecraftpe -c android.intent.category.LAUNCHER 1"

# 3) 取 pid（注意：su -c 里写 $(pidof ...) 会拿到空值，分开两步取）
pidof com.mojang.minecraftpe

# 4) 注入
su -c "/data/tinjector -n com.mojang.minecraftpe /data/local/tmp/libmcmod.so"
```

**坑：绝对不要在游戏还映射着旧 so 的时候直接覆盖 `/data/local/tmp/libmcmod.so`**，
先 `am force-stop` 再 `cp`。

---

## 4. 指令通道 `/sdcard/act.txt`

- 格式：每行 `op arg off val`（`sscanf("%15s %39s %llx %llx")`）。
- **只在游戏前台渲染时消费**，处理完 `remove()` 整个文件。
- 现有指令：`INV`、`ENCH`、`ITEMV`、`ITEMID`、`FINDP`、`S16`、`VT`、**`NJ`（新增）**。
- 新增写法（在 `ProcessActFile()` 里加分支）：

```cpp
if (!strcmp(op, "NJ")) { MCLOG("[nj] file=%s", a); NbImportJsonFile(a); continue; }
```

- 验证是否被消费：`ls /sdcard/act.txt` 文件消失即处理过；日志看 `[act] begin/end`。

---

## 5. 库偏移总表（全部是相对 libminecraftpe.so base 的**静态**地址）

### 5.1 基址与重定位

- base 必须取 `maps` 里**偏移为 0** 的那段（`offset==0`），不能取 `min(s-o)`。
- `.data.rel.ro` 在文件里全是 0，**必须先自解 `.rela.dyn`（868,395 条 R_AARCH64_RELATIVE）** 才能读虚表/typeinfo。
- 运行时大量指针带 `0xb4` 高标签：`(v & 0xFFFF000000000000)==0xB400000000000000` 时 `v &= 0x0000FFFFFFFFFFFF`。**读任何指针前先剥标签**。

### 5.2 类 / 虚表

| 用途 | 偏移 |
|---|---|
| LocalPlayer vptr | `0x139F74E0`（Player `0x139F8518`、Actor `0x13CC61A8`、Mob `0x13CB8A18`） |
| PlayerInventory vptr | `0x13D43B78` |
| Inventory vptr | `0x13D43160` |
| ItemStack vptr | `0x13D96650` |
| ClientInstance vptr | `0x138904B0` |
| MinecraftGame | `0x1389FEF8` |
| Minecraft | `0x13D076B8` |
| BlockSource | `0x137959D8` |
| Dimension | `0x137E3A50` |
| Level | `0x13950B90` |
| LevelData | `0x13E6EDA0` |
| GuiData | `0x1387A9A0` |
| CameraAPI / CameraClientInstance | `0x137919D8` / `0x13794508` |
| TickTimeManager / Client | `0x13E08610` / `0x13E08648` |
| LoopbackPacketSender | `0x13B3B258` |
| ChatScreenController | `0x138C1A80` |

### 5.3 NBT 相关（核心）

| Tag 类型 | typeinfo | vptr | clone(slot10) | 对象大小 | 值位置 |
|---|---|---|---|---|---|
| ByteTag | `0x13EBED50` | `0x13EBE698` | `0x11B9FCEC` | 0x10 | +8 byte |
| ShortTag | `0x13EBEE40` | `0x13EBEBE0` | `0x11BB07B8` | 0x10 | +8 int16 |
| IntTag | `0x13EBEDE0` | `0x13EBE938` | `0x11BA6EA4` | 0x10 | +8 int32 |
| Int64Tag | `0x13EBEDB0` | `0x13EBE858` | `0x11BA589C` | 0x10 | +8 int64 |
| FloatTag | `0x13EBED90` | `0x13EBE7E8` | `0x11BA5304` | 0x10 | +8 float |
| DoubleTag | `0x13EBED78` | `0x13EBE778` | `0x11BA4CDC` | 0x10 | +8 double |
| StringTag | `0x13EBEE58` | `0x13EBEC50` | `0x11BB0D90` | 0x20 | +8 libc++ string(24B) |
| ListTag | `0x13EBEDF8` | `0x13EBE9A8` | `0x11BA9388` | 0x28 | +8 begin / +0x10 end / +0x18 cap / +0x20 元素类型 |
| CompoundTag | `0x13EBED60` | `0x13EBE708` | `0x11BA3270` | 0x20 | +8 begin / +0x10 end_node.left(=root) / +0x18 size |

### 5.4 构造 NBT 需要的函数

| 功能 | 地址 | 签名 / 说明 |
|---|---|---|
| `operator new` | `0x135D3F30` | `void* f(size_t)`；**必须用它分配，后面游戏会 operator delete** |
| `operator delete` | `0xE831A80` | `void f(void*)` |
| CompoundTag::put | **`0x11BA0E40`** | `uintptr_t f(CompoundTag* this, std::string const* key, Tag* val)`，返回节点内联 Tag 指针（node+0x38） |
| 找/建节点 | `0x11BB2988` | put 内部调用，不用直接调 |
| Tag 按类型赋值 | `0x11BA0EBC` | put 内部调用（按 src 的 getType 分派拷贝） |
| libc++ string 初始化 | `0xE831AFC` | `void f(std::string* dst, const char* s, size_t n)`；**长字符串才需要，≤22 字节直接手工 SSO 更稳** |
| CompoundTag::clone | `0x11BA3270` | **只对 CompoundTag 有效**，别拿它 clone 别的类型（旧版本「复制 NBT 变空气」就是这个原因） |
| Compound 空初始化 | 见 6.2 | `{vptr, self+0x10, 0, 0}` |

---

## 6. NBT 内存结构（实测钉死的）

### 6.1 字符串是 libc++ 小字符串

- 首字节 = **长度 × 2**（bit0 是 is_long 标志），字符从 +1 开始，容量 22 字节。
- 所以之前看到的「1 字节＝2×长度」不是自定义编码，`Damage`(6字) → `0x0c` 完全对得上。
- key 和 StringTag 的值都是这个布局。

### 6.2 CompoundTag

```
+0x00 vptr(0x13EBE708)
+0x08 begin  (最左节点；空时 = self+0x10)
+0x10 end_node.left = **root 节点**（空时 = 0）
+0x18 size（条目数）
```

它内部是 libc++ `__tree`：节点有 **三条指针边** `+0x00`(left) / `+0x08`(right) / `+0x10`(parent)。
节点布局：`+0x20` 起是 24 字节 std::string key（`len<<1` 编码），**key 之后 8 字节对齐处就是内联 Tag 对象**（vptr 直接出现在节点里），节点 `+0x60` 存一个 int 类型 id（Compound=10）。

**遍历方法（实测唯一正确）**：从 `*(comp+0x10)` 取 root，沿 `+0x00/+0x08/+0x10` 做 **带 visited 集合的 BFS**，收集到 `comp+0x18` 个有效节点就停。
只沿 `+0x10` 走会漏（`ench` 就是只能从 `RepairCost` 的 `+0x08` 到达的）。

> Python 探针里已经这么实现（`/data/local/tmp/parseat.py`、`allnbt.py`），可直接用。

### 6.3 ListTag

- `+0x08` begin / `+0x10` end / `+0x18` cap，元素是 **8 字节的 Tag 指针**（unique_ptr 语义）。
- `+0x20` 是元素类型 byte。

---

## 7. JSON → NBT 构造（已实测正确）

类型映射（`Mod/McMod.cpp` 中 `J2Tag`）：

| JSON | Tag |
|---|---|
| `"64b"` | Byte |
| `"0s"` | Short |
| `"1l"` | Long |
| `"0.0f"` | Float |
| 纯数字 `19` / JSON 整数 | Int |
| JSON 小数 | Double |
| `true/false` | Byte 1/0 |
| 对象 | Compound |
| 数组 | List（元素类型取自第一个元素） |
| 其他字符串 | String |

构造要点：

```cpp
// Compound 空对象
p = new(0x20); *(uptr*)p = base+0x13EBE708;
*(uptr*)(p+0x08) = p + 0x10;
*(uptr*)(p+0x10) = 0;
*(uptr*)(p+0x18) = 0;

// 插键值（key 用 24 字节栈上 std::string，手动 SSO）
put(compound, keyStr, valTag);
```

- 必须走 `put`，**不能自己拼节点**（树要维持平衡/父子关系，手工拼游戏读不到）。
- `put` 会把 value **按类型复制**进节点（节点里是内联 Tag），所以传进去的临时 Tag 可以不管（泄漏比 double-free 安全）。
- 实测：`Damage→size=1`、`RepairCost→2`、`ench→3`、`display→4`，独立 Python 探针解析结果完全正确。

---

## 8. 背包 / 物品结构

```
PlayerInventory +0x10  : int，当前选中槽（0 基）
PlayerInventory +0xB8  : -> Inventory
Inventory +0x140       : vec begin
Inventory +0x148       : vec end        (end-begin)/0x98 = 36 格
ItemStack (stride 0x98):
   +0x00 vptr(0x13D96650)
   +0x08 -> Item 对象
   +0x10 -> CompoundTag 根（空物品为 0）
   +0x20 aux(short)
   +0x22 count(byte)
   +0x23 flag(byte)=1
```

- ItemStack 的 `+0x10` 直接换指针就能换整棵 NBT，这是目前导入功能的落点。

---

## 9. 踩过的坑（按严重度，**别再犯**）

1. **`--hide` 注入必崩**；`-P <pid>` 也崩。只用 `-n <包名> <so>`。
2. **不能在游戏运行时覆盖已映射的 so**；先 `am force-stop`。
3. 写入一颗「结构不对」的 CompoundTag 会让**游戏当场崩**（第一次测试就崩了，靠重启 + 手动把 `+0x10` 还原旧指针救回来）。
   → 正确做法：**先只构造 + 打印，确认结构对，再写**。`NbImportJsonFile` 已加此开关。
4. `su -c "... $(pidof ...) ..."` 会拿到空 pid（两套 shell 嵌套 + 引号），**分两步取 pid**。
5. 服务器 `run_shell` 有 **60s 硬超时**，长任务要 `nohup setsid ... &` 再轮询；但增量编译 20s 内没事。
6. 服务器命令里**不要塞单引号 + 管道混排**，会被 PowerShell/JSON 双层转义搞坏；用 `<<"PYEOF"` heredoc 传 python 最稳。
7. 用 PowerShell 给 shell 传 python 时，base64 手抄极易出错（`for i in` 变 `for i m in`）→ 优先 heredoc。
8. 静态读虚表必须先解 `.rela.dyn`，否则全是 0（曾经只索引到 7396/295307 条也是这个原因）。
9. 按基类 vptr 扫内存**永远扫不到**（实例指向派生类）；要从指针链取：
   LocalPlayer `+0x1E0` → OverworldDimension、`+0x1F0` → ClientLevel。
10. 别用 `0x11BA3270` 给非 Compound 类型做 clone。
11. 节点遍历必须带 visited 集合 + 条目数封顶，否则死循环/重复（我第一版 C++ reader 就栽在这）。
12. `mcmod.log` 由模组自己打开，路径是 `/storage/emulated/0/Android/data/<pkg>/mcmod.log`；进程没注入时它**不会增长**，别误判成「没日志」。
13. 判断模组是否活着：`act.txt` 是否被消费 + 日志是否增长；`grep mcmod /proc/<pid>/maps` 在 `--hide`/普通注入下都可能是 0 行。

---

## 10. 新增代码位置（`Mod/McMod.cpp`）

| 内容 | 大致位置 |
|---|---|
| `NB_*` 偏移宏 + `NbNewObj/NbStrSet/NbNum/NbString/NbCompound/NbList/NbPut` | `ProcessActFile()` 之前约 400 行 |
| `NbLogTree`（C++ 读取器，**待改成带 visited 的版本**） | 同上 |
| 极简 JSON 解析（`JV/JParse/J2Tag`） | 同上 |
| `NbImportJsonFile(const char* path)` | 同上 |
| `NJ` 指令分支 | `ProcessActFile()` 开头 |

---

## 11. 待办 / 下一步

1. **C++ 侧读取器**加上 visited 集合（对齐 Python 版），否则 UI 里读出来的树是乱的。
2. 实测 `NJ` 写入（当前已开启写入，但还没在世界里跑过一次干净的验证）。
3. 支持长字符串（>22 字节）的 StringTag：目前回退到 `0xE831AFC`，未验证。
4. 非方块物品的**显示名**（如 `minecraft:iron_sword`）不在 root NBT 里，在 Item 对象内部字符串池，尚未定位偏移。
5. UI：输入框 + 点击才弹出的文件夹管理器（默认 `/sdcard`）+ 下方「导入」按钮。
6. 旧指针（被替换掉的 tag）目前**故意不释放**，避免 double free；后续可考虑交给游戏 GC 或安全释放。

---

## 12. 设备上的探针脚本（root python 直读 /proc/<pid>/mem）

| 脚本 | 作用 |
|---|---|
| `/data/local/tmp/parseat.py <hexaddr>` | 从任意地址解析整棵 NBT 树（正确版，带 visited） |
| `/data/local/tmp/allnbt.py` | 导出全部 36 格的 NBT |
| `/data/local/tmp/invnbt.py` | 背包 NBT 简表 |
| `/data/local/tmp/nbtree2.py` | 手持物品 NBT 递归打印 |
| `/data/local/tmp/chk.py` | 扫 PlayerInventory 实例（慢，全内存扫描） |

注意：所有 python 探针都要先剥 `0xb4` 标签，base 取 `offset==0` 段。

---

## 13. Windows 侧 IDA MCP（ida-pro-mcp 1.30.0）安装记录

> 2026-11-21 装好，用于替代「肉眼读反汇编」的慢路径。

**位置**
- Python：`C:\Users\admin\.workbuddy\binaries\python\versions\3.13.12\python.exe`（3.13.14）
- 包：`Lib\site-packages\ida_pro_mcp\`（含 `server.py`、`mcp-plugin.py`、`idalib_server.py`）
- ~~安装器~~ / 备用副本：`D:\User\Desktop\asm_tools\IDA_MCP_plugin\`（`ida-pro-mcp.exe`、`mcp-plugin.py`）

**已执行的安装**
```powershell
& 'D:\User\Desktop\asm_tools\IDA_MCP_plugin\ida-pro-mcp.exe' --install
```
产出：
- IDA 插件 → `C:\Users\admin\AppData\Roaming\Hex-Rays\IDA Pro\plugins\mcp-plugin.py`
- Claude Desktop → `C:\Users\admin\AppData\Roaming\Claude\claude_desktop_config.json`
- Cursor → `C:\Users\admin\.cursor\mcp.json`
- Claude Code → `C:\Users\admin\.claude.json`

**注意的坑**
1. `--install` 会把**当前进程的 PYTHONPATH**（豆包沙箱路径）写进配置，跨客户端可能冲突 → 已手动把两处配置的 `env` 改为空对象，实测纯环境也能正常启动。
2. 正确参数是 `--install`，不是 `install`（写成 `install` 报 `unrecognized arguments`）。
3. 校验方法：起 stdio，发 `initialize` + `tools/list`，能回 **48 个工具**（`check_connection`、`decompile_function`、`disassemble_function`、`list_functions`、`get_xrefs_to`…）即服务正常。

**尚未满足的前置条件**
- 本机**未安装 IDA 本体**（Program Files / Cursor / 注册表 / `%APPDATA%\Hex-Rays` 全空）。工具能启动但一律返回「插件未运行」。
- IDA Free 官方下载站 `out7.hex-rays.com` 被网络挡，需用户自己在能联网的浏览器装（hex-rays.com/ida-free）。
- 装好后若插件不出现在菜单，把 `mcp-plugin.py` 拷到对应版本的 `plugins\` 目录再重启 IDA。
- **风险提示**：`libminecraftpe.so` 有 342 MB，IDA Free 对输入体积有上限，可能要按段/切片喂。

---

## 14. 重大进展：RTTI 全量类索引（2026-11-21）

**发现**：`.data.rel.ro` 里 RTTI typeinfo 完整保留，名字格式是「长度前缀+类名」（如 `11LocalPlayer`、`11CompoundTag`），
`typeinfo+8` 指向该字符串。非 `N...E` 形式，之前按 Itanium 正则去匹配所以全漏了。

**扫描方法**（脚本 `vtscan2.py`）：
1. 自解 `.rela.dyn`（R_AARCH64_RELATIVE）→ 得到 `.data.rel.ro` 的实际指针值；
2. 筛 typeinfo：`q(t+8)` 指向 `.rodata` 且字符串匹配 `^(\d+)(.+)$` 且长度自洽；
3. 反推虚表：找 `a` 使 `q(a) == typeinfo`，则 vtable = `a + 8`；
4. 数连续指向 `.text` 的槽位 = 虚函数个数。

**结果**：4,653 个类型 / 4,981 个带名虚表 →`mc_classes.txt`。

**实例定位新方法（虚表指纹扫描）**：内存里搜 qword == vtable 地址，命中地址就是对象 +0x00。

**关键新锚点（主表）**：
| 类 | 虚表 | 槽数 |
|---|---|---|
| LocalPlayer | 0x139F74E0 | 247 |
| Player | 0x13D43330 | 181 |
| ServerPlayer | 0x13C7C690 | 249 |
| ClientInstance | 0x138904B0 | 436 |
| ClientLevel | 0x1394FB50 | 413 |
| MinecraftGame | 0x1389FEF8 | 316 |
| Actor | 0x13CF5D40 | 137 |
| Mob | 0x13CC5328 | 175 |
| ItemActor（掉落物） | 0x13D4EAC0 | 137 |
| BlockSource | 0x13D6E328 | 67 |
| LevelRenderer | 0x13A555C0 | 36 |
| CameraAPI | 0x137919D8 | 18 |
| ChatScreenController | 0x138C1A80 | 54 |
| InGamePlayScreen | 0x138D39F8 | 109 |
| MouseHandlerAndroid | 0x13ABD7E0 | 7 |
| LoopbackPacketSender | 0x13B3B258 | 12 |

**纠正**：`Actor` 旧值 0x13CC61A8、`BlockSource` 旧值 0x137959D8 都是残缺副本，主表见上。
同一类名有多个虚表副本（不同 TU），扫实例要全部当候选。

## 15. Windows 侧 Ghidra 12.2 + JDK25（2026-11-21）

- JDK25（Temurin 25.0.5.1）→ `D:\Tools\jdk25\jdk-25.0.5.1+2`
- Ghidra 12.2（543MB zip）→ `D:\Tools\ghidra\gh\ghidra_12.2_PUBLIC`
- 环境变量（用户级）：`JAVA_HOME`、`GHIDRA_HOME`
- 桌面快捷方式：`Ghidra 12.2.lnk`
- 验证：`analyzeHeadless` 可用，notepad.exe 导入成功；`libminecraftpe.so` 正在导入项目 `D:\Tools\ghidra\proj\mc`
- **坑**：Ghidra 12.2 **必须 JDK25+**（JDK21 报“unsupported java version”）；GitHub 直连很慢且会断，用 `aria2c -x16` 能从 ~45KB/s 提到 ~2-4MB/s；Adoptium 还可走清华镜像 `mirrors.tuna.tsinghua.edu.cn/Adoptium/`（curl 可，aria2 多连会 403）。

## 16. Windows 侧 IDA Professional 9.0 + ida-pro-mcp 打通（2026-11-21 晚，**已连通**）

IDA 本体：`C:\Program Files\IDA Professional 9.0\ida.exe`（9.0.240925）。
MCP 插件：`C:\Users\admin\AppData\Roaming\Hex-Rays\IDA Pro\plugins\mcp-plugin.py`（`ida-pro-mcp 1.30.0 --install` 装的）。
拷进 `C:\Program Files\IDA Professional 9.0\plugins\` 被系统拒绝（非管理员），**别浪费时间**，用户级 plugins 目录就够。

### 16.1 必须解决的前提：IDAPython 要外部 Python，且版本要对

- IDA 9.0 不带内置 Python，靠 `idapyswitch.exe` 选系统里的 Python。
- 配置：`cd "C:\Program Files\IDA Professional 9.0"` 后执行
  `idapyswitch.exe --force-path D:\Tools\python312\python3.dll`
  **必须传 python3.dll 全路径**；传目录会报 `No "python3.dll" >= 3.8 found`。退出码 0 就是成功（它写哪里不用管，重启 IDA 生效）。
- 坑：先试了 WorkBuddy 自带的 3.13.12（`C:\Users\admin\.workbuddy\binaries\python\versions\3.13.12`），`python313.dll` 确实被 IDA 加载，但 **MCP 插件不注册、Ctrl+Alt+M 无任何反应**。换 **Python 3.12** 后一次成功 → 结论：IDA 9.0 + 3.13 不稳，用 3.12。
- Python 3.12.10 安装包：`https://mirrors.huaweicloud.com/python/3.12.10/python-3.12.10-amd64.exe`（python.org 直连会 EOF 断流）。静默安装到 `D:\Tools\python312`：
  `python-3.12.10-amd64.exe /quiet InstallAllUsers=0 TargetDir=D:\Tools\python312 Include_test=0 Include_doc=0 Include_launcher=0 AssociateFiles=0 PrependPath=0 Shortcuts=0 SimpleInstall=1`

### 16.2 不再依赖 Ctrl+Alt+M：用 idapythonrc.py 自动拉服务（**关键**）

`C:\Users\admin\AppData\Roaming\Hex-Rays\IDA Pro\idapythonrc.py`（IDAPython 启动时自动执行）：

```python
# IDAPython 启动脚本：自动拉起 MCP 服务（等价于 Ctrl+Alt+M）
import sys, traceback
LOG  = r'D:\Tools\ida_autostart.log'
PLUG = r'C:\Users\admin\AppData\Roaming\Hex-Rays\IDA Pro\plugins\mcp-plugin.py'
try:
    src = open(PLUG, encoding='utf-8').read()
    ns = {'__name__': 'mcp_plugin_auto', '__file__': PLUG}
    exec(compile(src, PLUG, 'exec'), ns)
    ns['Server']().start()
    open(LOG, 'a', encoding='utf-8').write('mcp autostart ok\n')
except Exception:
    open(LOG, 'a', encoding='utf-8').write(traceback.format_exc() + '\n')
```

- 服务在 daemon 线程里起，`localhost:13337`；启动日志写在 `D:\Tools\ida_autostart.log`，出问题先看它。
- 为什么不用快捷键：**必须让 IDA 窗口真的拿到前台焦点**。我这边后台 pwsh/conhost 窗口会抢焦点，快捷键反复打空；而且 Quick start（无数据库）窗口下按了也没反应。

### 16.3 验证结果（已实测）

- `Get-NetTCPConnection -State Listen | ? LocalPort -eq 13337` → 1 条监听。
- MCP `check_connection` → `Successfully connected to IDA Pro (open file: libminecraftpe.so)`。
- `get_metadata` → path `D:\User\Desktop\我的世界ki\libminecraftpe.so`，base `0x0`，size `0x14678260`，md5 `ed8ac2c33a788478371503fc5731cf5a`（与桌面副本一致）。
- **IDA 里 base = 0x0**，所以 IDA 地址 == ELF vaddr == 文档里所有 `0x11BAxxxx` 这种偏移，不用再加基址。

### 16.4 打开整库的正确姿势

```powershell
Start-Process 'C:\Program Files\IDA Professional 9.0\ida.exe' -ArgumentList '-A','"D:\User\Desktop\我的世界ki\libminecraftpe.so"'
Start-Sleep -Seconds 8   # 342MB ELF，加载要等
```
- `-A` 必带：否则会弹 ELF 加载选项框，MCP 就永远连不上。
- **坑**：342MB 全量自动分析非常慢（实测分析中调 `decompile_function` 直接 60s 超时、后续 MCP 调用还会因主线程被占用报“连接器暂时不可用”）。
  → 战术：**用 IDA 只做定点反编译**（已知地址才调），批量扫类/找虚表继续用 `vtscan2.py` + `mc_classes.txt`；等 IDA 分析进度跑完再批量用。
- 分析进度看主窗口标题栏 / 状态栏；`Get-Process ida | select CPU,WorkingSet64` 也能判断在不在跑。

### 16.5 常用 MCP 调用（工具名固定）

- `decompile_function(address="0x11BA0E40")` —— `CompoundTag::put`
- `disassemble_function(start_address="0x135D3F30")` —— `operator new`
- `list_functions(offset=0, count=100)` / `get_metadata()` / `check_connection()`
- 改名/改原型：`rename_function` / `set_function_prototype`（工具名见 server 的 tools/list）
- 反编译质量差时优先核对：IDA 是否把该地址识别成函数（未识别就先 `create_function`/`U`+`C`，否则反编译出来是 `sub_xxx` 垃圾）

### 16.6 备用通道：直连 HTTP JSON-RPC（连接器挂了也能用）

MCP 的 stdio 连接器一次 60s 超时就会整个断开（报 `Transport closed` / `CONNECTOR_NOT_AVAILABLE`），**不要再依赖它**。插件本身就是个 HTTP 服务：

- 端点：`http://127.0.0.1:13337/mcp`（**必须带 `/mcp`**，否则返回 `-32098 Invalid endpoint`）
- 请求体：`{"jsonrpc":"2.0","id":1,"method":"<插件里的函数名>","params":{...}}`
- 方法名 = 插件文件里 `@jsonrpc` 装饰的函数名（`decompile_function`、`get_xrefs_to`、`list_functions`…）
- 封装好的脚本：`D:\Tools\ida_rpc.ps1`

```powershell
& 'D:\Tools\ida_rpc.ps1' -Method list_functions -ParamsJson '{"offset":0,"count":3}' -TimeoutSec 120 -IdaUrl 'http://127.0.0.1:13337/mcp'
```

### 16.7 给 mcp-plugin.py 追加的自定义方法（本机扩展）

在 `class MCP(idaapi.plugin_t):` 之前插入，重启 IDA 生效：

| 方法 | 参数 | 作用 |
|---|---|---|
| `create_function(address)` | 地址 | 在未识别地址建函数，返回 ok/start/end/name |
| `decompile_any(address)` | 地址 | **先建函数再反编译**，最常用 |
| `analysis_state()` | 无 | 返回 `auto_is_ok` 判断全量分析是否跑完 |
| `list_segments()` | 无 | 列出段名/起止/大小 |
| `find_bytes(pattern,start,end)` | `"1F 20 03 D5"` 形式 | 在范围内搜机器码，最多 50 条 |
| `get_bytes(address,length)` | 地址/长度 | 读原始字节（hex） |
| `stop_analysis()` | 无 | **无效**：`ida_auto.auto_cancel(ea1,ea2)` 只支持按范围取消，不能全局停 |

- **坑**：RPC 的 `params` 键必须和函数签名**完全一致**（含默认值的可选参数也要给），否则报 `-32602 Invalid params`。
- **坑**：`execute_sync` 的请求会排在自动分析后面，全量分析进行中每个调用可能等几十秒，超时就当没返回，别连着猛调。

### 16.8 已用 IDA 复核的事实（2026-11-21 21:xx）

- `CompoundTag::put 0x11BA0E40` 反编译结果：
  ```c
  v13 = Tree::find_or_create(a1 + 8, a2, &unk_2A13F00, &key, &out);
  Tag::operator=(v13 + 56, a3);
  return v13 + 56;
  ```
  → 与运行期实测「树在 `this+8`、节点内联 Tag 在 `+0x38`(=56)」**完全吻合**；`unk_2A13F00` 是空 key 的比较/查找辅助数据。
- `Tree::find_or_create 0x11BB2988`：4 参 `(tree, key_cstr, tag_type_ptr, out_ptr)`，返回节点。
- `ListTag::clone 0x11BA9388` 反编译全文已存 `D:\User\Desktop\我的世界ki\IDA定点反编译-CompoundTag.txt`。

### 16.9 全量分析太慢时的正确战术（**记住这条**）

- 342MB 的 so 全量 auto-analysis：实测 20+ 分钟 CPU 1000s+ 仍没走到 `0x11BA` 区域，`get_function_by_address` 一律 `No function found`。
- 不要等：直接 `decompile_any(地址)` 即时建函数+反编译，**已知地址就能出 C 代码**（已验证）。
- 需要 `get_xrefs_to` 时注意：代码区没被反汇编到就返回空列表（`Inventory 0x13D43160` / `PlayerInventory 0x13D43B78` 目前都是空 = 构造器还没被分析到），这不是虚表地址错。
- `ItemStack 0x13D96650` 的 xref 会返回几百条 data 引用（vtable 互相引用噪声），别当成构造器线索。

### 16.10 IDA 数据库落地位置（下次直接用，不用重新导入）

- 打开 `D:\User\Desktop\我的世界ki\libminecraftpe.so` 后，IDA 在**同目录**生成拆包数据库：
  `libminecraftpe.so.id0` / `.id1` / `.id2` / `.nam` / `.til`（id0+id1 合计约 2.7GB）。
- 下次直接双击/命令行打开同路径，IDA 会识别已有库，**不会重新全量分析**（但仍可能继续跑上次没跑完的分析）。
- 影子文件很多，别误删；要移植就把 `.so + .id* + .nam + .til` 一起拷。
- 本机已有 `D:\Tools\ida_rpc.ps1`（HTTP 直连）、`D:\Tools\ida_slice_decomp.py` / `idat_probe.py`（idat 批处理试验，尚未跑通，可弃）。

## 17. NBT JSON 导入 UI（代码已写+已编译，**待上机验证**）

- 源码位置：`jni/Mod/McMod.cpp`（新增 `ImpRefresh()` / `DrawNbtImportSection()`，插在 `DrawPanel()` 之前，调用点在「物品 NBT 编辑器（手持物品）」标题后的 Dummy 之后）。
- 组件：
  1. 路径输入框（默认 `/sdcard/nbt_test.json`）
  2. 「浏览文件夹」按钮 → 展开内置文件浏览器（`popen("ls -1p '<dir>' 2>/dev/null | head -n 200")`，目录带 `/` 后缀识别，首行「.. 上级目录」）
  3. 「导入到手持」按钮 → `FindInventoryNow()` + `NbImportJsonFile(g_impPath)`，结果绿/红文字提示，同时写 `[ui] import json ...` 日志
- 静态数组：`g_impName[200][128]` + `g_impIsDir[200]`（约 26KB），只在「浏览」或切目录时刷新，不会每帧 popen。
- 当前构建：`libs/arm64-v8a/libmcmod.so` 4,299,256 B，md5 `435475d5865656a5a411c446e07597e3`；已拷到服务器 `/opt/dist/libmcmod_impui.so`（部署时平板 `curl http://192.168.4.132:8898/libmcmod_impui.so`）。
- **未做/注意**：
  - 还没上机验证（要 force-stop 重启游戏才生效；IDA 全量分析跑完后一起验）。
  - 导入后旧 CompoundTag 指针故意不释放（连续导入会泄漏），已知。
  - 导入是“整体替换手持 NBT”，不复制物品本体；若目标格为空会失败（`NbImportJsonFile` 内部判定）。
  - `DrawPanel` 外层有 `if (!g_verOK) BeginDisabled(true)`，版本不支持时导入按钮也点不了（预期行为）。

## 18. 多核并行分析：切片法（2026-11-21 晚，已跑起来）

### 18.1 为什么要切片

- IDA 的 **auto-analysis 是单线程**：342MB 的 so 全量跑，实测 41 分钟 CPU 2493s（只吃满 1 个核），20 核机器上只占 ~10%，而且还没走到 `0x11BA` 区域 → **直接放弃全量**。
- 想要多核：拆成多片，每片独立建库并行分析，最后合并函数表。

### 18.2 关键：把代码区间包成“合法 AArch64 ELF”（不能直接丢 raw bin）

直接截成 `.bin` 丢给 IDA 时 processor 识别不对（`-T"Binary file"` + `-parm` 直接 exit 4、建不出函数）。绕法是自己造一个最小 ELF64：

- `ET_DYN`、`EM_AARCH64`、`e_entry=vaddr`、1 个 `PT_LOAD`（R+X，`p_offset=0x1000`、`p_vaddr=vaddr`、size）
- 字节从原 so 的**文件偏移 = vaddr** 处 `seek` 直读（本库 ELF 里 vaddr 与 file offset 相等）
- 脚本：`D:\Tools\mkelf_slice.py`

### 18.3 实际参数（8 片，已生成并验证能跑）

- `.text`: vaddr `0x06834C80`，size `215608648`(=0xCDA0E48)，末尾 `0x135D39C8`
- 每片 `0x19C0000`（26.2MB），共 8 片（第 8 片截到 `0x195ED48`）
- 产物：`D:\Tools\slices\mc_text_0..7.elf`（每片 27,004,928 B）

### 18.4 批处理脚本与启动方式

`D:\Tools\ida_slice_fnmap.py`（`-A` 下的 IDAPython）：`ida_auto.auto_wait()` 后遍历 `idautils.Functions()`，写 CSV（`start,size,name`，末尾 `# total=N`），最后 `idaapi.qexit(0)`（**故意不存库**，省 2GB/片磁盘）。

```powershell
$env:SLICE_OUT = "D:\Tools\slices\fnmap_$i.csv"
Start-Process 'C:\Program Files\IDA Professional 9.0\idat.exe' `
  -ArgumentList '-A','-SD:\Tools\ida_slice_fnmap.py',"D:\Tools\slices\mc_text_$i.elf" `
  -WindowStyle Hidden
```

- 实测：8 个 idat 进程，每进程 ~1 核，**整机 CPU 从 10% 拉到 57%**（20 逻辑核≈11 核在忙，IDA 自身还有 worker 线程），每进程工作集 ~300MB。
- 32GB 内存开 8 片很安全（15GB 空闲）。

### 18.5 代价 / 注意

- **跨片 xref 会断**：只适合“地址→函数名/边界”这个批量映射；要完整 xref 图得单独跑小范围。
- 片子切在函数中间会多出假函数，合并时按地址去重。
- 不存库 → 想反编译某片要重新导入并重新分析几分钟；需要反复反编译的片建议 `-o` 另存库。
- 片子太多（>12）收益递减：固定开销与边界损失上升。

### 18.6 切片并行最终结果（2026-11-21 晚，8/8 完成）

- 总耗时约 1 小时 10 分（8 片并行；第 0 片最慢，吃了 ~4156s CPU ≈ 单核 69 分钟；其余片 20–35 分钟）。
- 每片函数数：`0:56070  1:86133  2:99825  3:72794  4:72065  5:85711  6:68059  7:55382`
- **合并去重后 596,039 个函数**，文件：`D:\User\Desktop\我的世界ki\mc_functions_ida.csv`（14.67 MB，`start,size,name`，地址=ELF vaddr，首行带 `#` 注释）。
- 锚点核对：`11ba0e40 size=0x7c`、`11bb2988 size=0x184` 对得上；`11ba9388` 只报了 `size=4`（切片边界/重复起点造成的残影，**不要采信**）。
- 区间统计：`0x11B00000–0x11C00000` 共 2176 个函数。
- **噪声警告**：切片里没有重定位信息，IDA 会把数据当代码，出现超巨“函数”，例如 `sub_976BF88` size `0x870304`、`sub_D0658D4` `0x498496`。→ 这张表只适合“按地址查边界/名字”，**反编译仍要回 GUI 整库（`ida_rpc.ps1`）**。
- `operator new 0x135D3F30` 不在这张表里：`.text` 到 `0x135D39C8` 就结束，它在别的段（如 `.text.unlikely`）；需要时单独切一段。
