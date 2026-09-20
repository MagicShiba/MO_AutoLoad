# 自动装车 (AutoLoad) 研究笔记

目标:按 Ctrl+D 时,把**选中的步兵**按类型平均分配进**选中的可载员载具**(只处理选中的单位),
通过游戏自身的标准事件管线下发指令 → 天然联机同步安全。

游戏环境:G:\motest = 心灵终结 (Mental Omega) + CnCNet 客户端
- 引擎: gamemd.exe (尤里的复仇 1.001, dwTimestamp=0x3BDF544E, CRC=0x54CC0A13)
- 加载器: Syringe 0.7.3.0mo (调试器式注入器),Ares 3.0 已挂 1450 个钩子
- 启动链: MentalOmegaClient.exe → Syringe.exe → gamemd.exe -SPAWN -CD -SPEEDCONTROL -LOG

---

## 1. Syringe 0.7 DLL 加载机制 (已实测确认)

- `syringe.log`: Syringe 扫描游戏目录 DLL,识别带 `SyringeHandshake` 导出的 DLL
  ("Recognized DLL: Ares.dll" → 调用其导出函数 SyringeHandshake,返回 0 = 接受)
- **钩子地址元数据在同名 `.inj` 纯文本文件里**(Ares.dll.inj),格式:
  ```
  ; 注释
  533058 = CommandClassCallback_Register, 7
  ^十六进制地址 = 导出函数名, 原始指令字节数
  ```
- 钩子函数签名: `DWORD __cdecl fn(REGISTERS* R)`,REGISTERS 见 YRpp/Syringe.h
- 返回值 = 相对钩子地址的偏移: **0 = 恢复并执行原指令后继续**;返回 size = 跳过原指令
- cncnet5.dll 是 "shim"(只导出 `DllMain@12`),走另一条 import-patch 路径,与我们无关

**结论 (双文件方案, v1.6 起已被 §13 内嵌段方案取代, 此处留作回退参考):
   新 DLL 只需: ①导出 SyringeHandshake ②导出钩子函数 ③放 `<DLL全名>.inj` 文本文件**
   ⚠ 命名必须是 `AutoLoad.dll.inj`(逆向 Syringe 0x42B910 解析器确认: 拼接常量 ".inj" @0x43C430)。
   文件名错误 → 解析失败 → **DLL 被静默跳过, syringe.log 不提一字**(第一版踩坑)。
   解析器格式: ';' 行注释; 行= `HEXADDR = ExportName, HEXSIZE`; 钩子函数返回 0 = 执行原指令继续。

## 2. 键位

- KeyboardMD.ini / INI/KeyboardDefaults.ini: `DeployObject=68`(D)
- RA2 键位编码 = 虚拟码 + 修饰符(Ctrl=+256, Shift=+512,由 TeamSelect 48..57 / TeamAddSelect 304..313 / TeamCreate 560..569 推出)
- **Ctrl+D = 68+256 = 324,未被占用** ✓

## 3. 事件系统 (联机同步的核心,全部已逆向确认)

`EventClass` (pack(1), sizeof=111):
- +0 Type (EventType), +1 IsExecuted, +2 HouseIndex, +3..7 Frame(=全局 CurrentFrame 0xA8ED84), +7.. DataBuffer[104]
- **EventType::MegaMission = 0x4** —— 玩家点击下命令的标准事件
- DataBuffer 内 MEGAMISSION 布局:
  - +7  Whom (TargetClass, 受令单位)
  - +0xC Mission (u8; Mission::Enter=7, Move=2, Attack=1, ...)
  - +0xE Target (TargetClass, 命令目标=载具)
  - +0x13 Destination (TargetClass)
  - +0x18 Follow (TargetClass)
  - +0x1D IsPlanningEvent (bool)
- `TargetClass` = {int m_ID; uchar m_RTTI} pack(1) 5字节 —— **按 (RTTI,ID) 引用对象,跨机器可序列化,不是裸指针**
  - RTTI: Unit=1, Aircraft=2, Building=6, Infantry=15 (AbstractType 枚举)
  - 对象身上: [obj+0]=ID(dword), [obj+4]=RTTI(byte)
- `OutList` (QueueClass<EventClass,128>) @ 0xA802C8:
  - 0xA802C8 Count, 0xA802D0 WriteIndex, 0xA802D4 起为 128×111 环形数组
  - 每项旁边 0xA83A54[idx*4] 存 timeGetTime 戳
  - OutList 每帧经网络层广播 + 进入 DoList(0x8B41F8) 在所有机器按帧执行

**同步安全逻辑**:选中状态(SELECT)根本不在事件表里 —— 选择是纯本地 UI 状态;
每条 MegaMission 事件自带 Whom(受令单位),由发起方在按下热键那一刻解析好。
=> 我们构造的 Enter 事件和玩家右键载具产生的事件完全同构,走同一网络管线 → 不可能造成不同步。

## 4. 指令下发链 (逆向确认)

玩家点击→命令的官方链路:
```
MouseClass::Right_Click (本地 UI)
  → DisplayClass::Active_Click_With(action, object)   [本地, PR1453 钩的就是这里 0x4AE7B3]
     遍历选中列表: call [vtbl+0x1A4] = ObjectClickedAction(action, pTarget, false)
        → ... → TechnoClass::ClickedMission(mission, pTarget, TargetCell, NearestCell) @ 0x6FFBE0
             本地语音反馈分支: [vtbl+0x358](Enter) 等
             **无条件走 0x646E90 = QueueMegaMission** (计划模式另有 IsPlanningEvent=1 的路径)
```

`QueueMegaMission` @ **0x646E90** (被 ClickedMission 调用, ret 0xC, MSVC fastcall):
```
ECX = mission (int, Enter=7)
EDX = TargetClass* pTarget   (指向命令目标: 载具)
栈+4  = TargetClass whom byval (8字节槽: ID dword + RTTI byte)  ← 受令单位(步兵)
栈+C  = TargetClass* pDest    (指向 Destination 的 TargetClass, 引擎传 TargetCell 的 TC)
行为: 构造 111 字节 EventClass{Type=4, HouseIndex=CurrentPlayer->ArrayIndex,
      Frame=CurrentFrame, Whom, Mission, Target=*pTarget, Destination=*pDest,
      Follow={Whom.ID, RTTI=0}}, 直接写入 OutList 环形队列 (满了则丢弃, al=0)
```
- 事件构造器 (MegaMission) @ 0x4C6860: 只写上述 11 字节,其余**不初始化**(引擎自己的脏习惯);
  我们的实现会把 111 字节先清零再填 —— 更严格,发送的是确定字节
