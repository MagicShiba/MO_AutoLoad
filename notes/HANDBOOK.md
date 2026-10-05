# AutoLoad 手册（结论版）

> 只记"改代码 / 部署 / 排错时必须知道的结论"。过程性排查、已取代的旧方案、
> 版本断代史一律不记。实现以 `src/AutoLoad.c` 为准，本文件只解释"为什么这样写"。

源码布局：`src/AutoLoad.c`（单文件全部逻辑）、`dist/`（发布构建 + `AutoLoad.ini`
样例）、`tools/mingw`（构建链）、`tools/out/ares_hooks.txt`（Ares 钩子清单）。

---

## 1. 部署与加载（双环境，行为不同）

- DLL 通过 Syringe 挂到引擎每帧函数（`0x55D360`，5 字节钩子）上工作；
  钩子声明内嵌在 PE 段 `.syhks00`（段名必须 8 字节精确，16 字节一条
  `{hookAddr, hookSize, 导出名指针, 0}`），**不需要 `.inj` 文件**。
- `G:\motest`：经典 Syringe，无 `-i` 参数，扫描全目录自动发现 DLL。
- `G:\Red Alert 2`（CnCNet 客户端）：`SyringeEx` 只加载 `-i=` 名单里的 DLL，
  名单在 `Resources\ClientDefinitions.ini` 的 `ExtraCommandLineParams` 行，
  部署必须追加 `-i=AutoLoad.dll`，否则 DLL 一行代码都不会执行
  （表现为连 `AutoLoad.log` 都没有）。
- 同一客户端启动的是 `gamemd-spawn.exe` 而非 `gamemd.exe`，帧钩里的宿主兜底
  必须两种名都认。两者基址/大小/时间戳（`0x3BDF544E`）相同、关键地址字节级
  一致，硬编码地址通用；**不校验 exeCRC**（重打包变体 CRC 不同但映像一致）。

## 2. 联机安全与事件布局（核心约束）

- 装车走引擎标准事件管线：`QueueMegaMission (0x646E90, fastcall)`，
  `ECX=mission(7=Enter)`、`EDX=&TargetTC`、栈上 `Whom(byval 8字节)` + `&DestTC`，
  返回 AL=是否入队。事件经 `OutList (0xA802C8)` 广播，所有机器按帧执行。
- 事件里的单位引用是 `TargetClass {ID, RTTI}`（5 字节 pack(1)），不是裸指针，
  所以跨机器可序列化；构造的 Enter 事件与玩家右键载具产生的**字节级同构**，
  不可能造成不同步。满了则丢弃（返回 0），下帧续传。
- 真实 Enter 事件布局（对照 FootClass::Active_Click_With 点载具分支确认）：
  `Mission=7, Target={0,0}空, Destination=TC(载具对象), Follow={0,0}`。
  两条铁律，违反必现对应故障：
  - 载具**必须放 Destination**：放 Target 会进 `SetTarCom`（攻击目标槽），
    步兵开火打自家载具。
  - Destination **必须给载具对象**：给格子会被 `0x40DD70` 判 NULL，
    步兵走到格旁就放弃（Area_Guard）。
- 变体选择（Ctrl+T）只调本地 `Select` 改选中状态，不写 `OutList`，
  与原生 T 键一致，联机安全。

## 3. 地址 / 偏移速查

