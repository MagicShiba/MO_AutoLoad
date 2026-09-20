# 自动装车 AutoLoad — 使用说明

红警2尤里复仇(心灵终结/CnCNet 环境)的两个本地辅助功能:

1. **自动装车**:战斗中框选步兵/载具后按热键(默认 **Ctrl+D**,可在
   AutoLoad.ini 里自定义),选中的单位会自动、按类型均衡地进入选中的载具
   (支持步兵上车,也支持 FV/AMC 这类载具开进 LCRF/SAPC 运输船)。
2. **同类变体选择**:选中一辆 IFV 类载具后按热键(默认 **Ctrl+T**,可在
   AutoLoad.ini 里自定义),按武器实际值加选同变体 —— 原生T键按注册名会把
   同名不同武器 (如空车/机枪车/导弹车都是 [FV]) 一起选中, 本功能只加选与
   当前选中相同 (类型, 实际武器) 的单位。按一次=同屏, 500ms内按第二次=全图。

## 文件与安装

游戏目录(G:\motest)里**只有一个文件** `AutoLoad.dll`,删掉即卸载。
钩子声明内嵌在 DLL 的 PE 段里(Syringe 自动识别,与 Ares 系 DLL 同机制),
**不再需要** `AutoLoad.dll.inj` —— 如果目录里还有旧版遗留的
`AutoLoad.dll.inj`,请删掉,否则钩子会被重复声明。