- OutList.Add @ 0x6521C0 (stdcall, 事件按值传栈上112字节): 填 Frame=CurrentFrame → 追加环形队列 → timeGetTime 戳
- 执行端 (所有机器): EventClass::Execute → MegaMission 分支 → 解析 Whom → 单位进入 Mission::Enter,
  TarCom=Target → 步兵走向载具并进入。**这是联机中每右键一次就在跑的路径,构造性证明其确定性**

## 5. 选中单位列表 (PR1453 钩子处 0x4AE7B3 的真实代码确认)

- `ObjectClass** items = *(ObjectClass***)0xA8ECBC`
- `int count = *(int*)0xA8ECC8`
(相对 Map 0xA8B230 偏移 0x1A8C / 0x1A98,DynamicVectorClass)

## 6. Reference PR1453 (Distribution click)

- 与装车无关,但模式可借鉴: 本地拦截点击 → 用引擎标准入口(ObjectClickedAction 等)下发 → 自动同步
- 新命令注册钩子 0x533066 (CommandClassCallback_Register)

## 7. AutoLoad 设计

- **AutoLoad.dll** (32位, Syringe 加载, 纯无侵入):
  - DllMain 起 GetAsyncKeyState 轮询线程 (仅前台=游戏窗口时响应 Ctrl+D, 置 volatile flag)
  - 唯一钩子: 每帧函数 0x55D360 → flag 置位时: 读选中列表 → 分类(步兵/载具) →
    均衡分配 → 每对 (乘客,载具) 调 QueueMegaMission(7, Target={0,0}, Whom=乘客,
    Destination=TC(载具对象)) → 清 flag (事件布局依据见 §11)
  - 不修改任何游戏逻辑/数值/状态 → 不热键时零行为差异
- 分配算法: 按步兵类型分组,每种类型轮流分给剩余容量最大者(平局取已装最少→数组序),
  2gi+6ggi+2战斗要塞(各空10) → 每堡 1gi+3ggi ✓ 多余步兵不动 ✓
- 字段偏移(TechnoClass::Type / Passengers.NumPassengers / TechnoTypeClass::Passengers):
  用 mingw 编译 YRpp 头取 offsetof,再用 YRpp 自带 static_assert 校验布局

## 8. 字段偏移 (YRpp+mingw 探针实测, static_assert 全过)

- AbstractClass::UniqueID = 0x10 (4个COM接口vfptr后)
- ObjectClass::Location = 0x9C
- TechnoClass::Passengers = 0x114 (NumPassengers 在 +0)
- InfantryClass::Type = 0x6C0 / UnitClass::Type = 0x6C4 / BuildingClass::Type = 0x520 / AircraftClass::Type = 0x6C4
- TechnoTypeClass::Passengers(容量) = 0x5E0
- 对象身份: 主虚表指针 [obj+0] == 类vtable地址 (Infantry=0x7EB058 等) → 免虚调用分类
- TargetClass(对象) = {ID=Fetch_ID(), RTTI=0x34}; TargetClass(格) = {X+1000*Y, 0xB}

## 9. 状态 (v2.0)

- [x] 帧钩子 0x55D360, 单文件内嵌 .syhks00 声明 (免 .inj, 见 §13; 不校验 exeCRC, 见 §15)
- [x] 基础装车 + 类型均衡 + 严格容量 + 距离优先 — 用户实测通过 (§11)
- [x] 飞行单位跳过 (类型级 Locomotor GUID, §12)
- [x] 载具->运输载具 (SizeLimit 判据, §12/§14)
- [x] NoManualUnload 载具双向跳过 (Ares ExtData, §16.1)
- [x] 热键可配置 AutoLoad.ini, 默认 Ctrl+D (§16.2)
- [ ] 用户对 v1.7 SizeLimit / v1.9 无 CRC / v2.0 新特性的回归测试

## 10. 执行端深度逆向 (事件消费链)

事件消费链: DoList(0x64C380) → Type==4 转存 MegaMissionList(0xA83ED0, 256容量) →
Execute_DoList 尾部逐条 `call 0x4C6CB0`(ecx=事件) 执行。

0x4C6CB0 按 Type 跳转(表@0x4C8114), Type4/5 → 0x4C71CA:
1. Whom = (事件+7).As_Techno(0x6E6F20), 校验存活/血量
2. IsPlanningEvent(事件+0x1D)==1 → 走计划模式路径(0x637E00)
3. Target(事件+0xE/0x12) RTTI!=0 时 As_Object(0x6E6FF0) 并校验(死亡/0血 → 整个事件作废)
4. Destination(事件+0x13/0x17) 同样解析校验
5. `vtbl+0x4A4(事件)` = FootClass::ReceiveMegaMission(0x4DF0E0):
   - Mission==0x1D(AttackMove) 有专用暂存逻辑([0x5C4]/[0x5C8]/[0x5CC])
   - 其他任务(含 Enter=7)只做 vtbl+0x4A8() = 清 AttackMove 暂存, 返回 mission
6. `vtbl+0x1E8(mission, 0)` = QueueMission(0x5B35E0): 写 [unit+0xB4]=7
7. `0x70C610(unit, 0)` = 清 [unit+0x218](Archive/进入目标槽) + `0x4DA1C0` = Locomotion停止
8. `vtbl+0x3C8(Target.As_Abstract())` = **SetTarCom** (0x51B1F0)
9. `vtbl+0x480(Destination.As_Abstract(), 1)` = SetDestination(0x51AA40)
   ⚠ 内部对 Enter 任务有专用分支: `0x40DD70(dest)` 只认 Unit/Aircraft/Building/Infantry,
   **格子返回 NULL → 专用分支被跳过** → Archive 不会从格子设置