| 用途 | 地址 / 偏移 |
|---|---|
| 每帧钩子 | `0x55D360`（5 字节） |
| 下发事件 | `0x646E90` QueueMegaMission；`0x6E6AB0` 由对象取 TargetClass |
| 选中列表 | Items `0xA8ECBC`，Count `0xA8ECC8` |
| 对象坐标（lepton） | `[obj+0x9C]`（XY 各 4 字节，距离比较只用平面） |
| 身份判定 | `[obj+0]` 主虚表：步兵 `0x7EB058` / 载具 `0x7F5C70` / 建筑 `0x7E3EBC` / 飞行器 `0x7E22A4` |
| 类型指针 | 步兵 `[obj+0x6C0]` / 载具 `[obj+0x6C4]` / 建筑 `[obj+0x520]` / 飞行器 `[obj+0x6C4]` |
| 容量 / 已占 | 容量 `[type+0x5E0]`（int）；已占槽数调 `0x473460`（`ecx=trn+0x114`），= Σ乘客 `Size`（`[type+0x380]` double，逐个 FTOL），**不要读 `[trn+0x114]`（那是乘客个数）** |
| 车内存量枚举 | 链表头 `[trn+0x114+0x4]`（`FirstPassenger`），后继 `[pax+0x30]`（`NextObject`），上限 `[trn+0x114]`（`NumPassengers`）；布局来自 YRpp `PassengersClass`，`0x473460` 反汇编互证；逐个指针先验活再碰类型 |
| 装载等级 | `[type+0x388]` SizeLimit（double）；判据与引擎一致：`used+Size<=容量 且 Size<=SizeLimit`；`<1.0` 的载具谁也进不来 |
| T 键判定复用 | `0x732580`（归属/可选中）、`0x7342C0`（存活）；`Select` = `vtbl+0x14C` |
| 同屏候选 | Tactical 实例 `[0x887324]`，数量 `+0xDB0`，数组 `0xB0CEC8`（每项 12 字节，+0=对象） |
| 全图候选 | Techno Array Items `0xA8EC7C`，Count `0xA8EC88` |
| IFV 变体 | 类型 `Gunner+0x805`（byte）、`WeaponCount+0x80C`、ID 字符串 `+0x24`（仅日志）；对象当前武器序号 `[obj+0x138]` |
| 武器解析 | **必须调 accessor `0x7177C0`**（`ecx=类型, idx` 入栈，返回 WeaponStruct*，+0 即实际武器指针）。Ares 接管了 18+ 槽位存储，直接按 `type+0x898+idx*0x1C` 计算只对 `idx<18` 有效，更大会读到垃圾并分出幻影组。该 accessor 只给普通武器，天然忽略 Elite（符合需求） |
| Ares NoManualUnload / NoManualEnter | `ares.dll` 在才可信：`ext=*(type+0x2FC)`，`NoManualUnload=*(byte)(ext+0x4C5)`、`NoManualEnter=*(byte)(ext+0x4C7)`（中间隔 `0x4C6=NoManualFire`；Ares 读取点 `0x10049E70` / 光标判定 `0x74031A`；若 Ares 升级先复查此式） |

## 4. 装载算法（最终规则）

输入：当前选中里的步兵 + 地面载具（乘客）与有容量载具（运输方）。
口诀：**分组 → 组内就近排队 → 组间轮流 → 按四级规则选车**。

1. **收集**：乘客 = 非飞行步兵 + 非飞行地面载具（占位 `cost=FTOL(Size)`，
   保底 1）；运输方 = 容量 >0 且 `SizeLimit>=1` 的载具，空位
   `= 容量 − 已占 − 本轮已用预算`。预算表跨帧累加（载具的 used 计数要等
   乘客真钻进去才涨，不记账会超发）。
2. **范围过滤**：只留 30 格内（相对任一选中乘客）的运输方。
3. **分组**：按类型指针分组（首次出现顺序），类型内保持选中顺序。
4. **组内就近**：按（到最近可进载具的距离，选中顺序）重排，
   超员时近者先得；无可进载具者 key=INF 排最后。
5. **组间轮流 + 选车**：每轮每组派一个成员占座。选车四级：
   **同类型已分最少（含车内已有存量） → 总已分配最少（含车内已有） → 距离最近 → 选中顺序**。
   存量种子：规划前沿车内链表（`FirstPassenger` + `NextObject` 链，见 §3）统计每车
   已有分型/总数，作为比较初值（如某要塞已有 3B，2A+6B 进两车得每车 1A+3B，
   而非从 0 起算的 0A+4B / 2A+2B）。
   第一级保证同类铺开（均分优先，如 2+8 争两车必得每车 1+4）。
   规划模式随后做**组内就近换位**：每组每车几人定死不动，只把
   成员换到离自己近的车上（贪心取最短人车对），消除“两边站好人却对调跑
   远路”（位置合适时的换位跑）；位置不好时的跑远是必要的，保留。选满/`OutList` 满（每帧 ≤96 条）即停，满则下帧续传。
6. **整组判死**：同类型成员 cost 相同、可进集合相同，一旦某成员无车可进，
   整组退出（余量只减不增，后面的同样进不去），避免空转。
7. **结束**：全部办完 → 完成；有剩余但本帧零下发 → 范围内无空位，结束；
   队列满 → 保持，下帧继续（以上是 `Stagger=0` 的单帧下发行为）。