开发/研究资料全部在 `AutoLoadDev\` 子目录(源码、逆向笔记、工具、测试脚本),不影响游戏。

## 使用方法

1. 正常从客户端启动游戏(联机或遭遇战均可)。
2. 框选若干步兵 + 若干载具(和平时手动操作完全一致)。
3. 按 **Ctrl+D**。
4. 步兵会像被你逐个左键载具那样跑过去上车。

变体选择用法: 先选中一辆 (或几辆) IFV 类载具 (如 [FV]/[AMC]/[TRACTOR]/[STING]),
按 **Ctrl+T** 加选同屏同变体; 紧接着 (500ms内) 再按一次 **Ctrl+T** 则把
全图同变体都加选进来。非 IFV 逻辑 (Gunner=no) 的单位退化为纯类型加选,
与原生T键一致。

### 行为细节

- **只处理选中的单位**:选中外的载具绝不会被进入;装不下的多余单位原地不动。
- **步兵 + 载具都能当乘客**:选中的 FV/AMC 等载具也可以开进选中的 LCRF/SAPC
  运输船。载具占的座位数 = 它的 Size(和游戏原生规则一致,如 Size=3 占 3 格)。
  优先安排步兵;一辆载具本轮里当了运输工具就不会再作为乘客。
- **均衡分配**:各单位类型轮流派成员上车,人数少的类型不会被淹没
  (例:3 大兵 + 12 磁暴步兵争 3 辆×4 座 → 3 大兵全上 + 9 个最近的磁暴,
  落选的是最远的 3 个磁暴;而不会像以前那样磁暴吃光所有座位)。
  每辆车优先接"还没有的同类型"(同类强制铺开,例:5 反转士 + 5 光棱 +
  10 守卫大兵进 5 辆×4 座战斗要塞 → 每辆 1 反转士 + 1 光棱 + 2 守卫大兵,
  不会出现一辆车进 2 个反转士)。
  挑满同类后按"已分配最少"补齐(保证均衡),平局给距离最近的
  (例:2 大兵 + 6 守卫大兵 + 2 空战斗要塞 → 每个要塞 1 大兵 + 3 守卫大兵,
  且不会绕开身边空车去远处)。同类型内离车近的先上车。
- **容量严格**:占位按引擎原生规则(Size 之和)计算,只按剩余座位下发指令,绝不多发
  (例:10 步兵 + 2 个 1 座载具 → 只有 2 人上车,其余原地不动)。
- **装载等级 (SizeLimit)**:与引擎同判据(乘客 Size ≤ 运输方 SizeLimit)。
  坦克等载具只会进运输船(LCRF/SAPC,SizeLimit=12),不会再试图钻进
  战斗要塞/IFV 这类只装步兵(SizeLimit=1)的车。
- **跳过飞行单位**:火箭飞行兵(JUMPJET)等飞行运动模式的单位不参与装车,自动跳过。
- **跳过 NoManualUnload 载具(仅作为运输方)**:清道夫(TRACTOR)等内部载员不可释放、
  也不可被进入的特殊载具(规则标志 NoManualUnload=yes)不会再被当作上车的目标 ——
  不会再出现清道夫互相尝试钻进对方的情况;但清道夫本身仍可作为乘客,
  正常开进选中的 LCRF/SAPC 等运输船。
- **自定义热键**:DLL 同目录放一个 `AutoLoad.ini`(见 dist/ 里的样例):
  ```ini
  [AutoLoad]
  Hotkey=Alt+D
  VariantHotkey=Alt+T
  ```
  支持 Ctrl/Alt/Shift/Win 组合 + A-Z/0-9/F1-F24/方向键等;文件不存在时为默认
  (装车 Ctrl+D + 变体选择 Ctrl+T);设 `Hotkey=None` / `VariantHotkey=None`
  可分别禁用。
- **同类变体选择 (Ctrl+T)**:IFV 逻辑载具 (Gunner=yes,如 FV/AMC/TRACTOR/STING)
  按 (类型, 当前普通武器实际指针) 归类加选 —— Weapon2 与 Weapon10 指向同一
  实际武器即视为同一种; 只用普通 Weapon, 忽略升级后的 EliteWeapon (精英单位
  与普通单位同载员即视为同变体); 空车 (CurrentWeaponNumber=0, 即 Weapon1)
  自成一组。按一次只扫同屏 (Tactical), 500ms内按第二次扫全图 (Techno Array),
  与原生T键的单/双击语义对齐; 只做加选不取消已选, 判定复用原生
  0x732580/0x7342C0, 下发复用 Select, 选择是纯本地UI状态, 联机安全。
- **范围**:只装 30 格以内的载具(屏幕内),太远的不派。
- **载具类型**:任何带载员容量的单位(IFV、战斗要塞、夜鹰、碉堡类建筑等);
  载具乘客只进载具类运输单位。

## 两个版本 (AutoLoadDev/dist/)

| 文件 | 用途 |
|---|---|
| `AutoLoad.dll` | 带日志版(写入 AutoLoad.log,排查问题用;游戏目录部署的就是它) |
| `AutoLoad_nolog.dll` | 无日志版(零文件 I/O;确认稳定后可替换游戏目录的同名文件) |

两个文件功能完全一致,都自带内嵌钩子声明(单文件,不需要 .inj),
直接改名/复制替换即可。

## 日志(排查用)

游戏目录下的 **`AutoLoad.log`**(每次启动追加,可随时删除):

- `handshake OK` — DLL 已被 Syringe 加载
- `poll thread started` — 热键监听已就绪
- `hotkey pressed: Ctrl+D` — 检测到装车热键(名称以实际配置为准)
- `variant hotkey pressed: Ctrl+T` — 检测到变体选择热键
- `config: hotkey = …` / `config: variant hotkey = …` — 启动时读取的热键配置
- `variant-select: refs=… scope=SCREEN/MAP` — 变体参照组合数与范围
- `ref0: type=AMC gunner=1 cur=3/56 wpn=00A1B2C3 pax=1` — 每个参照的类型名、
  当前武器序号/总数、武器指针、载员数（非 Gunner 则 cur=-1/wpn=0）
- `variant-select: groups=… scanned=… selectable=…` — DLL 视角下的全部分组数、
  扫描候选数、通过归属/可选中判定的数
- `group: type=STNK wpn=XXXXXXXX n=30 MATCHED` — 每个分组的类型名、武器指针、
  数量（MATCHED=命中本次参照被加选）
- `auto-load: selected count=… infantry=… techno=… flyingSkipped=…` — 选中内容(含跳过的飞行单位数)
- `noManualUnloadSkipped=…` — 被跳过的不可进出载具数
- `skip NoManualUnload unit…` / `transport NoManualUnload…` — 具体被跳过的对象
- `skip flying infantry: obj=…` — 被跳过的飞行步兵
- `capacity=… used=… budgetUsed=…` — 每个载具的容量、实际占用与本轮已派预算
- `queue enter: inf=XXXX -> trn=YYYY dist=…` — 每条下发的上车指令(含距离, lepton/256=格)
- `auto-load pass COMPLETE` — 本轮分配完成
- `auto-load pass ENDED, leftover infantry=…` — 有步兵没上车(范围内无空位), 本轮结束

如果按热键后日志里连 `hotkey pressed` 都没有 → 把 `syringe.log` 一起发来。
若有检测和 queue enter 但游戏里没反应 → 把 `AutoLoad.log` 完整内容发来。

## 联机安全性(为什么不会不同步)

DLL 不修改任何游戏逻辑,唯一做的事:在你按下热键时,通过引擎自己的
`QueueMegaMission`(0x646E90)往 `OutList`(0xA802C8)里写入**标准 MegaMission(Enter) 事件**。
这与玩家点击载具产生的事件**字节级同构**(真实点击路径 0x4D76C6:
ClickedMission(7, NULL, 载具, NULL) → Whom=步兵, Mission=7, Target=空,
Destination=TC(载具对象)),走游戏原有网络管线广播,所有玩家按帧执行同样的指令。
对没有装这个 DLL 的其他玩家完全透明 —— 指令是标准指令,同步由引擎保证。