10. Follow(事件+0x18/0x1C) RTTI!=0 时 `vtbl+0x47C(Follow)`; 我们的 Follow={whomID,0} → 跳过

FootClass::Mission_Enter (vtbl+0x258 = 0x4DDF90, 槽位由 YRpp+mingw 探针编译求得):
- 状态机 [unit+0xBC]: 0=搜索, 1=走路
- 状态0: Archive[0x218]为空时自动填"自己所在格" → 围绕 Archive 坐标搜可进载具
  (vtbl+0x3C4 = 0x51E140) → 找到且距离≤阈值 → SetTarCom(找到的) → 上车
- 找不到 → NavCom=Destination 格子, 状态=1
- 状态1: [0x5A4]非0 继续走; ==0 → SetArchive(0)+QueueMission(Area_Guard)+NextMission → 放弃

**v1.1 失败根因**: 事件 Target={0,0} → SetTarCom(NULL), Archive 又因格子目标被跳过 →
单位只得到"走到该格"的指令(实际由 Area_Guard 执行走位), 到附近后永不搜索 → 放弃待机。
(v1.2 曾把载具放进 Target, 实测步兵反而攻击载具 —— TarCom 是攻击目标槽;
最终布局 Target={0,0} + Destination=TC(载具), 见 §11 地面真相。)

## 11. 真实点击路径地面真相 (v1.3 根因 + v1.4 优化依据)

### v1.3: 载具必须放 Destination 而不是 Target (用户实测 v1.2 = 步兵攻击载具)

Mission 枚举权威表: MissionControlClass::Names 表在 0x816CAC, GetName =
`[idx*4 + 0x816CAC]` (函数 0x5B3740 尾部):
0=Sleep, 1=Attack, 2=Move, 3=QMove, 4=Retreat, 5=Guard, 6=Sticky, **7=Enter**,
8=Capture, 11=Area Guard, 13=Stop, 16=Unload, **29(0x1D)="Attack Move"**。

真实点击链 (FootClass::Active_Click_With 0x4D74E0, 动作 switch 0x4D7CD0):
- 点到可进入的载具 → 分支 0x4D76C6:
  `0x40DD70(点击对象)` 确认可进入 → **ClickedMission(7, pTarget=NULL, TargetCell=载具对象, NearestCell=NULL)**
  (vtbl+0x378, 栈序: push 0, push 载具, push 0, push 7)
- 点到建筑(驻扎) → 分支 0x4D7597: ClickedMission(0x11, 点击对象, 0, 0) — 0x11=驻扎/破坏任务

ClickedMission(0x6FFBE0) 事件构造 (ctor 0x4C6860, 栈参: house, src, mission, target, dest, follow):
- target  = TC(arg2 = pTarget = **NULL**) → **{0,0}**
- dest    = TC(arg3 = TargetCell 槽被塞了**载具对象**) → TC(载具, RTTI 0x34)
- follow  = TC(arg4 = NearestCell = NULL) → {0,0}
- IsPlanningEvent(事件+0x1D) 由 ctor 显式清 0

⇒ **真实 Enter 事件: Mission=7, Target={0,0}, Destination=TC(载具对象), Follow={0,0}**

**v1.2 失败根因**: 我们把载具放进了 Target → 执行器 SetTarCom(载具)。TarCom 槽
([unit+0x2B4], SetTarCom 0x51B1F0 里的比较槽) 是**攻击目标槽** → 步兵锁定载具,
Mission_Enter 又因 TarCom!=0 走 vtbl+0x53C(0x522340) 分支无法装车 → 最终放弃进
Area_Guard → Area_Guard 对着 TarCom(友方载具) 开火 = "攻击载具" 现象。
(用户提示 "Ctrl+左键=强制攻击" 与此完全吻合: TarCom+放任 = 强制攻击的表现。)

**v1.1 失败根因修正**: Destination=格子 TC → 执行器 SetDestination(格子) → 0x40DD70
对格子返回 NULL → 无进入目标 → 走到格子放弃。v1.3: Target={0,0} + Destination=TC(载具),
与真实点击字节级一致 → 用户实测装车成功。

另: 0x646E90 (QueueMegaMission) 会把事件 Follow 自动填成 {whomID, RTTI=0},
执行器因 RTTI==0 跳过 Follow → 无副作用。

### v1.4 优化

1. **跳过飞行步兵**: FootClass::Locomotor = [inf+0x5AC] (ILocomotion COM 指针,
   对象 +0 即主接口 vtable)。RTTI 挖出各运动器类 vtable (构造函数写入点确认主 vtable):
   Jumpjet 0x7ECE34 / Rocket 0x7F0BE8 / Fly 0x7E8AC0 / Hover 0x7EADC8 /
   DropPod 0x7E8344 (+各次接口)。命中即跳过 (步行 0x7F6AC4、传送 0x7F50CC 不受影响)。
   (⚠ v1.4 实测不生效, v1.5 改类型级 GUID 判定, 见 §12。)
2. **容量预算**: 载具的 used 计数 ([obj+0x114]) 要等步兵真钻进去才涨, 跨帧重读会超发
   ("10 步兵+2 个 1 座车 → 全部上车")。修复: pass 级预算表 g_asgTrn/g_asgCnt,
   freeSlots = cap - used - PassAssigned(obj), 队列成功即 AddAssigned。
3. **距离优先**: 挑选规则从 "剩余容量最大" 改为 [已分配最少(保均衡) → 距离最近 → 选中序]。
   平衡优先保证 2gi+6ggi+2BF → 每要塞 1gi+3ggi 不变; 平局给最近, 消除 "身边有空车却绕远"。
4. **结束条件**: events==0 且还有剩余步兵 → 视为 "范围内无空位", 结束 pass (原实现会
   每帧重试形成死循环); OutList 满(queueFull)仍保持 pass 下帧续传。
5. **1 帧延迟**: 热键触发后延迟 1 帧再下发 (用户建议), 避开按键当帧的其它输入。

## 12. v1.5: 飞行过滤修正 (GUID) + 载具装载具 + 双版本构建