8. **先走后进**（`Stagger=1` 时启用，默认关闭）：分配一次算完记入规划表，发令
   分两路——车道内最近者首波直接拿 `Enter`（空槽直链，第一个上）；
   其余先拿 `Move`（目的地=载具所在格，`Target={0,0}` + `Destination=格子TC`，
   `ID=X+1000*Y`，`RTTI=11=Cell`），并行行军但不进链、不抢槽；
   载具每上一个人（`occ` 上涨）下一个才转 `Enter`。同一时刻最多一个未上车
   的 `Enter` 持有者，槽交接无竞争，上车顺序=车道距离序，总耗时≈最远行军
   + N×轮询。现任死亡立即越过；现任 300 帧无进展（任务丢失/载具被开走）
   也越过并写日志。`Stagger=0` 回到第 7 步的旧行为（同帧全下发，顺序随机）。
   用户接管（S/X/G/鼠标下令，认任务号+认方向，不认按键，重映射也有效；
   实测 S 落下来是 Guard，Phobos 全工程也无一处下发 Stop）：现任拿住 Enter
   后变 Stop/Guard/AreaGuard（单持槽下现任恒持槽，引擎只会对非持槽等待者改派
   Move），且无上车进展 → 整车道丢弃（载具自己在开时不误杀追车者）；拿住后
   变 Move 且相对规划距离跑远 6 格以上（X 散开/鼠标拉走，行军只会越走越近）
   → 同样丢弃；待发成员 Stop/被改派/另有 Enter 在身 → 跳过（行军中跑远的
   同样跳过，Guard 但距车 10 格外视为半路被停）。
   停掉的单位不再被复活；之后再按热键选中它们会当新单位规划。
   已放行名单（released 集）只在**全新**一轮（旧 session 已结束）开始时清空；
   顶替进行中的旧 session 时保留，但只跳过**还拿着旧 `Enter`**（任务号=7）
   的单位；任务已丢失（用户按 S / 手动改派 / 引擎清任务）的除名重规划，
   否则被停掉的单位会被永久跳过。

## 5. 过滤规则

- **飞行**：按**类型**上的 Locomotor GUID（`[type+0x34C]`，16 字节）判定，
  命中 Rocket（全部 MO 飞行步兵）或 Jumpjet（兼容原版）即跳过。
  不要用运行时运动器虚表（实测不可靠）。
- **NoManualUnload / NoManualEnter**（清道夫 TRACTOR / 监狱车 RAVA·CHRP 等）：
  **只在收集运输方时跳过**（不可被手动进入），它自己仍可当乘客进运输船
  （规则只约束被进入/卸载）。`NoManualEnter=yes` 单独出现的类型（RAVA/CHRP）
  同样被跳过。
- **载具乘客**：只进 Unit 型运输载具（不进建筑/飞行器）；本轮已当运输工具
  的载具不再当乘客。

## 6. 变体选择（Ctrl+T）

- 需求：Gunner 载具（FV/AMC/TRACTOR/STING 等）同注册名下有多种实际武器，
  原生 T 键全选会混在一起。按**（类型指针，当前普通武器实际指针）**
  过滤加选：`Weapon2` 与 `Weapon10` 指向同一实际武器视为同一种；
  非 Gunner 退化为纯类型选择。
- 参照 = 当前选中里的（类型，武器）组合去重；序号越界回落 0（即 Weapon1，
  与引擎一致）；单击同屏、500ms 内双击全图（对齐原生 T 键窗口）。

## 7. 构建

```bat
set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
gcc -O2 -Wall -Wextra -ffreestanding -shared -o dist/AutoLoad.dll src/AutoLoad.c ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--entry,DllMain
:: nolog 版加 -DAUTOLOAD_NOLOG（无任何文件 I/O，输出 dist/AutoLoad_nolog.dll）
```
（目录职责见根目录 `目录规范.md`：构建直出 `dist/`，调试中间物进 `build/`，
实验文件进 `test/`，三者不混用。）

- `-nostdlib` 无 CRT：源码自带字节版 `memcpy`/`memset`（GCC 会把数组搬移
  习语优化成库调用，没有自带实现就链接失败）；`-ffreestanding` 让 GCC
  不再主动生成此类调用；`-lgcc` 必需（大栈帧触发 `__chkstk_ms`）。
- 构建后必查：`objdump -h` 有 `.syhks00` 段，`objdump -p` 有
  `SyringeHandshake` + `AutoLoad_FrameHook` 两个导出。
- 构建产物同步到 `dist/`（`AutoLoad.dll` 带日志版先行验证，
  稳定后再换 nolog 版）。

## 8. 日志速查（带日志版，游戏目录 `AutoLoad.log`）

- `config: hotkey = … / variant hotkey = … / stagger = …`：实际生效键与分波
  配置（文件缺失即默认）。
- `released set cleared (fresh session): count=…`：全新一轮清空已放行名单
 （顶替旧 session 时不出现此行，名单保留）。
- `group N type=…` + `  #序号 obj=… d2cells=…`：组内就近顺序（距离平方/格²，
  `NO TRANSPORT` = 无可进载具）。
- `plan result: pairs planned=…`：规划对数。
- `reassign proximity moved=…`：组内换位人数；位置已站好时应为 0，
  `release wave` 的 `dist=` 不应出现万级。