### 飞行过滤为何 v1.4 失败 / v1.5 修正
v1.4 用 [inf+0x5AC] 的运行时 vtable 比对 —— 实测 (JUMPJET/ARMR/GYRO 仍上车) 不生效。
改为**类型级 Locomotor GUID**: TechnoTypeClass+0x34C (ini 解析代码 0x7123ED
`lea edi,[ebp+0x34c]` + StringToGUID 0x527920 存入确认), 16 字节。
rulesmo.ini 步兵统计 (151 种):
- {4A582744-9839-11d1-B709-00A024DDAFD1} ×135 = 地面步行 (E1/GGI/SUPR/...)
- {92612C46-F71F-11d1-AC9F-006008055BB5} ×7 = **全部飞行步兵** (JUMPJET/ARMR/GYRO/
  MOTHRA/WASP/URAGAN/LUNR, 均 ConsideredAircraft=yes)
- {4A582747-...} ×1 = CLEG (地面特例, 不过滤)
DLL 内比对 Rocket + Jumpjet({2BEA74E1-7CCA-11d3-BE14-00104B62A16C}) 两个 CLSID。

### 乘客占位规则 (引擎权威)
PassengersClass::GetTotalSize (0x473460, YRpp TechnoClass.h 声明):
已占槽数 = Σ (乘客 Type 的 double @+0x380), 每加一个 FTOL 截断一次。
[type+0x380] = ini 的 `Size=` (double; 与载员 pip 显示代码 0x709EA0 交叉印证;
YRpp 头文件把 Size 排在 0x37C, 与二进制差 4, 以二进制为准)。
⇒ 步兵占 1 格 (Size=1); FV/AMC Size=3; LCRF/SAPC 自身 Size=15。
容量 = Type->Passengers ([type+0x5E0], int): AMC/FV=1, LCRF/SAPC=12。
DLL 不再读 [trn+0x114] (那是乘客个数不是槽数), 直接 thiscall 0x473460
(ecx=trn+0x114) 取已占槽数。

### 载具 -> 运输载具
选中列表里的 Unit 也作为乘客候选 (cost=Size), 收集顺序步兵在前 (分组轮转时步兵优先)。
约束: 载具乘客只进 UNIT 型运输载具 (不进建筑/飞行器); 本轮已作为运输工具用掉的
载具 (预算>0) 不再作为乘客; 飞行载具跳过。
真实点击依据: UnitClass::Active_Click_With 的 Enter 分支 (0x4D8171) 同样
ClickedMission(7, NULL, 格子, 载具) → Mission=7。⚠ 注意其 TargetCell/NearestCell
槽位与步兵版 (0x4D76C6) 不同 (格子+Follow=载具 vs 载具+无Follow);
v1.5 对两种乘客统一用已验证的步兵布局 (Destination=TC(载具对象)),
若载具实测出现"走到不进"再补 Follow 布局。

### 双版本构建 (AutoLoadDev/dist/)
- AutoLoad.dll = 带日志 (部署到游戏目录)
- AutoLoad_nolog.dll = -DAUTOLOAD_NOLOG, 无文件 I/O
构建命令 (tools/mingw/mingw32/bin 加入 PATH 后):
  gcc -O2 -shared -o AutoLoad.dll ../src/AutoLoad.c -nostdlib -lkernel32 -luser32
      -lgdi32 -lgcc -Wl,--enable-stdcall-fixup -Wl,--entry,DllMain
  (nolog 加 -DAUTOLOAD_NOLOG; -lgcc 必须: 大栈帧触发 __chkstk_ms)

## 13. v1.6: 单文件加载 (内嵌 .syhks00 段, 免 .inj)

用户放入的 dll 无 .inj 也能被 Syringe 加载 → 解析其 PE 得到格式:
- PE 段名 `.syhks00` —— 恰好 8 字符 (flags: INITIALIZED_DATA|READ|WRITE), 16 字节一条:
  ⚠ 首测失败原因: 误用段名 `.syhks` (dll 段名实为 .syhks00, 之前解析时
  误把结尾的 0 当填充剥掉)。Syringe 按 8 字节段名精确匹配 ([0x43c374]=".syhks00")。
  { unsigned hookAddr; unsigned hookSize; const char* hookName; unsigned pad }
- 名称指针是绝对 VA, 需要重定位项 (GCC 自带 .reloc 覆盖 ✓)
- 无 .syexe00 (宿主校验), 无 SyringeHandshake 导出 → 二者皆非必需
- Syringe 侧: 按名查段 (0x42ADF0, {char* name, len 8}) 后枚举条目挂钩

DLL v1.6 变更:
- `__attribute__((section(".syhks00"), used, aligned(16)))` 声明
  {0x55D360, 5, "AutoLoad_FrameHook", 0}
- 保留 SyringeHandshake 导出 (v1.9 起不校验 CRC, 仅写日志/提示信息)
- 帧钩子开头加宿主兜底校验: GetModuleHandleA("gamemd.exe") 为空直接返回
- **游戏目录不得再放 AutoLoad.dll.inj** (否则同一钩子声明两次)
- dist/ 只放 AutoLoad.dll 与 AutoLoad_nolog.dll 两个单文件

## 14. v1.7: 装载等级 SizeLimit (车辆准入的引擎判据)

用户实测: 非运输坦克(size 3)会尝试进入 FV/战斗要塞这类"载人载具"并被拒;
运输船(LCRF/SAPC)载坦克则正常。

定位: 任务/装载代码中对 GetTotalSize(0x473460) 的调用有 4 处, 其中 0x737xxx
簇是装载判定, **同一判据出现两遍** (0x73762B 与 0x737845):
1. used + passenger.Size <= Passengers(容量, type+0x5E0)   —— v1.5 已实现
2. passenger.Size <= *(double*)(运输方Type+0x388)          —— 即 ini `SizeLimit=`
   (fld Size; fcomp SizeLimit; 大于则跳 0x73780F 拒绝)

rulesmo.ini 实测数据佐证:
- LCRF/SAPC/YHVR/SEAT/TRACTOR: SizeLimit=12 → 可载载具(坦克 Size=3 ✓)
- FV/AMC/TENGU/STNK/APOC 等: SizeLimit=1 → 只载步兵(Size=1)
- BFRT(战斗要塞): Passengers=4, SizeLimit=2 → 只载步兵

DLL v1.7 变更 (src/AutoLoad.c):
- `#define OFF_TTYPE_SIZELIMIT 0x388`; TItem 增加 sizeLimit
- 收集运输工具时 SizeLimit<1.0 的直接跳过 (任何乘客都进不来)
- 乘客/运输配对时 `(double)cost > sizeLimit → 不选`, 严格复刻引擎浮点判据
- 乘客占位 cost 统一为 FTOL(Size), 最小 1 (步兵不再硬编码 1; Size<1 按保守 1 计)

## 15. v1.8→v1.9: exeCRC 校验从白名单到彻底取消 (兼容性排查)

白名单方案依赖 CRC 枚举: 若出现"引擎映像一致但文件信息
(头/签名/时间戳等)不同"的变体, 又会误拒。用户决定彻底取消 CRC 校验:
- 本 DLL 只被 Syringe.exe 按内嵌 .syhks00 钩子声明注入 gamemd.exe,
  不会被其它程序加载; 帧钩子另有 GetModuleHandleA("gamemd.exe")
  宿主兜底, 安全性不受影响。
- SyringeHandshake 现仅校验 pInfo 结构本身, 不再看 exeCRC。
- (v1.8 曾把 spzy 变体 CRC 0x1B499086 加入白名单, 两个 exe 四个段逐字节相同,
  仅文件头/附加数据不同 —— 证明 CRC 校验无意义后 v1.9 彻底取消。)

## 16. v2.0: NoManualUnload 过滤 + 热键可配置

### 16.1 NoManualUnload (Ares 规则标志)

用户报告: 清道夫 TRACTOR 等载具用内部载员逻辑实现特殊效果, 内部单位不会被释放,
也不能主动进入; 但框选多个 TRACTOR 时它们会互相尝试进入, 卡在一起。

排查:
- gamemd.exe 里没有 "NoManualUnload" 字符串 —— 这不是原生标志, 是 Ares 解析的
  (与 SizeLimit 等原生字段不同, 不能直接在 TechnoTypeClass 上找)。
- 三份游戏目录 (motest/MOx336/MOx336-spzy) 的 Ares.dll md5 完全一致, 偏移可固化。
- Ares.dll 逆向 (capstone):
  - 解析点: ExtData::LoadFromINI 0x1003EFEC push "NoManualUnload" ->
    `lea ecx,[ebx+0x4C5]` => **ExtData+0x4C5 = NoManualUnload (byte)**,
    SaveLoad 0x1003ACFF 处按 bool 存档, 互为印证。
  - 读取点: 0x10049E70 `type -> [type+0x2FC] -> cmp byte [ext+0x4C5], 0`
    (UnitClass GetActionOnObject 光标判定), 0x10048A90 同判据 =>
    **TechnoTypeClass+0x2FC 存 Ares ExtData 指针**。
  - 写入点: 0x10040E75 `mov [edi+0x2FC], esi` (esi 来自 ExtMap find/allocate,
    ecx=0x100C1CCC) —— 类型构造钩子为每个 TechnoType 都写, 全类型可用。
- 结论: DLL 侧 `ares.dll 已加载 && *(void**)(type+0x2FC) &&
  *(byte*)(ext+0x4C5)` 即判定。ares.dll 未加载时 +0x2FC 是普通引擎数据,
  不可信, 直接不过滤 (此时也不存在该规则标志)。
- DLL v2.0 变更 (用户修正后): IsNoManualUnloadType(); 只在收集运输方时跳过
  (TRACTOR 不被别人进), 乘客侧不过滤 —— NoManualUnload 只约束"作为运输方被进入/
  卸载", 不限制它自己当乘客 (Ares 光标判定也只查目标载具), TRACTOR 仍可正常
  进入 LCRF/SAPC。日志计数 noManualUnloadSkipped (运输方向)。

### 16.2 热键可配置 (AutoLoad.ini)

- 配置文件: **DLL 同目录** AutoLoad.ini, `[AutoLoad] Hotkey=Ctrl+D`
  (GetPrivateProfileStringA, 默认值 Ctrl+D; 文件缺失=默认)。
- 语法: `[Ctrl+][Alt+][Shift+][Win+]主键`, 主键支持 A-Z/0-9/F1-F24 与
  Space/Tab/Enter/Backspace/Insert/Delete/Home/End/PageUp/PageDown/方向键/小键盘;
  大小写不敏感; `None`/`Off` = 禁用热键; 非法值回退 Ctrl+D 并写日志。
- 实现: DllMain 记自身 hInst -> GetModuleFileNameA 定位同目录 -> 解析成
  {g_vkKey, g_mods 位集} -> 轮询线程 GetAsyncKeyState 按位集判定
  (替代原来硬编码的 Ctrl+D)。握手时也读一次, 供 Syringe 提示文本使用。
- 修饰键位集命名为 HKMOD_*: windows.h 的 MOD_* (RegisterHotKey 语义) 取值不同,
  不能复用。

## 17. v2.1: 同类变体选择 (Ctrl+T, 对T键加武器过滤)

需求: IFV逻辑载具 (FV/AMC/TRACTOR/STING 等 Gunner=yes) 随载员不同使用
Weapon1/Weapon2/... 中的不同实际武器, 但原生T键 (TypeSelect=84) 按类型ID
字符串全选, 会把同注册名下不同武器变体一起选中。加一个 Ctrl+T (可配置
VariantHotkey, 默认 Ctrl+T=340, 经 KeyboardMD.ini 确认未被占用):
按 (类型, 当前普通武器实际指针) 过滤加选; 单击=同屏, 500ms内双击=全图;
Weapon2与Weapon10指向同一实际武器视为同一种; 只用普通Weapon, 忽略
EliteWeapon; 非Gunner退化为纯类型选择。

### 17.1 原生T键逆向 (gamemd.exe)

- 入口有两条: 键盘命令分发 0x6D0833 (`cmp eax,[0xB0CB38]` 后 `call 0x732950`,
  [0xB0CB38] 即 TypeSelect 命令ID) 与 CommandClass 回调 0x5368AD
  (按 WWKey::Release=0x800 位选择调 0x732CA0(按下记时)/0x732CC0(抬起判500ms内调
  0x732950); WWKey: Shift=0x100/Ctrl=0x200/Alt=0x400/Release=0x800, YRpp 确认)。