- `queue enter: unit=… -> trn=… cost=… tSame=… dist=…`（`Stagger=0` 时）：
  下发记录；`tSame` = 该车接完后车上同类数（理想分配里小类型应全是 1）；
  `dist` = 与所选车的距离（`>>8`，单调）。
- `release wave=W unit=… -> trn=… tSame=… dist=…`（`Stagger=1` 时）：`Enter`
  下发记录（只有持槽顺序，`W` 即车道距离序）。
- `march wave=W unit=… -> cell=X,Y`：`Move` 行军下发记录。
  `lane advance: active dead / stall timeout`：现任死亡或 300 帧无进展越过。
- `group out of slots …`：某类型无车可进，整组剩余落选（正常超员现象）。
- `frame result: events queued=… units remaining=…` → `auto-load pass COMPLETE`
  / `ENDED, leftover units=… (no slots in range)`（`Stagger=0` 时）。
- `variant-select: refs=… scope=SCREEN/MAP` + `refN: type=… cur=a/b wpn=…` +
  `group: type=… wpn=… n=… MATCHED`：变体选择的参照与候选直方图
  （组数应等于实际武器数，`MATCHED` = 命中本次参照）。

## 9. 热键配置（DLL 同目录 `AutoLoad.ini`）

```ini
[AutoLoad]
Hotkey=Ctrl+D
VariantHotkey=Ctrl+T
Stagger=0
```

语法：`[Ctrl+][Alt+][Shift+][Win+]主键`，主键 A-Z/0-9/F1-F24/方向键等，
大小写不敏感；`None` 禁用；非法值回落默认并写日志。缺文件即默认值。
默认 `Stagger=0`（同帧全下发，顺序随机）；置 `Stagger=1` 启用分波放行。

## 10. 引擎上车顺序机制（结论）

- 引擎里**没有上车排队，但有链路独占**：`UnitClass::ReceiveCommand` 对
  `RequestLoading(14)`（`0x7377D8`，经 `0x737B54` 跳表确认）只做三件事——
  移动中（`Is_Moving` 且 `CurrentMission==Move(2)`）/超载/超 `SizeLimit`
  答 `Negative(0xA)`；与载具不在同一 movement zone
  （`0x56D230=MapClass::GetMovementZoneType`）答 `0xE`（继续等）；
  **其余全部答 `Positive(1)`**。
- 上车动作在乘客侧：`FootClass::Mission_Enter(0x4D9290)` 拿到 `Positive`
  且 locomotor 已到达 → 当场进舱；还在路上 → 继续走。
- `Mission_Enter` **不是每帧跑**：尾部（`0x4D946C`）返回下次执行的帧延迟
  `= F2I(Rate×900) + RandomRanged(0,2)`（`0x65C7E0`，RNG 是联机同步的
  `Scenario.Random`）。`Rate` 默认值 `0.016`（`MissionControl` 构造
  `0x5B3700` 全任务统一，`rulesmo.ini` 无 `[Enter]` 覆盖段）→
  **单轮询延迟 = F2I(14.4)+0~2 = 14~16 帧（约 1 秒）**。
- **链路是独占槽 + 等待者，交接只发生在上车瞬间，由 log 实测**（1 车 4 兵）：
  空槽时首个请求者直链（`RequestLink`，`tLink0` 立刻指向它）并跑 Enter；
  非持槽者被引擎改派 Move 靠站（任务号读数 7→2）、Move 走完转 Guard（2→5）
  原地等，全程无链；每次上车瞬间槽交接给一个等待者（持槽者到达后约一个
  轮询周期即上）。
- **交接给谁是轮询竞态**：五次交接实测里，顺序/距离/LIFO/FIFO 没有任何
  确定性规则能同时解释——槽给交接后第一个轮询到的等待者（各 14~16 帧独立
  相位 + 同步抖动），纯运气。
  推论：任何“先后下发 Enter”的办法（同帧、顺序分波、倒序分波）都控不住顺序。
- **唯一确定性做法**：交接瞬间只留一个等待者。`Enter` 只在 `occ`
  上涨（上一个人上车、槽空出）后才发给下一个；其余人拿 `Move` 并行走路
  （`Move` 不发 `RequestLoading`，不进链、不抢槽）。`Move` 事件布局与原生
  点地移动一致（`Destination=格子TC`）。现任死亡/300 帧超时则越过，保证
  不死锁。速度≈最远行军 + N×轮询（行军并行，只串行上车本身）。
- 日志指纹（认“上完”）：载具 `occ` 跳变即上车帧；上车不离 Techno Array。
- 联机安全：抖动用的是同步 RNG，各客户端看到的上车顺序逐帧一致；
  分波/倒序仍是单一下令者走广播命令。红线：不写模拟内存、不用非同步
  RNG/墙钟做模拟决策。