- 0x732950 主流程: [0xB0FE54]==2 则跳过清零 (2=上次是TypeSelect, 见尾部
  0x732BF5 `mov [0xB0FE54],2`), 否则清 [0xB0FE64]=0; 调 0x732D10 从当前选中
  (0xA8ECBC/0xA8ECC8) 收集类型ID字符串表 (逐个调 vtbl+0x88 取TechnoType后
  +0x24 取ID, 字符串比较); [0xB0FE64]==0 走同屏分支 (Tactical 0x887324 经
  0x6DC420 取数/0x6DC430 取项, 基址 0xB0CEC8, 每项12字节+0=Object, 先过
  0x7342C0 `[ecx+0x14]&1` 存活关再过 0x732580 归属/可选中关), ==1 走全图分支
  (Techno Array 0xA8EC7C/0xA8EC88, 只过 0x732580); 两路都经 0x413A70 进临时表,
  再到 0x732B1F 用 0x732C30 (ecx=对象, edx=类型表, 内调 0x5F3E50->0x410A40
  `_stricmp(typeID)` 确认) 匹配后调 vtbl+0x14C=Select() 加选 (只加选不取消,
  故混合选中会扩展为全部相关类型); 尾部发声/提示 (0x734E60+0x7CA489,
  0x5D3BA0) 并置 [0xB0FE54]=2。
- 0x732580 (ecx=对象, 回AL): 归属/可选中判定 (读 [ecx+0x90]/[0xA8B238]/
  [ecx+0x21C]+0x1ED 等, 失败回0); 0x7342C0 (ecx=对象): `[ecx+0x14]&1`。
- 选择读取 0xA8ECBC/0xA8ECC8 即 §5 同地址 (0x4AE7B3 下令处同源);
  Techno Array 0xA8EC7C/0xA8EC88 = YRpp `TechnoClass::Array 0xA8EC78`
  的 Items(+4)/Count(+16), DynamicVector 布局以此互证
  (Items=+4, Count=+16; 同理 CurrentObjects 0xA8ECB8 的 Items/Count 恰为
  0xA8ECBC/0xA8ECC8 —— 与选择地址重合是 YRpp 地址与实测选择地址的字面重合,
  本DLL沿用实测有效的 §5 地址, 不依赖 YRpp 该条)。

### 17.2 IFV武器偏移 (全部逆向确认, 非YRpp编译)

- IFVMode@0x688: INI解析 0x714787 `push "IFVMode"` 后 `mov [ebp+0x688],eax`。
- Gunner@0x805: INI解析 0x714A50 `push "Gunner"` 后 `mov [ebp+0x805],al`;
  另有 6 处引擎读 (0x4DE648/0x4DE6E4/0x4DE72E/0x5180B5/0x709F61/0x735680 等)。
- TurretCount@0x808 / WeaponCount@0x80C: INI解析 0x712851/0x71286B
  `push "TurretCount"/"WeaponCount"` 确认。
- 普通Weapon数组基址@0x898 (GetWeapon 0x7177C0:
  `lea eax,[ecx+idx*28+0x898]` 确认, 每项 0x1C, WeaponType指针在+0;
  Elite数组在 0xA94 (GetEliteWeapon 0x7177E0 同构 `...+0xA94` 确认),
  中间隔 ClearAllWeapons; 武器加载循环 0x7128D6 `lea edi,[ebp+0xA94]`
  每次 `add edi,0x1C`, 先读 "Weapon%d" 存 [edi-0x1FC](=0x898基 normal)
  再读 "EliteWeapon%d" 存 [edi](=0xA94基 elite) 互证)。
  MO 的 FV/AMC 有 WeaponCount=56 (步兵 IFVMode 最大 55, 如 REPU=55),
  超出 YRpp 的 MaxWeapons=18 —— 普通数组首址 0x898 不变, 长数组向后延展
  (Elite基址相应后移, 但本功能不用Elite故无影响)。
- Techno::CurrentWeaponNumber@0x138: IFV换武器 0x70DC70
  `mov [esi+0x138],edi` 确认 (edi=IFVMode, 非法值<0/>=0x12 vanilla回落0;
  Ares 已 patch 以支持>18, 本DLL按 WeaponCount 钳制, 越界回落0即Weapon1);
  调用链 0x7464BB (Unit IFV更新: 取乘客TechnoType+0x688=IFVMode 后
  `push eax; call 0x70DC70`) 互证; GetTurretWeapon 0x70E1B6
  `mov eax,[esi+0x138]` 后调 vtbl+0x3F8=GetWeapon(0x70E140) 互证。
- 当前普通武器指针 = *(TechnoType+0x898+cur*0x1C) (+0即WeaponType*)。

### 17.3 DLL v2.1 变更 (src/AutoLoad.c)

- 配置: `[AutoLoad] VariantHotkey=Ctrl+T` (默认 Ctrl+T; 缺失=默认;
  语法与 Hotkey 共用 ParseHotkeyInto/MapKeyToken/StrEqNI, None=禁用,
  非法回落 Ctrl+T; 日志 `config: variant hotkey = …`)。
- 轮询: PollThread 同时看两组热键 (各自 prevDown 边沿, HotkeyDown() 按位集),
  分置 g_wantLoad / g_wantVariant。
- 帧钩: g_wantVariant 置位时用 GetTickCount() 判双击
  (s_lastVariantTick, 窗口 VARIANT_DBL_MS=500, 对齐原生 0x1F4),
  单击 wholeMap=0 (同屏), 双击 wholeMap=1 (全图), 调 DoVariantSelect()。
- DoVariantSelect(): ①从选中经 VariantKeyOf() 收集 (type,wpn) 去重参照
  (MAX_VARIANT_REFS=64; VariantKeyOf: 非Techno vtable/type=0跳过;
  Gunner==0 则wpn=NULL; 否则 cur=[obj+0x138], 越界回落0,
  wpn=*(type+0x898+cur*0x1C)); ②候选: 同屏读 Tactical
  ([0x887324]+0xDB0=数, 0xB0CEC8+i*12+0=obj, 先 CallAliveCheck(0x7342C0)
  再 CallTypeSelectCheck(0x732580)), 全图读 Techno Array
  (0xA8EC7C/0xA8EC88, 只过 0x732580); ③ (type,wpn) 命中则
  CallSelect (vtbl+0x14C, thiscall) 加选; 日志 refs/scope/scanned/added。
- 联机安全: 选择是纯本地UI状态 (Select 不发事件, 与T键一致), 不写 OutList,
  不引入自定义事件/状态; 装车路径 (QueueMegaMission) 未动。
- 版本: 握手提示改为 "AutoLoad 2.1: press … to load; … to select same IFV
  variant (T-filtered)."; dist/AutoLoad.ini 样例与 README 同步更新。
- 待用户回归测试: 空车组/同武器合并组 (如 CRM60×2/CRMP5×2)/精英同载员组/
  TRACTOR 多载员组/STING 双武器组 的单按同屏与双按全图。

## 18. v2.2: 变体分组错误的根因与修复 (Ares 扩展武器存储)

用户报告: AMC 不同武器大量坍成一种; STNK 只有 4 种实际武器却变出约 7 组,
且一组内含 3 种武器; 旧日志只有 refs/scanned/added, 无法定位。

根因 (tools/out/ares_hooks.txt 1450 条实证): Ares hook 了整条 IFV 武器链 —
`0x7128C0` (LoadFromINI_Weapons1)、`0x7177C0`/`0x7177E0`
(GetWeapon/GetEliteWeapon accessor)、`0x70DC70` (SwitchGunner)、`0x74642C`
(ReceiveGunner)、`0x7178B0` (GetWeaponTurretIndex)。
原生固定数组只够 18 项 (0x7177C0 lea 基址 0x898 / 0x7177E0 基址 0xA94,
间距 0x1FC = 18项 + ClearAllWeapons，可与 YRpp MaxWeapons=18 互证)，
而 MO 的 FV/AMC/STNK 有 WeaponCount=56 —— 18+ 槽位由 Ares 自有存储接管。
v2.1 直接按 `type+0x898+idx*0x1C` 计算，idx>=18 时读到 elite 残留/0/邻域：
大量读到 NULL/0 就坍成一组（含多种武器），读到各异邻域指针就变出幻影组
（4 种武器变 7 组）。+0x138 (CurrentWeaponNumber) 本身可信
（SwitchGunner 维护，未被 hook 的 GetTurretWeapon 0x70E1B6/0x70DD36 同样读它，
且游戏内开火表现正常），错的只是数组解析侧。

修复 (src/AutoLoad.c v2.2): VariantKeyOf 改调被 hook 的引擎 accessor
`0x7177C0` (ecx=类型, 栈参idx, ret 4 自清理, 返回 WeaponStruct*，+0 即
WeaponType*) 解析普通武器 —— 对 idx<18 与直接计算同值，对 18+ 走 Ares 扩展
存储；该 accessor 只给普通 Weapon（虚拟 GetWeapon 0x70E140 才是“精英则给
Elite”的分发），天然符合“忽略 EliteWeapon”的需求。wcount 钳制
（越界回落 Weapon1）与 +0x138 读取保持不变。

日志增强 (用户要求“日志简陋”问题的直接回应):
- 类型一律记名 (type+0x24, 0x410A40 确认, 定长 31 拷贝)：`refN: type=AMC
  gunner=1 cur=3/56 wpn=00A1B2C3 pax=1`（cur=当前武器序号/总数，pax=载员数，
  非 Gunner 则 cur=-1/wpn=0）。
- 新增候选直方图（DLL 视角下的全部分组）：`groups=… scanned=… selectable=…`
  后逐行 `group: type=STNK wpn=XXXXXXXX n=30 MATCHED`（MATCHED=命中本次参照）。
  排查时直接对照：组数是否等于实际武器数、同组 n 是否只含一种武器。
- 版本号升 2.2（握手文本同步），以便从日志区分构建。

待用户用带日志版复测并贴对应 `variant-select:` 段（AMC 单车单按/双按、
STNK 单车单按/双按各一次即可定位残余问题）。

## 19. v2.3: G:\Red Alert 2 (CnCNet 客户端) 不生效的双重原因

现象：同一份 DLL 在 G:\motest 生效，在 G:\Red Alert 2 无任何效果，
且游戏目录下连 `AutoLoad.log` 都没有（LogInit 在 handshake/帧钩都会建文件，
无文件 = DLL 从未被执行）。

原因一（主因）：SyringeEx 只加载 `-i=` 名单。G:\Red Alert 2\syringe.log：
`SyringeEx 0.1.0.2` 由客户端带 `-i=Ares.dll -i=CnCNet-Spawner.dll
-i=Phobos.dll gamemd-spawn.exe` 启动，`FindDLLs` 只找这三个 DLL
（`Done (2761 hooks added)`），AutoLoad.dll 根本没被加载。
而 G:\motest 用经典 Syringe 0.7.3.0mo、无 `-i` 参数，`FindDLLs()` 扫描全目录，
自动发现 `AutoLoad_nolog.dll` 并握手（syringe.log 实证）。
名单来源：`G:\Red Alert 2\Resources\ClientDefinitions.ini` 第 21 行
`ExtraCommandLineParams=-i=Ares.dll -i=CnCNet-Spawner.dll -i=Phobos.dll
gamemd-spawn.exe --args=...` —— 部署时须在此行追加 `-i=AutoLoad.dll`
（SyringeEx 0.1.0.2 基于 0.7.2.0；该目录无 Phobos.dll.inj 而 Phobos 正常加载，
证明内嵌 `.syhks00` 段机制可用，无需配 .inj）。
原因二（次因，即使加载了也会静默无行为）：帧钩宿主兜底只认
`GetModuleHandleA("gamemd.exe")`，而 CnCNet 启动的是 `gamemd-spawn.exe`
（进程模块名即此名），检查恒失败直接 return，轮询线程永不启动、无日志。
v2.3 改为两种名任一命中即工作。

同源验证（cmpexe.py 一次性脚本，已删）：gamemd-spawn.exe 与 gamemd.exe
基址同为 0x400000、SizeOfImage 同为 0x793000、时间戳同为 0x3BDF544E
（CRC 不同：0x098465B3 vs 0x54CC0A13，属重打包差异，见 §15 结论）；
12 处关键地址（帧钩 0x55D360、QueueMegaMission、T判定 0x732580/0x7342C0、
T主流程、GetWeapon、SwitchGunner、GetTotalSize、Unit vtbl 0x4D4/0x14C）
字节级全同 —— 硬编码地址无需改动，v2.3 只改宿主名判断并升版本号
（握手文本 2.3，便于从日志区分构建）。

## 20. v2.4: 就近装载 —— 组内按"最近可进载具距离"重排 (超员时近者先得)

症状：选中单位多于座位时，优先装的是地图靠上的单位而不是身边的；
远处先装、身边的不装。

根因：DoAutoLoad 第 5 步按组内链表顺序遍历乘客、先到先得占座，
链表顺序 = 选中数组顺序（实测表现为地图从上到下），与距离无关。
距离只参与"每个乘客选哪辆车"（已分配最少→最近），不参与"谁有座"。
所以超员时排在前面的远处单位先把座位占光。

修复（§7 设计的"均匀+就近"补完就近一半）：分组后、分配前加一步 4b，
组内按（到最近可进载具的距离平方，选中顺序）插入排序重排链表。
可进性判据与第 5 步同源：初始空位>=cost、cost<=SizeLimit、
载具乘客只进 Unit；动态余量仍由第 5 步把关，无可进载具者 key=INF
排最后（日志标 NO TRANSPORT）。平局裁决用选中顺序，与旧行为一致；
组间顺序与选车规则不动，均匀性不受影响。np<=128，插入排序开销可忽略；
无 CRT 可用（-nostdlib），手写排序而非 qsort。

日志：每组重排后逐成员打 `#序号 obj= d2cells=`（距离平方/格²，
单调同距离），可直接验证"近的排前面"。握手文本升 2.4。

## 21. v2.5: 均匀分配 —— 组间轮流 (人数少的类型不再被淹没)

症状：3 载具×4 座，3 动员兵 + 12 磁暴步兵，磁暴位置在上（选中顺序在前）时，
每个载具都分不到动员兵 —— 磁暴吃光 12 个座。

根因：DoAutoLoad 第 5 步 v2.4 及之前是整组串行（第 0 组从头分到尾，
再第 1 组……）。§7/NOTES 注释里写的"轮流"实际并未实现；
座位总数 < 乘客总数时，先出现的组垄断全部座位。v2.4 的组内就近只修了
同类型内的公平，跨类型仍是饿死。

修复：组间改为真·轮流 —— 每轮每组派一个成员（组内仍是 v2.4 就近顺序）
占座，取完为止。3GI+12 磁暴争 12 座 → 前 3 轮各上 1GI+1 磁暴，
GI 上完，剩下 6 座给最近的 6 磁暴，落选最远的 3 磁暴；
座位充足时的老例子（2gi+6ggi+2BF → 每堡 1gi+3ggi）结果不变。
同类型成员 cost 相同、可进集合相同，一旦某成员无车可进则整组判死
（余量只减不增，后面的同样进不去），余量耗尽组打
`group out of slots ...` 日志。终止性：每步或下发（events≤96 上限）
或杀组或队列满 goto，不会空转；跨帧续传仍靠 IsDone/预算记账，
与之前一致。握手文本升 2.5。

构建附带修复：GCC 把插入排序的移位习语优化成 `memcpy` 调用，
而本工程 `-nostdlib` 无 CRT 实现，链接期 `undefined reference to
memcpy`。两处着手：源码自带字节版 `memcpy`/`memset` 兜底，
构建加 `-ffreestanding` 让 GCC 不再主动生成此类库调用；
`-Wextra` 下零警告（顺手清掉 `ParseHotkeyInto` 的未用参数警告）。

## 22. v2.6: 均匀分配 —— 选车加"同类型已分最少"第一主键 (同类强制铺开)

症状：5 战斗要塞×4 座，5 反转士 + 5 光棱攻城兵 + 10 守卫大兵，
合理是每要塞 1+1+2，实际基本乱分配（某要塞进 2 反转士）。

根因：v2.5 的选车规则是 [总已分配最少 → 最近]，只看车上总人数。
走一遍：第 3 轮时各车总数 F1=2、其余 1，第 3 个反转士轮到它时 F1
总数并列最少、距离又最近 → 第二个反转士进了同一辆要塞。
跨轮次后，距离会把同类又吸回同一辆车；总数均衡只能保每车人数 ±1，
保不了每车的类型构成。

修复：选车加第一主键 [同类型已分最少 → 总已分配最少 → 最近 →
选中顺序]。每个成员永远先去"还没有同类的车"：5 反分 5 车必然
1 车 1 个（第 1 个反去最近，第 2 个去剩下里总数最少/最近……第 5 个
只能去最后一个 0 反的），光棱同理，守卫 2 车 1 个 —— 上例与地理
无关，必得每要塞 1+1+2。单类型场景同类型数恒差 ≤1，主键退化为
原来的总数规则，无回归；座位充足的老例子（2gi+6ggi+2BF）结果不变；
容量仍由空位/SizeLimit 硬把关。

实现：`g_typeAsg[128组][64车]` 全局表（32KB 静态区，不占栈），
与 `g_asgCnt` 预算同寿命（新 pass 起 `ClearTypeAsg()`，同 pass 跨帧
累加；组下标每帧按同一选中顺序重分组，保持稳定）。成功下发时
`g_typeAsg[g2][best]++`。

日志两处：`queue enter` 行加 `tSame=`（该车接完后同类数；
理想分配里反/光的 tSame 应全是 1，守卫是 1、2 交替）；
附带修了 `LogParts` 段数上限 6→10 的截断 bug —— 之前
`queue enter` 行的 `dist=` 和 `usable:` 行的后半从未打印出来过，
v2.4 让用户看 dist 验证是看不到的。握手文本升 2.6。
