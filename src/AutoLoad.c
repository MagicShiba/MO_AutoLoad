/*
 * AutoLoad.dll - 自动装车 (热键可配置, 默认 Ctrl+D)
 *              + 同类变体选择 (热键可配置, 默认 Ctrl+T)
 * 目标引擎: gamemd.exe (尤里的复仇 1.001), 由 Syringe 0.7.3mo 自动加载。
 *
 * 功能1 装车: 按下热键时, 把【选中的步兵和载具】按类型均衡分配进【选中的、
 *       范围内、有空位且装载等级达标】的可载员载具: 各类型轮流派成员占座
 *       (人数少的类型不会被淹没), 每辆车优先接"还没有的同类型"
 *       (同类强制铺开, 如每要塞 1反+1光+2守), 同类型内离载具近的优先
 *       (超员时远处的落选, 而不是选中顺序/地图从上到下在前者先得)。
 *       只处理选中单位; 装不下的
 *       多余单位不做处理。飞行运动模式的单位 (火箭飞行兵等) 自动跳过;
 *       NoManualUnload=yes 的载具 (清道夫 TRACTOR 等) 不可被进入, 仅作为运输方
 *       被跳过 —— 它本身仍可作为乘客进入 LCRF/SAPC 等正常运输载具; 容量与装载
 *       等级按引擎原生判据 (Size 之和 / SizeLimit) 严格记账; 支持 载具->运输载具。
 *       分波放行: 分配一次算完。先走后进 (HANDBOOK §10): 首波最近者直接拿
 *       Enter (空槽直链, 第一个上), 其余先 Move 行军到载具附近 (Move 不进链,
 *       多人并行不抢槽), 载具每上一个人 (occ 上涨) 下一个才转 Enter —— 同一时刻
 *       最多一个未上车的 Enter 持有者, 槽交接无竞争, 上车顺序=距离序, 总耗时≈
 *       最远行军+ N×轮询。现任死亡/任务丢失/超时 300 帧无进展则越过。
 *       Stagger=0 回到 legacy 行为 (同帧全下发, 顺序随机)。
 *
 * 功能2 变体选择: IFV逻辑载具 (FV/AMC/TRACTOR/STING 等 Gunner=yes) 随载员不同
 *       使用 Weapon1/Weapon2/... 中的不同实际武器, 但原生T键按注册名全选会把
 *       不同武器变体一起选中。按 VariantHotkey (默认 Ctrl+T) 则按 (类型,
 *       当前普通武器实际指针) 过滤加选 —— Weapon2与Weapon10指向同一实际武器
 *       即视为同一种; 只用普通Weapon, 忽略升级后的EliteWeapon; 非Gunner单位
 *       退化为纯类型选择。单击=同屏 (复用Tactical), 500ms内双击=全图 (复用
 *       Techno Array), 与原生T键的单/双击语义对齐。选择是纯本地UI状态
 *       (调 Select, 不发网络事件), 与T键一致, 联机安全。
 *       实现注意: 武器指针必须经引擎 accessor 0x7177C0 解析 (Ares 接管了
 *       18+ 槽位存储), 不得直接按 type+0x898+idx*0x1C 计算 (见 HANDBOOK §3)。
 *
 * 热键: 游戏目录旁的 AutoLoad.ini ([AutoLoad] Hotkey=Ctrl+D,
 *       VariantHotkey=Ctrl+T), 支持 Ctrl/Alt/Shift/Win 组合 与
 *       A-Z/0-9/F1-F24/方向键等。文件不存在时用默认值。
 *
 * 同步安全: 每一对 (步兵, 载具) 通过引擎自身的 QueueMegaMission (0x646E90) 下发一条
 *       标准 MegaMission(Mission::Enter) 事件 —— 与玩家点击载具产生的事件字节级同构
 *       (真实点击路径 0x4D76C6: ClickedMission(7, NULL, 载具, NULL) =>
 *        Whom=步兵, Mission=7, Target={0,0}空, Destination=TC(载具对象)),
 *       走 OutList 正常网络管线广播, 所有机器按帧执行, 不引入任何自定义事件/状态。
 *       分波放行同样安全: session 状态是按下者本机静态内存 (别机无 session 不跟进),
 *       间隔是固定帧数 (不用随机数), 校验读的是同步的 Techno Array, 下发仍是
 *       标准广播命令 —— 等价于玩家分几次手动点选, 锁步模型下天然同步。红线:
 *       不写单位/载具模拟内存, 不用非同步 RNG/墙钟做模拟决策, 本 DLL 均不碰。
 *
 * 日志: 游戏目录 AutoLoad.log (仅带日志版; nolog 版无任何文件 I/O)。
 * 结构/地址依据见 AutoLoadDev/notes/HANDBOOK.md。
 */

#include <windows.h>
#include <stdarg.h>

/* 无 CRT (-nostdlib): 数组搬移/清零习语会被 GCC 优化成 memcpy/memset
 * 调用而导致链接期缺符号 。自带字节实现兜底;
 * 构建再加 -ffreestanding 让 GCC 不再主动生成此类库调用。 */
static void* AL_memcpy(void* d, const void* s, unsigned n)
{
    unsigned char* p = (unsigned char*)d;
    const unsigned char* q = (const unsigned char*)s;
    while (n--) *p++ = *q++;
    return d;
}
void* memcpy(void* d, const void* s, unsigned n) { return AL_memcpy(d, s, n); }
void* memset(void* s, int c, unsigned n)
{
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

/* ---------------- 引擎常量 (逆向确认) ---------------- */

#define INFANTRY_VTABLE   0x007EB058u
#define UNIT_VTABLE       0x007F5C70u
#define BUILDING_VTABLE   0x007E3EBCu
#define AIRCRAFT_VTABLE   0x007E22A4u

#define OFF_LOCATION          0x9C   /* ObjectClass::Location (CoordStruct, lepton) */
#define OFF_PASSENGERS        0x114  /* TechnoClass::Passengers (PassengersClass, Num 在 +0) */
#define OFF_TYPE_INFANTRY     0x6C0  /* InfantryClass::Type */
#define OFF_TYPE_UNIT         0x6C4  /* UnitClass::Type */
#define OFF_TYPE_BUILDING     0x520  /* BuildingClass::Type */
#define OFF_TYPE_AIRCRAFT     0x6C4  /* AircraftClass::Type */
#define OFF_TTYPE_PASSENGERS  0x5E0  /* TechnoTypeClass::Passengers (容量, int) */
#define OFF_TTYPE_LOCOMOTOR   0x34C  /* TechnoTypeClass::Locomotor (_GUID 16字节, ini解析0x7123ED确认) */
#define OFF_TTYPE_SIZE        0x380  /* TechnoTypeClass::Size (double, 乘客占位=Size, GetTotalSize 0x473460 累加确认) */
#define OFF_TTYPE_SIZELIMIT   0x388  /* TechnoTypeClass::SizeLimit (double, 装载等级:
                                         车辆任务代码 0x737845/0x73762B 两处相同判定:
                                         used+Size<=Passengers 且 Size<=SizeLimit;
                                         LCRF/SAPC=12 可载车, FV/AMC=1 只载步兵) */
#define OFF_TTYPE_ARES_EXT    0x2FC  /* Ares: TechnoTypeExt::ExtData 指针
                                         (Ares 类型构造钩子写入; 仅 ares.dll 已加载时可信) */
#define OFF_ARES_EXT_NMU      0x4C5  /* Ares ExtData::NoManualUnload (byte; Ares 读取点
                                         0x10049E70: type+0x2FC -> +0x4C5 判定, 三份
                                         游戏目录的 Ares.dll 二进制一致) */
#define ADDR_GET_TOTAL_SIZE   0x00473460u /* PassengersClass::GetTotalSize (thiscall) 已占槽数 */
#define ADDR_CHECK_TYPESELECT 0x00732580u /* (thiscall, ecx=object) T选择用的归属/可选中判定, 返回AL */
#define ADDR_CHECK_ALIVE      0x007342C0u /* (ecx=object) [obj+0x14]&1, 返回AL (同屏候选先过此关) */
#define ADDR_GET_WEAPON       0x007177C0u /* TechnoTypeClass::GetWeapon(idx): 普通Weapon accessor
                                             (ecx=类型, 栈参idx, ret 4自清理, 返回WeaponStruct*;
                                              Ares hook点, 见 VariantKeyOf 注释) */
#define OFF_SELECT_VTBL       0x14Cu      /* ObjectClass::Select() 虚表偏移 (0x732B41 call [eax+0x14C] 确认) */

#define OFF_TECHNO_ITEMS      0xA8EC7Cu /* Techno Array Items (全图候选, 同 0x7329CD/0x732AF7 用法) */
#define OFF_TECHNO_COUNT      0xA8EC88u /* Techno Array Count */
#define OFF_TACTICAL_INST     0x887324u /* TacticalClass::Instance (同屏候选) */
#define OFF_TACTICAL_CNT      0xDB0     /* [Tactical+0xDB0] = 同屏可选数量 (0x6DC420 确认) */
#define OFF_TACTICAL_ARRAY    0xB0CEC8u /* TacticalSelectables[500], 每项12字节, +0=Object* */
#define SZ_TACTICAL_ENTRY     12

/* IFV 变体判定偏移 (全部逆向确认):
 * Gunner@0x805 (INI解析0x714A50确认), WeaponCount@0x80C
 * (INI解析0x71286B确认), Techno::CurrentWeaponNumber@0x138
 * (IFV换武器 0x70DC70: mov [esi+0x138],edi 确认; 空车/非法值回落0, 即Weapon1)。
 * 注意: 不得直接按 type+0x898+idx*0x1C 读普通Weapon数组 —— 原生固定数组只够
 * 18 项 (GetWeapon 0x7177C0 的 lea 基址 0x898 / GetElite 0x7177E0 基址 0xA94,
 * 间距 0x1FC=18项+ClearAllWeapons 可互证), 而 MO 的 FV/AMC/STNK 有 56 项;
 * Ares hook 了武器加载 (0x7128C0)、GetWeapon/GetEliteWeapon (0x7177C0/
 * 0x7177E0)、SwitchGunner (0x70DC70)、ReceiveGunner (0x74642C),
 * 18+ 槽位存放在 Ares 自有存储里。直接计算在 idx>=18 时读到 elite 残留/0/邻域
 * (STNK 4种武器变出7组、AMC 大量不同武器坍成一组的病根)。必须调被 hook
 * 的 0x7177C0 accessor 解析 (它同时只给普通Weapon, 精英单位也不返回Elite,
 * 正好符合“忽略EliteWeapon”的需求)。类型ID字符串在 type+0x24
 * (0x410A40: add ecx,0x24 后 _stricmp 确认, 仅用于日志)。 */
#define OFF_TTYPE_GUNNER      0x805
#define OFF_TTYPE_WEAPONCOUNT 0x80C
#define OFF_TTYPE_IDSTR       0x24
#define OFF_TECHNO_CURWEAPON  0x138
#define VARIANT_DBL_MS        500u /* Ctrl+T 双击窗口: 对齐原生T键的0x1F4=500ms */
#define MAX_VARIANT_REFS      64
#define MAX_VARIANT_HIST      128

#define OFF_SELECTION_ITEMS   0xA8ECBCu /* *(ObjectClass***) 选中单位指针数组 */
#define OFF_SELECTION_COUNT   0xA8ECC8u /* *(int*) 选中数量 */

#define ADDR_QUEUE_MEGA_MISSION  0x00646E90u /* (fastcall) mission, &TargetTC, WhomTC(byval), &DestTC */
#define ADDR_TC_FROM_OBJECT      0x006E6AB0u /* (thiscall) tc_out, object  -> {ID, 0x34} */
#define ADDR_HOOK_FRAME          0x0055D360u /* 每帧函数入口 (Phobos FrameStep_Begin 同款) */

#define MISSION_ENTER   7
#define MISSION_MOVE    2   /* 行军 (先走后进, Destination=格子TC) */
#define RTTI_CELL       11  /* TargetClass RTTI: 格子 (ID = X+1000*Y, 见 YRpp TargetClass.h) */
#define MAX_PASSENGERS  128
#define MAX_TRANSPORTS  64
#define MAX_EVENTS_PER_FRAME 96
#define RANGE_LEPTONS   (30 * 256)  /* "附近"半径: 30 格 (与任一选中乘客的平面距离) */

/* Session watchdog: a normal session ends in dozens of frames;
 * SESS_MAX_FRAMES is a pure backstop. Boarding latency itself comes
 * from the engine (Mission_Enter polls every 14-16 frames, HANDBOOK). */
#define SESS_MAX_FRAMES 900
/* Stall timeout: a released Enter holder making no progress (dead,
 * mission lost, or carrier driven away) is skipped after N frames. */
#define STALL_MAX_FRAMES 300

/* ---------------- 内嵌 Syringe 钩子声明 (单文件, 无需 .dll.inj) ----------------
 * PE 段名必须恰好是 ".syhks00" —— 8 字节精确匹配 (参考无 .inj 的 dll 实测),
 * 16 字节一条: { hookAddr, hookSize, 导出函数名字符串指针, 填充 }。
 * Syringe 枚举段内条目, 按名字 GetProcAddress 后挂钩。SyringeHandshake 导出
 * 可选, 这里只用于写日志与提示信息, 不做 CRC 等任何把关。 */
typedef struct { unsigned int addr; unsigned int size; const char* name; unsigned int pad; } SyHookDecl;
static SyHookDecl g_syHook __attribute__((section(".syhks00"), used, aligned(16))) =
    { ADDR_HOOK_FRAME, 5u, "AutoLoad_FrameHook", 0u };

/* 飞行运动模式的运动器 CLSID (来自 rulesmo.ini 全部飞行步兵:
 * JUMPJET/ARMR/GYRO/MOTHRA/WASP/URAGAN/LUNR 都用 Rocket;
 * {2BEA74E1} 是 YR 原版跳跃机, 一并覆盖)。按类型的 Locomotor GUID 判定,
 * 不依赖运行时对象状态。步行步兵 = {4A582744-9839-11d1-B709-00A024DDAFD1}。 */
static const unsigned char GUID_LOCO_ROCKET[16] =
{ 0x46,0x2C,0x61,0x92, 0x1F,0xF7, 0xD1,0x11, 0xAC,0x9F,0x00,0x60,0x08,0x05,0x5B,0xB5 };
static const unsigned char GUID_LOCO_JUMPJET[16] =
{ 0xE1,0x74,0xEA,0x2B, 0xCA,0x7C, 0xD3,0x11, 0xBE,0x14,0x00,0x10,0x4B,0x62,0xA1,0x6C };

/* ---------------- 日志 (游戏目录 AutoLoad.log) ----------------
 * 构建时定义 AUTOLOAD_NOLOG 即得到无日志版 (AutoLoad_nolog.dll) */

static char* UtoA(unsigned v, char* p) /* p >= 11 字节 */
{
    char tmp[12]; int i = 0, j = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) p[j++] = tmp[--i];
    p[j] = 0; return p;
}

static char* ItoA(int v, char* p)
{
    if (v < 0) { *p++ = '-'; return UtoA((unsigned)(-v), p); }
    return UtoA((unsigned)v, p);
}

static char* HtoA8(unsigned v, char* p) /* p >= 9 字节 */
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 7; i >= 0; i--) { p[i] = hex[v & 0xF]; v >>= 4; }
    p[8] = 0; return p;
}

#ifdef AUTOLOAD_NOLOG
static void LogInit(void) { }
static void LogParts(const char* s, ...) { (void)s; }
#else

static HANDLE g_logFile = NULL;
static CRITICAL_SECTION g_logCS;
static int g_logCSInit = 0;

static void LogInit(void)
{
    if (g_logFile) return;
    if (!g_logCSInit) { InitializeCriticalSection(&g_logCS); g_logCSInit = 1; }
    g_logFile = CreateFileA("AutoLoad.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logFile == INVALID_HANDLE_VALUE) g_logFile = NULL;
}

/* 最多 9 段文本拼接成一行 (缓冲 512 字节, n<480 兜底) */
static void LogParts(const char* s, ...)
{
    char buf[512];
    unsigned n = 0, i, k;
    SYSTEMTIME st;
    DWORD written = 0;
    va_list ap;
    const char* seg;
    if (!g_logFile) return;
    GetLocalTime(&st);
    n += (unsigned)wsprintfA(buf + n, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, s);
    for (k = 0, seg = s; seg && k < 10; k++, seg = va_arg(ap, const char*))
        for (i = 0; seg[i] && n < 480; i++) buf[n++] = seg[i];
    va_end(ap);
    buf[n++] = 13; buf[n++] = 10;  /* CR LF */
    EnterCriticalSection(&g_logCS);
    SetFilePointer(g_logFile, 0, NULL, FILE_END);
    WriteFile(g_logFile, buf, n, &written, NULL);
    LeaveCriticalSection(&g_logCS);
}

#endif /* !AUTOLOAD_NOLOG */

#define LOGS(s)    LogParts((s), NULL)
#define LOG2(s, x) LogParts((s), (x), NULL)

#pragma pack(push, 1)
typedef struct { int ID; unsigned char RTTI; } TargetClass;
#pragma pack(pop)

/* ---------------- 引擎函数调用 (内联汇编, 精确控制 MSVC ABI) ---------------- */

/* thiscall: ECX=tc_out, 栈参数=object; 函数 ret 4 自清理。写入 5 字节 TargetClass */
static void TCFromObject(void* tcOut, void* obj)
{
    __asm__ __volatile__ (
        "pushl %1\n\t"
        "movl %0, %%ecx\n\t"
        "call *%2\n\t"
        :
        : "r"(tcOut), "r"(obj), "r"(ADDR_TC_FROM_OBJECT)
        : "eax", "ecx", "edx", "memory", "cc"
    );
}

/* 0x646E90: ECX=mission, EDX=&TargetTC, 栈: Whom(byval 8字节槽: ID dword + RTTI byte @+4), &DestTC
 * 函数 ret 0xC 自清理。返回 AL = 是否成功入队。 */
static unsigned char QueueMegaMission(int mission, TargetClass* pTarget,
                                      unsigned int whomID, unsigned char whomRTTI,
                                      TargetClass* pDest)
{
    unsigned char ok;
    register int __ecx_mission __asm__("ecx") = mission;
    register TargetClass* __edx_target __asm__("edx") = pTarget;
    __asm__ __volatile__ (
        "pushl %5\n\t"
        "subl $8, %%esp\n\t"
        "movl %3, (%%esp)\n\t"
        "movb %b4, 4(%%esp)\n\t"
        "movl %1, %%ecx\n\t"
        "movl %2, %%edx\n\t"
        "call *%6\n\t"
        : "=a"(ok)
        : "r"(__ecx_mission), "r"(__edx_target), "r"(whomID), "q"(whomRTTI),
          "r"(pDest), "r"(ADDR_QUEUE_MEGA_MISSION)
        : "memory", "cc"
    );
    return ok;
}

/* ---------------- 热键配置 (DLL 同目录 AutoLoad.ini, [AutoLoad] Hotkey=) ---------------- */

#define HKMOD_CONTROL 0x01u
#define HKMOD_SHIFT   0x02u
#define HKMOD_ALT     0x04u
#define HKMOD_WIN     0x08u

static HINSTANCE     g_hSelf = NULL;        /* DllMain 里记录自身模块句柄 */
static unsigned int  g_vkKey = 'D';         /* 主键 VK (装车) */
static unsigned int  g_mods = HKMOD_CONTROL;  /* 修饰键集合 (装车) */
static int           g_hotkeyDisabled = 0;  /* Hotkey=None 时置位 */
static char          g_hotkeyDesc[24] = "Ctrl+D";
/* 同类变体选择热键 (默认 Ctrl+T): 按武器实际值过滤T键选择 */
static unsigned int  g_vVkKey = 'T';
static unsigned int  g_vMods = HKMOD_CONTROL;
static int           g_vDisabled = 0;
static char          g_vDesc[24] = "Ctrl+T";
/* Stagger=1 (default): plan once, release waves gated on boarding. */
/* Stagger=0: legacy single-frame dispatch (boarding order random). */
static int           g_stagger = 1;

static int ChrLower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }


static int StrEqNI(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (ChrLower((unsigned char)*a) != ChrLower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* 单个键名 -> VK 码; 0 = 不认识 */
static unsigned int MapKeyToken(const char* t)
{
    static const struct { const char* name; unsigned int vk; } named[] =
    {
        { "Space", VK_SPACE },  { "Tab", VK_TAB },     { "Enter", VK_RETURN },
        { "Return", VK_RETURN },{ "Backspace", VK_BACK },{ "Back", VK_BACK },
        { "Insert", VK_INSERT },{ "Ins", VK_INSERT },  { "Delete", VK_DELETE },
        { "Del", VK_DELETE },   { "Home", VK_HOME },   { "End", VK_END },
        { "PageUp", VK_PRIOR }, { "PgUp", VK_PRIOR },  { "PageDown", VK_NEXT },
        { "PgDn", VK_NEXT },    { "Up", VK_UP },       { "Down", VK_DOWN },
        { "Left", VK_LEFT },    { "Right", VK_RIGHT }, { "Pause", VK_PAUSE },
        { "Num0", VK_NUMPAD0 }, { "Num1", VK_NUMPAD1 },{ "Num2", VK_NUMPAD2 },
        { "Num3", VK_NUMPAD3 }, { "Num4", VK_NUMPAD4 },{ "Num5", VK_NUMPAD5 },
        { "Num6", VK_NUMPAD6 }, { "Num7", VK_NUMPAD7 },{ "Num8", VK_NUMPAD8 },
        { "Num9", VK_NUMPAD9 },
    };
    int i, n = 0;
    while (t[n]) n++;
    if (n == 1)
    {
        char c = ChrLower((unsigned char)t[0]);
        if (c >= 'a' && c <= 'z') return (unsigned int)(c - 'a' + 'A');
        if (t[0] >= '0' && t[0] <= '9') return (unsigned int)t[0];
    }
    if (n >= 2 && (t[0] == 'F' || t[0] == 'f'))
    {
        int num = 0, ok = 1;
        for (i = 1; i < n; i++)
        {
            if (t[i] < '0' || t[i] > '9') { ok = 0; break; }
            num = num * 10 + (t[i] - '0');
        }
        if (ok && num >= 1 && num <= 24) return VK_F1 + (unsigned int)num - 1;
    }
    for (i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++)
        if (StrEqNI(t, named[i].name)) return named[i].vk;
    return 0;
}

static void BuildHotkeyDescInto(unsigned int vk, unsigned int mods, int disabled,
                                char* desc)
{
    int n = 0;
    if (disabled) { lstrcpyA(desc, "disabled"); return; }
    if (mods & HKMOD_CONTROL) { desc[n++]='C'; desc[n++]='t'; desc[n++]='r'; desc[n++]='l'; desc[n++]='+'; }
    if (mods & HKMOD_ALT)     { desc[n++]='A'; desc[n++]='l'; desc[n++]='t'; desc[n++]='+'; }
    if (mods & HKMOD_SHIFT)   { desc[n++]='S'; desc[n++]='h'; desc[n++]='i'; desc[n++]='f'; desc[n++]='t'; desc[n++]='+'; }
    if (mods & HKMOD_WIN)     { desc[n++]='W'; desc[n++]='i'; desc[n++]='n'; desc[n++]='+'; }
    if (vk >= 'A' && vk <= 'Z') desc[n++] = (char)vk;
    else if (vk >= VK_F1 && vk <= VK_F24)
        n += wsprintfA(desc + n, "F%d", (int)(vk - VK_F1 + 1));
    else n += wsprintfA(desc + n, "VK%u", vk);
    desc[n] = 0;
}

static void BuildHotkeyDesc(void)
{
    BuildHotkeyDescInto(g_vkKey, g_mods, g_hotkeyDisabled, g_hotkeyDesc);
}

static void BuildVariantDesc(void)
{
    BuildHotkeyDescInto(g_vVkKey, g_vMods, g_vDisabled, g_vDesc);
}

static void ParseHotkeyInto(const char* spec, const char* fallbackDesc,
                            unsigned int fallbackVk, unsigned int fallbackMods,
                            unsigned int* outVk, unsigned int* outMods,
                            int* outDisabled, char* outDesc,
                            void (*buildDesc)(void))
{
    char toks[8][16];
    int ntok = 0, i;
    unsigned int mods = 0, key = 0;
    const char* p = spec;
    (void)outDesc; /* 描述由 buildDesc 回调统一生成, 此处仅解析键值 */

    if (StrEqNI(spec, "None") || StrEqNI(spec, "Off") || StrEqNI(spec, "Disabled"))
    {
        *outDisabled = 1;
        buildDesc();
        return;
    }
    *outDisabled = 0;
    while (*p && ntok < 8)
    {
        int n = 0;
        while (*p == '+' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        while (*p && *p != '+' && *p != ' ' && *p != '\t' && n < 15)
            toks[ntok][n++] = *p++;
        toks[ntok][n] = 0;
        if (n) ntok++;
    }
    for (i = 0; i < ntok; i++)
    {
        const char* t = toks[i];
        if (StrEqNI(t, "Ctrl") || StrEqNI(t, "Control")) { mods |= HKMOD_CONTROL; continue; }
        if (StrEqNI(t, "Alt") || StrEqNI(t, "Menu"))     { mods |= HKMOD_ALT; continue; }
        if (StrEqNI(t, "Shift"))                         { mods |= HKMOD_SHIFT; continue; }
        if (StrEqNI(t, "Win") || StrEqNI(t, "Windows"))  { mods |= HKMOD_WIN; continue; }
        {
            unsigned int vk = MapKeyToken(t);
            if (vk) key = vk; /* 多个主键时取最后一个 */
        }
    }
    if (!key)
    {
        LOG2("config: bad Hotkey spec, fallback to ", fallbackDesc);
        LOG2(" (bad spec was: ", spec);
        *outMods = fallbackMods; *outVk = fallbackVk;
        buildDesc();
        return;
    }
    *outMods = mods; /* 允许无修饰键, 由使用者自行权衡冲突 */
    *outVk = key;
    buildDesc();
}

static void ParseHotkey(const char* spec)
{
    ParseHotkeyInto(spec, "Ctrl+D", 'D', HKMOD_CONTROL,
                    &g_vkKey, &g_mods, &g_hotkeyDisabled, g_hotkeyDesc,
                    BuildHotkeyDesc);
}

static void ParseVariantHotkey(const char* spec)
{
    ParseHotkeyInto(spec, "Ctrl+T", 'T', HKMOD_CONTROL,
                    &g_vVkKey, &g_vMods, &g_vDisabled, g_vDesc,
                    BuildVariantDesc);
}

/* 读 DLL 同目录的 AutoLoad.ini; 文件/键缺失时 GetPrivateProfileString 返回默认值 */
static void ReadConfig(void)
{
    char path[MAX_PATH], buf[64];
    DWORD n;
    if (!g_hSelf) return;
    n = GetModuleFileNameA(g_hSelf, path, MAX_PATH);
    if (!n || n >= MAX_PATH - 16) return;
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    path[n] = 0;
    lstrcatA(path, "AutoLoad.ini");
    GetPrivateProfileStringA("AutoLoad", "Hotkey", "Ctrl+D", buf, sizeof(buf), path);
    ParseHotkey(buf);
    LogParts("config: hotkey = ", g_hotkeyDesc,
             " (from AutoLoad.ini [AutoLoad] Hotkey)", NULL);
    GetPrivateProfileStringA("AutoLoad", "VariantHotkey", "Ctrl+T", buf, sizeof(buf), path);
    ParseVariantHotkey(buf);
    LogParts("config: variant hotkey = ", g_vDesc,
             " (from AutoLoad.ini [AutoLoad] VariantHotkey)", NULL);
    GetPrivateProfileStringA("AutoLoad", "Stagger", "1", buf, sizeof(buf), path);
    g_stagger = (buf[0] == '0' && buf[1] == 0) ? 0 : 1;
    LOG2("config: stagger = ", g_stagger ? "1" : "0 (legacy dispatch)");
}

/* ---------------- 键盘轮询线程 ---------------- */

static volatile LONG g_wantLoad = 0;      /* 键盘线程 -> 游戏线程 (装车) */
static volatile LONG g_wantVariant = 0;   /* 键盘线程 -> 游戏线程 (变体选择) */
static HANDLE        g_pollThread = NULL;

static int HotkeyDown(unsigned int vk, unsigned int mods)
{
    int down = 1;
    if (mods & HKMOD_CONTROL) down &= ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    if (mods & HKMOD_SHIFT)   down &= ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0);
    if (mods & HKMOD_ALT)     down &= ((GetAsyncKeyState(VK_MENU)    & 0x8000) != 0);
    if (mods & HKMOD_WIN)     down &= (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0);
    down &= ((GetAsyncKeyState((int)vk) & 0x8000) != 0);
    return down;
}

static DWORD WINAPI PollThread(LPVOID param)
{
    int prevLoad = 0, prevVar = 0;
    (void)param;
    LogParts("poll thread started (hotkey ", g_hotkeyDesc,
             " + variant ", g_vDesc, " watcher)", NULL);
    for (;;)
    {
        Sleep(25);
        {
            DWORD fgPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
            if (fgPid != GetCurrentProcessId())
            {
                prevLoad = 0; prevVar = 0;
                continue;
            }
        }
        if (!g_hotkeyDisabled)
        {
            int down = HotkeyDown(g_vkKey, g_mods);
            if (down && !prevLoad)
            {
                LogParts("hotkey pressed: ", g_hotkeyDesc, NULL);
                /* 计数而非置位: 若游戏帧还没消费上一次按下, 置位会丢一次;
                 * FrameHook 用 Exchange 归零取计数, 同帧合并快照, 跨帧各起 session。 */
                InterlockedIncrement(&g_wantLoad);
            }
            prevLoad = down;
        }
        else prevLoad = 0;
        if (!g_vDisabled)
        {
            int down = HotkeyDown(g_vVkKey, g_vMods);
            if (down && !prevVar)
            {
                LogParts("variant hotkey pressed: ", g_vDesc, NULL);
                InterlockedIncrement(&g_wantVariant);
            }
            prevVar = down;
        }
        else prevVar = 0;
    }
    return 0;
}

static void EnsurePollThread(void)
{
    if (!g_pollThread)
    {
        ReadConfig();
        g_pollThread = CreateThread(NULL, 0, PollThread, NULL, 0, NULL);
    }
}

/* ---------------- 核心: 均衡分配并下发指令 ---------------- */

typedef struct { void* obj; unsigned int type; int cost; int isInf; } PItem;
typedef struct { void* obj; unsigned int type; int freeSlots; int assigned; double sizeLimit; } TItem;

static long long Dist2Leptons(const void* a, const void* b)
{
    const int* la = (const int*)((const char*)a + OFF_LOCATION);
    const int* lb = (const int*)((const char*)b + OFF_LOCATION);
    long long dx = (long long)la[0] - lb[0];
    long long dy = (long long)la[1] - lb[1];
    return dx * dx + dy * dy;
}

/* 进行中的装载轮次状态 (游戏线程独占访问) */
static void*         g_doneObjs[MAX_PASSENGERS];
static int           g_doneCount = 0;
static int           g_passActive = 0;

static int IsDone(const void* obj)
{
    int i;
    for (i = 0; i < g_doneCount; i++)
        if (g_doneObjs[i] == obj) return 1;
    return 0;
}

static void MarkDone(const void* obj)
{
    if (g_doneCount < MAX_PASSENGERS)
        g_doneObjs[g_doneCount++] = (void*)obj;
}

/* 本轮 pass 内已下发的容量预算 (按载具对象记账):
 * 游戏里载具的 used 计数要等步兵真正钻进去才会涨, 逐帧重读会超发 —— 预算在
 * pass 层持久记账, 保证 "10 步兵 + 2 个 1 座载具" 只下发 2 条指令。 */
static void* g_asgTrn[MAX_TRANSPORTS];
static int   g_asgCnt[MAX_TRANSPORTS];
static int   g_asgCount = 0;

/* 本轮 pass 内 (组, 载具) 的同类型已分配数 (均匀主键):
 * 选车只看"车上总人数"时, 跨轮次后距离会把同类又吸回同一辆车
 * (5反+5光+10守争5×4座 → 某要塞进2反)。以"车上同类型人数"为第一主键,
 * 每个成员永远先去"还没有同类的车", 小类型被强制铺开 (上例必得 1+1+2)。
 * 跨帧续传: 与 g_asgCnt 同寿命 (新 pass 起时清零, 同一 pass 内累加),
 * 组下标跨帧稳定 (每帧按同一选中顺序重分组)。 */
static int g_typeAsg[MAX_PASSENGERS][MAX_TRANSPORTS];

static void ClearTypeAsg(void)
{
    int a, b;
    for (a = 0; a < MAX_PASSENGERS; a++)
        for (b = 0; b < MAX_TRANSPORTS; b++)
            g_typeAsg[a][b] = 0;
}

static void EndPass(void)
{
    g_passActive = 0;
    g_doneCount = 0;
    g_asgCount = 0;
}

/* ---- Staggered sessions (Stagger=1) ----
 * Plan once into g_plan*; release per-carrier lanes: the nearest takes
 * Enter immediately, the rest march (Move, no link) and convert to
 * Enter one by one as occupancy rises. Every pointer is revalidated
 * against the Techno Array before use, so dead/destroyed/reloaded
 * units drop out and the session heals itself. Session state lives
 * on the presser's machine only: network-safe. */

/* 规划结果 (DoAutoLoad(forPlan=1) 填写, 发车道消费) */
static void* g_planPax[MAX_PASSENGERS];
static void* g_planTrn[MAX_PASSENGERS];
static int   g_planDist[MAX_PASSENGERS]; /* 距离>>8 (格), 单调, 排序键 */
static int   g_planSame[MAX_PASSENGERS]; /* 下发后车上同类数 (日志 tSame 用) */
static int   g_planCount = 0;
static int   g_planGrp[MAX_PASSENGERS]; /* group index per plan entry */

/* 车道: 一车一车道 (lane[0] 拿 Enter 先行, 其余先 Move 行军,
 * occ 上涨 (有人上车) 才逐个转 Enter —— 同一时刻最多一个未上车的 Enter
 * 持有者, 槽交接无竞争, 顺序=车道顺序=距离序)
 * 多 session 并发: 第二次 Ctrl+D 不得顶掉进行中的旧 session (旧 session
 * 未放行的成员会因此永远拿不到 Enter)。每个 session 独立车道, 各自推进;
 * 已放行名单 (g_relObjs) 全局共享, 新规划跳过还拿着旧 Enter 的单位。 */
#define MAX_SESSIONS 8
#define MAX_SNAP 512
#define MAX_PENDING 8

typedef struct
{
    int active;
    int sessFrame;    /* session 内帧计数 (hook 每帧+1, 确定性) */
    int sessReleased;
    int nlanes;
    void* laneTrn[MAX_TRANSPORTS];
    void* lanePax[MAX_TRANSPORTS][MAX_PASSENGERS];
    int   laneDist[MAX_TRANSPORTS][MAX_PASSENGERS];
    int   laneSame[MAX_TRANSPORTS][MAX_PASSENGERS];
    int   laneN[MAX_TRANSPORTS];
    int   laneNext[MAX_TRANSPORTS];     /* 下一个转 Enter 的下标 */
    int   laneMoveNext[MAX_TRANSPORTS]; /* 下一个待发 Move 的下标 */
    int   laneGate[MAX_TRANSPORTS];     /* 现任 Enter 下发时的 occ */
    int   laneStall[MAX_TRANSPORTS];    /* 现任无进展帧数 */
    void* laneActive[MAX_TRANSPORTS];   /* 现任 (最后下发 Enter 者) */
} ALSession;

static ALSession g_sess[MAX_SESSIONS];

/* 待规划快照队列: 按下瞬间拷贝选中列表 (FrameHook 游戏线程内),
 * 延迟 1 帧再规划。快照保证第二次按下不会偷走第一次的选中内容;
 * 每个快照独立起一个 session, 与旧 session 并发推进。 */
typedef struct
{
    int used;
    int delay;
    int count;
    void* objs[MAX_SNAP];
} ALPending;

static ALPending g_pend[MAX_PENDING];

/* Released units (Enter dispatched): skipped on replan, Enters kept. */
/* Fresh sessions clear the set; dead/gone entries pruned each tick. */
static void* g_relObjs[MAX_PASSENGERS];
static int   g_relCount = 0;





/* 对象是否还在全局 Techno 数组里 (存活且未被删档): 纯指针比较, 不解引用 */
static int ObjAliveInTechnoArray(const void* obj)
{
    void** items = *(void***)OFF_TECHNO_ITEMS;
    int count = *(int*)OFF_TECHNO_COUNT;
    int i;
    if (!obj || !items || count <= 0 || count > 100000)
        return 0;
    for (i = 0; i < count; i++)
        if (items[i] == obj) return 1;
    return 0;
}

static int IsReleased(const void* obj)
{
    int i;
    for (i = 0; i < g_relCount; i++)
        if (g_relObjs[i] == obj) return 1;
    return 0;
}

static void MarkReleased(const void* obj)
{
    if (!IsReleased(obj) && g_relCount < MAX_PASSENGERS)
        g_relObjs[g_relCount++] = (void*)obj;
}

/* 从 released 名单除名 (任务丢失后重新规划用) */
static void UnmarkReleased(const void* obj)
{
    int i;
    for (i = 0; i < g_relCount; i++)
    {
        if (g_relObjs[i] == obj)
        {
            int j;
            for (j = i + 1; j < g_relCount; j++)
                g_relObjs[j - 1] = g_relObjs[j];
            g_relCount--;
            return;
        }
    }
}

/* A released unit still holding its Enter (+0xAC = mission, 7 = Enter)?
 * A user Stop / manual redirect / engine-cleared mission voids the old Enter,
 * so such units must be unmarked and replanned (else skipped forever). */
static int ReleasedHoldsEnter(const void* obj)
{
    if (!ObjAliveInTechnoArray((void*)obj))
        return 0;
    return (*(int*)((char*)obj + 0xAC) == 7);
}

/* Drop dead/gone entries (boarding does NOT remove array entries). */
static void PruneReleased(void)
{
    int i, w = 0;
    for (i = 0; i < g_relCount; i++)
    {
        if (ObjAliveInTechnoArray(g_relObjs[i]))
            g_relObjs[w++] = g_relObjs[i];
    }
    g_relCount = w;
}

/* 引擎 PassengersClass::GetTotalSize 的前向声明 (定义在后, 诊断快照先用) */
static int GetUsedSlots(void* trn);

/* Intra-group proximity swap: keep the spread COUNTS from step 5
 * (evenness first: 1+4 stays 1+4), but move members onto the carrier
 * standing next to them. Kills "position swap" runs (both sides lined up
 * yet A-side units sent to B and vice versa).
 * Greedy: repeatedly take the shortest (member, carrier) pair whose
 * carrier still has group quota left. Same group => same cost, so per
 * carrier totals never change: capacity/SizeLimit conclusions unaffected.
 * Deterministic (fixed scan order, strict < for min): all peers agree.
 * Planning mode only (Stagger=0 keeps legacy behavior). */
static void ReassignProximity(void)
{
    int done[MAX_PASSENGERS];
    int e, moved = 0;
    char b1[16];
    for (e = 0; e < g_planCount; e++) done[e] = 0;
    for (e = 0; e < g_planCount; e++)
    {
        int g, f, ent[MAX_PASSENGERS], ne = 0;
        void* qc[MAX_TRANSPORTS];
        int qleft[MAX_TRANSPORTS], nq = 0;
        int asg[MAX_PASSENGERS], i2, c2, remaining;
        if (done[e])
            continue;
        g = g_planGrp[e];
        /* collect group members + quotas (quota = step-5 counts) */
        for (f = 0; f < g_planCount; f++)
        {
            int c, found = -1;
            if (g_planGrp[f] != g)
                continue;
            done[f] = 1;
            ent[ne++] = f;
            for (c = 0; c < nq; c++)
                if (qc[c] == g_planTrn[f]) { found = c; break; }
            if (found < 0) { qc[nq] = g_planTrn[f]; qleft[nq] = 1; nq++; }
            else qleft[found]++;
        }
        if (nq < 2)
            continue; /* single carrier: nothing to swap */
        for (i2 = 0; i2 < ne; i2++) asg[i2] = -1;
        remaining = ne;
        while (remaining > 0)
        {
            long long bestD = (long long)0x7FFFFFFFFFFFFFFFLL;
            int bi = -1, bc = -1;
            for (i2 = 0; i2 < ne; i2++)
            {
                if (asg[i2] >= 0)
                    continue;
                for (c2 = 0; c2 < nq; c2++)
                {
                    long long dd;
                    if (qleft[c2] <= 0)
                        continue;
                    dd = Dist2Leptons(g_planPax[ent[i2]], qc[c2]);
                    if (dd < bestD) { bestD = dd; bi = i2; bc = c2; }
                }
            }
            if (bi < 0)
                break; /* unreachable: quotas sum == members */
            asg[bi] = bc;
            qleft[bc]--;
            remaining--;
            if (g_planTrn[ent[bi]] != qc[bc])
            {
                g_planTrn[ent[bi]] = qc[bc];
                moved++;
            }
            g_planDist[ent[bi]] = (int)(bestD >> 8);
        }
    }
    /* renumber per-(group,carrier) same-type counts in plan order
     * (identical values when nothing moved) */
    for (e = 0; e < g_planCount; e++)
    {
        int cnt = 0, f;
        for (f = 0; f < e; f++)
            if (g_planGrp[f] == g_planGrp[e] && g_planTrn[f] == g_planTrn[e])
                cnt++;
        g_planSame[e] = cnt + 1;
    }
    LOG2("  reassign proximity moved=", ItoA(moved, b1));
}

static unsigned int TypeOfObj(const void* obj);

static void SessionEnd(ALSession* s, int idx, const char* why)
{
    char b1[16], b2[16], b3[16];
    LogParts("auto-load session ENDED [", ItoA(idx, b3),
             "] (", why ? why : "?", ") frames=",
             ItoA(s->sessFrame, b1),
             " released=", ItoA(s->sessReleased, b2), NULL);
    s->active = 0;
    s->sessFrame = 0;
    s->sessReleased = 0;
    s->nlanes = 0;
    EndPass();
}

static int AnySessionActive(void)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].active) return 1;
    return 0;
}

static int AllocSession(void)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (!g_sess[i].active) return i;
    return -1;
}

/* 乘客占位 (与收集阶段一致: FTOL(Size), 保底 1) */
static int PaxCostOf(void* pax)
{
    unsigned int typePtr;
    int sz;
    if (!pax) return 1;
    typePtr = TypeOfObj(pax);
    if (!typePtr) return 1;
    sz = (int)(*(double*)((char*)typePtr + OFF_TTYPE_SIZE));
    if (sz < 1) sz = 1;
    return sz;
}

/* 其它进行中 session 已预定但尚未体现为 used 的槽位 (防同车并发超发):
 * 对同一 transport, 累加各 session 车道里尚未释放的剩余成员 cost
 * (含现任持有者, 保守多算一个, 宁可欠发不少超发; 已上车后 used 上涨,
 * 剩余同步收缩, 短暂双算可接受)。规划新 session 时调用, exclude<0 表全部。 */
static int ReservedForTransport(const void* trn, int excludeIdx)
{
    int i, L, k, sum = 0;
    if (!trn) return 0;
    for (i = 0; i < MAX_SESSIONS; i++)
    {
        ALSession* s;
        if (i == excludeIdx) continue;
        s = &g_sess[i];
        if (!s->active) continue;
        for (L = 0; L < s->nlanes; L++)
        {
            int start;
            if (s->laneTrn[L] != trn) continue;
            start = s->laneNext[L] > 0 ? s->laneNext[L] - 1 : 0;
            for (k = start; k < s->laneN[L]; k++)
            {
                void* pax = s->lanePax[L][k];
                if (!ObjAliveInTechnoArray(pax)) continue;
                sum += PaxCostOf(pax);
            }
        }
    }
    return sum;
}

static int EnqueuePending(void** items, int count)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
    {
        if (!g_pend[i].used)
        {
            int n = count > MAX_SNAP ? MAX_SNAP : count;
            int k;
            for (k = 0; k < n; k++) g_pend[i].objs[k] = items[k];
            g_pend[i].count = n;
            g_pend[i].delay = 1;
            g_pend[i].used = 1;
            return i;
        }
    }
    return -1;
}

/* Ares NoManualUnload 过滤 (仅用于运输方):
 * TechnoType+0x2FC 存 Ares TechnoTypeExt::ExtData 指针 (Ares 类型构造钩子写入),
 * ExtData+0x4C5 = NoManualUnload (byte)。此类载具 (清道夫 TRACTOR 等) 不可被
 * 进入、内部载员也不可手动卸载 —— 只在收集运输方时跳过, 它自己作为乘客进入
 * LCRF/SAPC 等正常运输载具不受影响 (Ares 光标判定 0x10049E70 也只查目标载具)。
 * ares.dll 未加载时该偏移是普通引擎数据不可信, 直接不过滤 (此时也不存在该规则
 * 标志)。 */
static int IsNoManualUnloadType(unsigned int typePtr)
{
    unsigned int ext;
    HMODULE h;
    if (!typePtr) return 0;
    h = GetModuleHandleA("ares.dll");
    if (!h) return 0;
    ext = *(unsigned int*)(typePtr + OFF_TTYPE_ARES_EXT);
    if (!ext) return 0;
    return *(unsigned char*)(ext + OFF_ARES_EXT_NMU) != 0;
}

/* 飞行运动模式判定: 读类型上的 Locomotor GUID (TechnoType+0x34C) */
static int IsFlyingType(const void* typePtr)
{
    const unsigned char* g;
    int i;
    if (!typePtr)
        return 0;
    g = (const unsigned char*)typePtr + OFF_TTYPE_LOCOMOTOR;
    for (i = 0; i < 16; i++)
        if (g[i] != GUID_LOCO_ROCKET[i])
            break;
    if (i == 16) return 1;
    for (i = 0; i < 16; i++)
        if (g[i] != GUID_LOCO_JUMPJET[i])
            break;
    return i == 16;
}

static unsigned int TypeOfObj(const void* obj)
{
    unsigned int vt = *(unsigned int*)obj;
    if (vt == INFANTRY_VTABLE)  return *(unsigned int*)((const char*)obj + OFF_TYPE_INFANTRY);
    if (vt == UNIT_VTABLE)      return *(unsigned int*)((const char*)obj + OFF_TYPE_UNIT);
    if (vt == BUILDING_VTABLE)  return *(unsigned int*)((const char*)obj + OFF_TYPE_BUILDING);
    if (vt == AIRCRAFT_VTABLE)  return *(unsigned int*)((const char*)obj + OFF_TYPE_AIRCRAFT);
    return 0;
}

/* 引擎 PassengersClass::GetTotalSize(0x473460): 已占槽数 = Σ乘客Type->Size (逐个FTOL) */
static int GetUsedSlots(void* trn)
{
    int r;
    __asm__ __volatile__ (
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(r)
        : "r"((char*)trn + OFF_PASSENGERS), "r"(ADDR_GET_TOTAL_SIZE)
        : "ecx", "edx", "memory", "cc"
    );
    return r;
}

/* ---------------- 同类变体选择 (Ctrl+T): 按武器实际值过滤 ----------------
 * 背景见 NOTES: 原生T键按类型ID字符串全选, 同注册名下不同武器IFV会被一起
 * 选中 (如空车/机枪车/导弹车都是 [FV])。本功能对T加过滤:
 *  - 参照物 = 当前选中里的 (类型指针, 当前普通武器指针) 组合;
 *    Gunner=no 的单位武器键为NULL (退化为纯类型选择, 与原生一致);
 *    Gunner=yes (FV/AMC/TRACTOR/STING等) 按 Techno+0x138
 *    (CurrentWeaponNumber, 空车为0) 取普通Weapon数组 (TechnoType+0x898,
 *    每项0x1C, WeaponType在+0) 的实际武器指针 —— Weapon2与Weapon10指向
 *    同一实际武器即视为同一种; 只用普通Weapon, 忽略升级后的EliteWeapon。
 *  - 单击 (与上次间隔>500ms): 同屏 (Tactical 0x887324/0xB0CEC8, 同原生
 *    0x732950 首按分支); 双击 (间隔<=500ms): 全图 (Techno Array
 *    0xA8EC7C/0xA8EC88, 同原生第二按分支)。只做加选 (与原生一致, 不取消
 *    已选), 选择判定复用原生 0x732580/0x7342C0, 下发复用 Select (vtbl+0x14C)。
 *  - 选择是纯本地UI状态, 不走网络事件, 联机安全 (与T键本身一致)。 */

static unsigned char CallTypeSelectCheck(void* obj)
{
    unsigned char r;
    __asm__ __volatile__ (
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(r)
        : "r"(obj), "r"(ADDR_CHECK_TYPESELECT)
        : "ecx", "edx", "memory", "cc"
    );
    return r;
}

static unsigned char CallAliveCheck(void* obj)
{
    unsigned char r;
    __asm__ __volatile__ (
        "movl %1, %%ecx\n\t"
        "call *%2\n\t"
        : "=a"(r)
        : "r"(obj), "r"(ADDR_CHECK_ALIVE)
        : "ecx", "edx", "memory", "cc"
    );
    return r;
}

static unsigned char CallSelect(void* obj)
{
    unsigned int vt;
    unsigned char r;
    if (!obj) return 0;
    vt = *(unsigned int*)obj;
    __asm__ __volatile__ (
        "movl %1, %%ecx\n\t"
        "movl %2, %%eax\n\t"
        "call *%%eax\n\t"
        : "=a"(r)
        : "r"(obj), "r"(*(unsigned int*)(vt + OFF_SELECT_VTBL))
        : "ecx", "edx", "memory", "cc"
    );
    return r;
}

/* 经 (Ares 可 hook 的) 引擎 accessor 取普通 Weapon[idx] 的 WeaponType 指针。
 * 0x7177C0: ecx=TechnoType*, 栈参idx(int), ret 4 自清理, 返回 WeaponStruct*
 * (WeaponType指针在+0)。必须走它: 18+ 槽位的实际存储由 Ares 接管,
 * 直接计算只对 idx<18 有效。 */
static unsigned int GetNormalWeaponType(unsigned int typePtr, int idx)
{
    unsigned int ws;
    __asm__ __volatile__ (
        "pushl %2\n\t"
        "movl %1, %%ecx\n\t"
        "call *%3\n\t"
        : "=a"(ws)
        : "r"(typePtr), "r"((unsigned int)idx), "r"(ADDR_GET_WEAPON)
        : "ecx", "edx", "memory", "cc"
    );
    if (!ws) return 0;
    return *(unsigned int*)ws;
}

/* 类型ID字符串 (type+0x24, 仅日志用, 定长拷贝防越界) */
static char* TypeIDStr(unsigned int typePtr, char* buf) /* buf >= 32 字节 */
{
    const char* s;
    int i;
    if (!typePtr) { buf[0] = '?'; buf[1] = 0; return buf; }
    s = (const char*)(typePtr + OFF_TTYPE_IDSTR);
    for (i = 0; i < 31 && s[i]; i++) buf[i] = s[i];
    buf[i] = 0;
    return buf;
}

typedef struct { unsigned int type; unsigned int wpn; int cur; int wcount; int pax; int gunner; } VKey;

/* 取变体键: 非Techno则type=0; 非Gunner则wpn=0/cur=-1 (纯类型选择) */
static void VariantKeyOf(void* obj, VKey* k)
{
    unsigned int typePtr, vt;
    k->type = 0; k->wpn = 0; k->cur = -1; k->wcount = 0; k->pax = 0; k->gunner = 0;
    if (!obj) return;
    vt = *(unsigned int*)obj;
    if (vt != INFANTRY_VTABLE && vt != UNIT_VTABLE &&
        vt != BUILDING_VTABLE && vt != AIRCRAFT_VTABLE)
        return;
    typePtr = TypeOfObj(obj);
    if (!typePtr) return;
    k->type = typePtr;
    k->gunner = (*(unsigned char*)(typePtr + OFF_TTYPE_GUNNER)) ? 1 : 0;
    k->pax = *(int*)((const char*)obj + OFF_PASSENGERS);
    if (!k->gunner) return; /* 非IFV逻辑: 纯类型选择 */
    k->wcount = (int)*(unsigned int*)(typePtr + OFF_TTYPE_WEAPONCOUNT);
    {
        int cur = (int)*(unsigned int*)((const char*)obj + OFF_TECHNO_CURWEAPON);
        if (cur < 0 || (k->wcount && cur >= k->wcount))
            cur = 0; /* 引擎回落语义 (0x70DC70): 非法值用Weapon1 */
        k->cur = cur;
        k->wpn = GetNormalWeaponType(typePtr, cur);
    }
}

static int RefMatches(VKey* refs, int nref, unsigned int t, unsigned int w)
{
    int j;
    for (j = 0; j < nref; j++)
        if (refs[j].type == t && refs[j].wpn == w) return 1;
    return 0;
}

static void HistNote(unsigned int* hT, unsigned int* hW, int* hC, int* hN,
                     unsigned int t, unsigned int w)
{
    int i;
    for (i = 0; i < *hN; i++)
        if (hT[i] == t && hW[i] == w) { hC[i]++; return; }
    if (*hN < MAX_VARIANT_HIST)
    {
        hT[*hN] = t; hW[*hN] = w; hC[*hN] = 1; (*hN)++;
    }
}

static void DoVariantSelect(int wholeMap)
{
    void** selItems = *(void***)OFF_SELECTION_ITEMS;
    int selCount = *(int*)OFF_SELECTION_COUNT;
    VKey refs[MAX_VARIANT_REFS];
    unsigned int hT[MAX_VARIANT_HIST];
    unsigned int hW[MAX_VARIANT_HIST];
    int hC[MAX_VARIANT_HIST];
    int nref = 0, hN = 0, i, j;
    int scanned = 0, checked = 0, added = 0;
    char tid[32], line[256];

    if (!selItems || selCount <= 0)
    {
        LOGS("variant-select: selection empty, nothing to do");
        return;
    }
    /* 1. 从当前选中收集 (type, weapon) 参照组合 */
    for (i = 0; i < selCount && i < 512; i++)
    {
        VKey k;
        int dup = 0;
        VariantKeyOf(selItems[i], &k);
        if (!k.type) continue;
        for (j = 0; j < nref; j++)
            if (refs[j].type == k.type && refs[j].wpn == k.wpn) { dup = 1; break; }
        if (!dup && nref < MAX_VARIANT_REFS)
            refs[nref++] = k;
    }
    if (nref <= 0)
    {
        LOGS("variant-select: no techno in selection");
        return;
    }
    {
        char b1[16];
        LogParts("variant-select: refs=", ItoA(nref, b1),
                 wholeMap ? " scope=MAP" : " scope=SCREEN", NULL, NULL);
    }
    for (i = 0; i < nref; i++)
    {
        wsprintfA(line, "  ref%d: type=%s gunner=%d cur=%d/%d wpn=%08X pax=%d",
                  i, TypeIDStr(refs[i].type, tid), refs[i].gunner,
                  refs[i].cur, refs[i].wcount, refs[i].wpn, refs[i].pax);
        LOGS(line);
    }

    /* 2. 候选扫描 + 加选 (同步记直方图: DLL视角下的全部分组) */
    if (wholeMap)
    {
        void** items = *(void***)OFF_TECHNO_ITEMS;
        int count = *(int*)OFF_TECHNO_COUNT;
        if (!items || count <= 0) return;
        for (i = 0; i < count; i++)
        {
            void* obj = items[i];
            VKey k;
            if (!obj) continue;
            scanned++;
            if (!CallTypeSelectCheck(obj)) continue;
            checked++;
            VariantKeyOf(obj, &k);
            if (!k.type) continue;
            HistNote(hT, hW, hC, &hN, k.type, k.wpn);
            if (!RefMatches(refs, nref, k.type, k.wpn)) continue;
            if (CallSelect(obj)) added++;
        }
    }
    else
    {
        unsigned int tac = *(unsigned int*)OFF_TACTICAL_INST;
        int count;
        if (!tac) return;
        count = *(int*)(tac + OFF_TACTICAL_CNT);
        for (i = 0; i < count; i++)
        {
            void* obj = *(void**)(OFF_TACTICAL_ARRAY + (unsigned)i * SZ_TACTICAL_ENTRY);
            VKey k;
            if (!obj) continue;
            scanned++;
            if (!CallAliveCheck(obj)) continue;
            if (!CallTypeSelectCheck(obj)) continue;
            checked++;
            VariantKeyOf(obj, &k);
            if (!k.type) continue;
            HistNote(hT, hW, hC, &hN, k.type, k.wpn);
            if (!RefMatches(refs, nref, k.type, k.wpn)) continue;
            if (CallSelect(obj)) added++;
        }
    }
    {
        char b1[16], b2[16], b3[16];
        LogParts("variant-select: groups=", ItoA(hN, b1),
                 " scanned=", ItoA(scanned, b2),
                 " selectable=", ItoA(checked, b3), NULL);
    }
    for (i = 0; i < hN; i++)
    {
        wsprintfA(line, "  group: type=%s wpn=%08X n=%d%s",
                  TypeIDStr(hT[i], tid), hW[i], hC[i],
                  RefMatches(refs, nref, hT[i], hW[i]) ? " MATCHED" : "");
        LOGS(line);
    }
    {
        char b1[16], b2[16], b3[16];
        int nowSel = *(int*)OFF_SELECTION_COUNT;
        LogParts("variant-select done: added=", ItoA(added, b1),
                 " selection now=", ItoA(nowSel, b2),
                 wholeMap ? " (map)" : " (screen)", NULL);
        LogParts("  refs=", ItoA(nref, b1), " groups=", ItoA(hN, b2),
                 " checked=", ItoA(checked, b3), NULL);
    }
}

static int PassAssigned(const void* trn)
{
    int i;
    for (i = 0; i < g_asgCount; i++)
        if (g_asgTrn[i] == trn)
            return g_asgCnt[i];
    return 0;
}

static void AddAssigned(const void* trn, int slots)
{
    int i;
    for (i = 0; i < g_asgCount; i++)
        if (g_asgTrn[i] == trn) { g_asgCnt[i] += slots; return; }
    if (g_asgCount < MAX_TRANSPORTS)
    {
        g_asgTrn[g_asgCount] = (void*)trn;
        g_asgCnt[g_asgCount] = slots;
        g_asgCount++;
    }
}

static void DoAutoLoad(int forPlan, void** snapItems, int snapCount)
{
    void** items;
    int count;
    PItem ps[MAX_PASSENGERS];
    TItem ts[MAX_TRANSPORTS];
    int np = 0, nt = 0, i, j;
    int events = 0;
    int remaining = 0;
    int nInfSeen = 0, nTrnSeen = 0, nFlySeen = 0, nVehSeen = 0, nNmuSeen = 0;
    int nRelSeen = 0;
    int queueFull = 0;
    /* 快照模式 (Stagger 新 session): 用按下瞬间拷贝的选中, 不读实时选中,
     * 否则第二次按下会偷走第一次的规划输入。snapItems==NULL 时读实时
     * (legacy Stagger=0 延续旧行为)。 */
    if (snapItems)
    {
        items = snapItems;
        count = snapCount;
    }
    else
    {
        items = *(void***)OFF_SELECTION_ITEMS;
        count = *(int*)OFF_SELECTION_COUNT;
    }

    int groupHead[MAX_PASSENGERS];
    int groupType[MAX_PASSENGERS];
    int nextInGroup[MAX_PASSENGERS];
    int ngroups = 0;

    char b1[16], b2[16], b3[16], b4[16];

    if (!items || count <= 0)
    {
        LOGS("auto-load: selection is empty");
        if (g_passActive) EndPass();
        return;
    }

    /* 1. 收集 (全部来自当前选中列表):
     *    乘客 = 非飞行步兵 + 地面载具(Unit, 非飞行); 载具 = 有容量的 techno。
     *    先步兵后载具 => 分组时步兵组在前, 优先上车。 */
    for (i = 0; i < count && i < 512; i++)
    {
        void* obj = items[i];
        unsigned int vt, typePtr;
        int sz;
        if (!obj)
            continue;
        /* 快照与规划之间隔 1 帧, 单位可能在此期间死亡/删除:
         * 先验活, 避免解引用野指针; 旧实时模式同窗口, 同样受益。 */
        if (forPlan && !ObjAliveInTechnoArray(obj))
            continue;
        if (forPlan && IsReleased(obj))
        {
            if (ReleasedHoldsEnter(obj))
            {
                /* 还拿着旧 Enter: 保持不变, 重规划时跳过 */
                nRelSeen++;
                continue;
            }
            /* 旧 Enter 已丢失 (用户 S/改派/引擎清任务): 除名, 当作新单位规划 */
            UnmarkReleased(obj);
            LogParts("  stale release (mission lost, replanning): obj=",
                     HtoA8((unsigned)obj, b1), NULL);
        }
        vt = *(unsigned int*)obj;
        if (vt != INFANTRY_VTABLE && vt != UNIT_VTABLE)
            continue;
        typePtr = TypeOfObj(obj);
        if (IsFlyingType((void*)typePtr))
        {
            nFlySeen++;
            LogParts("  skip flying unit: obj=", HtoA8((unsigned)obj, b1),
                     " type=", HtoA8(typePtr, b2), NULL);
            continue;
        }
        sz = (int)(*(double*)((char*)typePtr + OFF_TTYPE_SIZE));
        if (sz < 1) sz = 1; /* 引擎按 double 累加后 FTOL; <1 的按 1 计保守处理 */
        if (np < MAX_PASSENGERS)
        {
            ps[np].obj = obj;
            ps[np].type = typePtr;
            ps[np].cost = sz;
            ps[np].isInf = (vt == INFANTRY_VTABLE);
            if (vt == INFANTRY_VTABLE) nInfSeen++; else nVehSeen++;
            np++;
        }
    }
    /* 2. 收集载具: UNIT/BUILDING/AIRCRAFT 且容量>0; 空位按引擎规则(Size和)计算 */
    for (i = 0; i < count && i < 512; i++)
    {
        void* obj = items[i];
        unsigned int vt, typePtr;
        int cap, used, freeSlots;
        if (!obj)
            continue;
        if (forPlan && !ObjAliveInTechnoArray(obj))
            continue;
        vt = *(unsigned int*)obj;
        if (vt != UNIT_VTABLE && vt != BUILDING_VTABLE && vt != AIRCRAFT_VTABLE)
            continue;
        typePtr = TypeOfObj(obj);
        nTrnSeen++;
        if (nt >= MAX_TRANSPORTS || !typePtr)
            continue;
        if (IsNoManualUnloadType(typePtr))
        {
            nNmuSeen++;
            LogParts("  transport NoManualUnload (internal cargo, not enterable): obj=",
                     HtoA8((unsigned)obj, b1), " type=", HtoA8(typePtr, b2), NULL);
            continue;
        }
        cap = *(int*)(typePtr + OFF_TTYPE_PASSENGERS);
        if (cap <= 0)
            continue;
        {
            double szLimit = *(double*)((char*)typePtr + OFF_TTYPE_SIZELIMIT);
            if (szLimit < 1.0)
            {
                /* 装载等级 <1: 引擎判 Size<=SizeLimit, 任何乘客(>=1)都进不来 */
                LogParts("  transport SizeLimit<1 (takes nobody), skipped: obj=",
                         HtoA8((unsigned)obj, b1), NULL, NULL);
                continue;
            }
            used = GetUsedSlots(obj);
            /* 减去本轮 pass 已占用的预算, 防止跨帧超发; 规划模式再减去
             * 其它并发 session 已预定 (ReservedForTransport), 防同车并发超发。
             * 不同地点 (不同载具) 时 reserved=0, 互不影响。 */
            freeSlots = cap - used - PassAssigned(obj);
            if (forPlan)
                freeSlots -= ReservedForTransport(obj, -1);
            LogParts("  transport: obj=", HtoA8((unsigned)obj, b1),
                     " type=", HtoA8(typePtr, b2), NULL);
            LogParts("    capacity=", ItoA(cap, b1), " usedSlots=", ItoA(used, b2),
                     " budgetUsed=", ItoA(PassAssigned(obj), b3), NULL);
            LogParts("    sizeLimit=", ItoA((int)szLimit, b1), NULL, NULL);
            if (freeSlots > 0)
            {
                ts[nt].obj = obj;
                ts[nt].type = typePtr;
                ts[nt].freeSlots = freeSlots;
                ts[nt].assigned = 0;
                ts[nt].sizeLimit = szLimit;
                nt++;
            }
            else
            {
                LOGS("    (full or no capacity - skipped)");
            }
        }
    }

    LogParts("auto-load: selected count=", ItoA(count, b1),
             " infantry=", ItoA(nInfSeen, b2), " techno=", ItoA(nTrnSeen, b3),
             " flyingSkipped=", ItoA(nFlySeen, b4), NULL);
    LogParts("noManualUnloadSkipped=", ItoA(nNmuSeen, b1), NULL, NULL);
    LogParts("usable: passengers=", ItoA(np, b1),
             " (inf=", ItoA(nInfSeen, b2), " veh=", ItoA(nVehSeen, b3),
             ") transports with free slots=", ItoA(nt, b4), NULL);
    if (forPlan && nRelSeen > 0)
        LogParts("  already-released (keep old Enter, skipped in replan): ",
                 ItoA(nRelSeen, b1), NULL, NULL);

    if (np <= 0 || nt <= 0)
    {
        LOGS("auto-load: nothing to do (no passengers, or no transport with free slots)");
        if (g_passActive) EndPass();
        return;
    }

    /* 3. "附近"过滤: 只保留与任一选中乘客距离 <= RANGE 的载具 */
    {
        long long r2 = (long long)RANGE_LEPTONS * RANGE_LEPTONS;
        for (j = 0; j < nt; )
        {
            int isNear = 0;
            for (i = 0; i < np; i++)
                if (Dist2Leptons(ts[j].obj, ps[i].obj) <= r2) { isNear = 1; break; }
            if (!isNear)
            {
                LOG2("  transport out of 30-cell range, skipped: obj=",
                     HtoA8((unsigned)ts[j].obj, b1));
                ts[j] = ts[nt - 1];
                nt--;
            }
            else j++;
        }
    }
    if (nt <= 0)
    {
        LOGS("auto-load: all transports out of range");
        if (g_passActive) EndPass();
        return;
    }

    /* 4. 按类型分组 (首次出现顺序), 类型内保持选中顺序 */
    for (i = 0; i < np; i++)
    {
        int g = -1;
        for (j = 0; j < ngroups; j++)
            if ((unsigned int)groupType[j] == ps[i].type) { g = j; break; }
        if (g < 0)
        {
            g = ngroups++;
            groupType[g] = (int)ps[i].type;
            groupHead[g] = i;
            nextInGroup[i] = -1;
        }
        else
        {
            int tail = groupHead[g];
            while (nextInGroup[tail] != -1) tail = nextInGroup[tail];
            nextInGroup[tail] = i;
            nextInGroup[i] = -1;
        }
    }
    LogParts("passenger type groups=", ItoA(ngroups, b1), NULL, NULL);
    for (i = 0; i < ngroups; i++)
    {
        int members = 0;
        for (j = groupHead[i]; j != -1; j = nextInGroup[j]) members++;
        LogParts("  group ", ItoA(i, b1), " type=", HtoA8((unsigned)groupType[i], b2), NULL);
        LOG2("    members=", ItoA(members, b1));
    }

    /* 4b. Nearest-first order within each group: over capacity, the
     * nearby units take the seats (not selection order). Key =
     * (distance to nearest usable carrier, selection index);
     * members with no usable carrier sort last. */
    {
        long long nearD[MAX_PASSENGERS];
        int g, m, k2;
        for (m = 0; m < np; m++)
        {
            long long best = (long long)0x7FFFFFFFFFFFFFFFLL; /* INF: 无可进载具 */
            for (k2 = 0; k2 < nt; k2++)
            {
                long long d;
                unsigned int tv;
                if (ts[k2].freeSlots < ps[m].cost)
                    continue;
                if ((double)ps[m].cost > ts[k2].sizeLimit)
                    continue;
                tv = *(unsigned int*)ts[k2].obj;
                if (!ps[m].isInf && tv != UNIT_VTABLE)
                    continue;
                d = Dist2Leptons(ps[m].obj, ts[k2].obj);
                if (d < best) best = d;
            }
            nearD[m] = best;
        }
        for (g = 0; g < ngroups; g++)
        {
            int ord[MAX_PASSENGERS];
            int nm = 0, a, p2;
            char line[128];
            for (m = groupHead[g]; m != -1; m = nextInGroup[m]) ord[nm++] = m;
            /* 插入排序: (nearD, 选中序号) 升序; np<=128, 开销可忽略。
             * 选中序号(m 本身即 ps 下标=选中顺序)作平局裁决, 与旧行为一致。 */
            for (a = 1; a < nm; a++)
            {
                int cur = ord[a];
                p2 = a - 1;
                while (p2 >= 0 && (nearD[ord[p2]] > nearD[cur] ||
                       (nearD[ord[p2]] == nearD[cur] && ord[p2] > cur)))
                { ord[p2 + 1] = ord[p2]; p2--; }
                ord[p2 + 1] = cur;
            }
            groupHead[g] = ord[0];
            for (a = 0; a < nm; a++)
                nextInGroup[ord[a]] = (a + 1 < nm) ? ord[a + 1] : -1;
            for (a = 0; a < nm; a++)
            {
                /* d2cells = 距离平方(格^2, lepton>>8再平方前先>>16防溢出):
                 * nearD是lepton平方, >>16即格平方, 单调性不变, 仅日志用 */
                wsprintfA(line, "    #%d obj=%08X d2cells=%d%s", a,
                          (unsigned)ps[ord[a]].obj,
                          (int)(nearD[ord[a]] == (long long)0x7FFFFFFFFFFFFFFFLL
                                ? -1 : (nearD[ord[a]] >> 16)),
                          nearD[ord[a]] == (long long)0x7FFFFFFFFFFFFFFFLL
                                ? " (NO TRANSPORT)" : "");
                LOGS(line);
            }
        }
    }

    /* 5. Issue Enter events, round-robin across groups (fair shares).
     *    Carrier pick: [fewest same-type aboard -> fewest total
     *    -> nearest -> selection order]. Same type shares cost and
     *    fit, so a group with no usable carrier is done (budgets
     *    only shrink). Planning mode then seats each member with
     *    its nearest carrier at fixed per-carrier counts. */
    {
        int cursor[MAX_PASSENGERS];
        int alive[MAX_PASSENGERS];
        int nalive = 0, g2;
        /* 事件布局必须与真实点击一致 (FootClass::Active_Click_With 0x4D76C6 ->
         * ClickedMission(7, NULL, 载具, NULL) -> 0x646E90):
         *   Target       = {0,0} 空    (执行器 SetTarCom(空): 不设攻击目标)
         *   Destination  = TC(载具对象, RTTI 0x34) (执行器 SetDestination(载具))
                   * 把载具放 Target -> SetTarCom(载具) = 攻击目标 -> 步兵开火打载具;
                   * Destination 只给格子 -> 步兵走到格旁即放弃 (Area_Guard)。 */
        for (g2 = 0; g2 < ngroups; g2++)
        {
            cursor[g2] = groupHead[g2];
            alive[g2] = 1;
            nalive++;
        }
        while (nalive > 0 && (forPlan || events < MAX_EVENTS_PER_FRAME))
        {
            for (g2 = 0; g2 < ngroups && (forPlan || events < MAX_EVENTS_PER_FRAME); g2++)
            {
                int m, best, bestType, bestAssigned, k;
                long long bestDist = 0;
                TargetClass tcWhom, tcDest, tcTarget;
                char b5[16];

                if (!alive[g2])
                    continue;
                /* 跳过已处理/已无资格成员 (跨帧续传靠 IsDone 去重):
                 * 载具乘客一旦被用作运输工具 (PassAssigned>0) 即永久失去上车资格 */
                m = cursor[g2];
                while (m != -1 && (IsDone(ps[m].obj) ||
                       (!ps[m].isInf && PassAssigned(ps[m].obj) > 0)))
                    m = nextInGroup[m];
                if (m == -1) { alive[g2] = 0; nalive--; continue; }
                best = -1; bestType = 0; bestAssigned = 0;
                for (k = 0; k < nt; k++)
                {
                    long long d;
                    unsigned int tv = *(unsigned int*)ts[k].obj;
                    if (ts[k].freeSlots < ps[m].cost)
                        continue;
                    /* 装载等级: 引擎判 Size<=SizeLimit (NOTES.md §14), 严格复刻 */
                    if ((double)ps[m].cost > ts[k].sizeLimit)
                        continue;
                    /* 载具乘客只进地面/水面载具(Unit), 不进建筑和飞行器 */
                    if (!ps[m].isInf && tv != UNIT_VTABLE)
                        continue;
                    d = Dist2Leptons(ps[m].obj, ts[k].obj);
                    if (best < 0 ||
                        g_typeAsg[g2][k] < bestType ||
                        (g_typeAsg[g2][k] == bestType &&
                         ts[k].assigned < bestAssigned) ||
                        (g_typeAsg[g2][k] == bestType &&
                         ts[k].assigned == bestAssigned && d < bestDist))
                    {
                        best = k;
                        bestType = g_typeAsg[g2][k];
                        bestAssigned = ts[k].assigned;
                        bestDist = d;
                    }
                }
                if (best < 0 || ts[best].freeSlots < ps[m].cost)
                {
                    LogParts("  group out of slots, its leftover members stay out: type=",
                             HtoA8((unsigned)ps[m].type, b1), NULL, NULL);
                    alive[g2] = 0; nalive--;
                    continue;
                }

                TCFromObject(&tcWhom, ps[m].obj);
                tcTarget.ID = 0;
                tcTarget.RTTI = 0;
                TCFromObject(&tcDest, ts[best].obj);

                if (forPlan)
                {
                    /* 规划模式: 只记账并记录 (乘客,载具,距离), 不下发事件。
                     * 预算 (freeSlots/assigned/AddAssigned/g_typeAsg) 与下发模式
                     * 完全相同的时机扣减, 同一分配逻辑、同一记账语义。 */
                    if (g_planCount < MAX_PASSENGERS)
                    {
                        g_planPax[g_planCount] = ps[m].obj;
                        g_planTrn[g_planCount] = ts[best].obj;
                        g_planDist[g_planCount] = (int)(bestDist >> 8);
                        g_planGrp[g_planCount] = g2;
                        ts[best].freeSlots -= ps[m].cost;
                        ts[best].assigned++;
                        AddAssigned(ts[best].obj, ps[m].cost);
                        g_typeAsg[g2][best]++;
                        g_planSame[g_planCount] = g_typeAsg[g2][best];
                        g_planCount++;
                        events++;
                        cursor[g2] = nextInGroup[m];
                        if (cursor[g2] == -1) { alive[g2] = 0; nalive--; }
                    }
                    else
                    {
                        /* 规划数组满 (理论上限 128): 停掉该组, 与事件上限对齐 */
                        alive[g2] = 0; nalive--;
                    }
                    continue;
                }

                if (QueueMegaMission(MISSION_ENTER, &tcTarget,
                                     (unsigned int)tcWhom.ID, tcWhom.RTTI, &tcDest))
                {
                    MarkDone(ps[m].obj);
                    ts[best].freeSlots -= ps[m].cost;
                    ts[best].assigned++;
                    AddAssigned(ts[best].obj, ps[m].cost);
                    g_typeAsg[g2][best]++;
                    events++;
                    cursor[g2] = nextInGroup[m];
                    if (cursor[g2] == -1) { alive[g2] = 0; nalive--; }
                    LogParts("  queue enter: unit=", HtoA8((unsigned)ps[m].obj, b1),
                             " -> trn=", HtoA8((unsigned)ts[best].obj, b2),
                             " cost=", ItoA(ps[m].cost, b3),
                             " tSame=", ItoA(g_typeAsg[g2][best], b4),
                             " dist=", ItoA((int)(bestDist >> 8), b5), NULL);
                }
                else
                {
                    LOGS("  event queue full - will retry next frame");
                    queueFull = 1;
                    goto report; /* OutList 满, 下帧继续 */
                }
            }
        }
    }

    if (forPlan && g_planCount > 0)
        ReassignProximity(); /* 均分计数不变, 组内就近换位消换位跑 */
report:
    /* 6. 是否全部处理完毕 (规划模式由 session 接管生命周期, 直接返回) */
    if (forPlan)
    {
        LogParts("plan result: pairs planned=", ItoA(events, b1),
                 " (stagger session will release them)", NULL, NULL);
        return;
    }
    for (i = 0; i < np; i++)
        if (!IsDone(ps[i].obj)) remaining++;
    LogParts("frame result: events queued=", ItoA(events, b1),
             " units remaining=", ItoA(remaining, b2), NULL, NULL);
    if (queueFull)
    {
        /* OutList 满: 保持 pass, 下帧继续 */
    }
    else if (remaining == 0)
    {
        LOGS("auto-load pass COMPLETE");
        EndPass();
    }
    else if (events == 0)
    {
        /* 没有新事件也没有空位: 剩余单位装不下 (容量/范围所限), 结束本轮 */
        LogParts("auto-load pass ENDED, leftover units=",
                 ItoA(remaining, b1), " (no slots in range)", NULL, NULL);
        EndPass();
    }
    /* 否则保持 g_passActive, 由钩子重新置位 g_wantLoad, 下一帧继续 (OutList 每帧清空) */
}

/* ---------------- 分波放行: 建道 / 发波 / tick ---------------- */

/* 由 g_plan* 建车道: 同一载具的乘客按距离升序排 (插入排序, 总量<=128)。
 * 车道顺序即放行顺序 (wave 0 = 最近) 。车道写入指定 session, 不碰其它 session。 */
static void BuildLanes(ALSession* s)
{
    int p, L, a, b2;
    char b1[16], bb[16];
    s->nlanes = 0;
    for (p = 0; p < g_planCount; p++)
    {
        int L2 = -1;
        for (L = 0; L < s->nlanes; L++)
            if (s->laneTrn[L] == g_planTrn[p]) { L2 = L; break; }
        if (L2 < 0)
        {
            if (s->nlanes >= MAX_TRANSPORTS)
                continue; /* 理论上限, 丢弃多余 */
            L2 = s->nlanes++;
            s->laneTrn[L2] = g_planTrn[p];
            s->laneN[L2] = 0;
        /* Lane order = release order (nearest first). */
        s->laneNext[L2] = 0;
        s->laneMoveNext[L2] = 1; /* lane[0] takes Enter, the rest start marched */
        s->laneGate[L2] = 0;
        s->laneStall[L2] = 0;
        s->laneActive[L2] = 0;
        }
        L = L2;
        if (s->laneN[L] < MAX_PASSENGERS)
        {
            int n = s->laneN[L]++;
            s->lanePax[L][n] = g_planPax[p];
            s->laneDist[L][n] = g_planDist[p];
            s->laneSame[L][n] = g_planSame[p];
        }
    }
    for (L = 0; L < s->nlanes; L++)
    {
        for (a = 1; a < s->laneN[L]; a++)
        {
            void* cp = s->lanePax[L][a];
            int cd = s->laneDist[L][a], cs = s->laneSame[L][a];
            b2 = a - 1;
            while (b2 >= 0 && s->laneDist[L][b2] > cd)
            {
                s->lanePax[L][b2 + 1] = s->lanePax[L][b2];
                s->laneDist[L][b2 + 1] = s->laneDist[L][b2];
                s->laneSame[L][b2 + 1] = s->laneSame[L][b2];
                b2--;
            }
            s->lanePax[L][b2 + 1] = cp;
            s->laneDist[L][b2 + 1] = cd;
            s->laneSame[L][b2 + 1] = cs;
        }
        LogParts("  lane trn=", HtoA8((unsigned)s->laneTrn[L], b1),
                 " npax=", ItoA(s->laneN[L], bb), NULL, NULL);
    }
}

/* 下发一对规划好的 Enter 事件 (事件布局与真实点击一致, 见第5步注释)。
 * 返回 1 = 已入队 (OutList 满返回 0, 调用方下帧重试)。 */
static int IssuePlanned(void* pax, void* trn, int wave, int tSame, int dist)
{
    TargetClass tcWhom, tcDest, tcTarget;
    char b1[16], b2[16], b3[16], b4[16], b5[16];
    TCFromObject(&tcWhom, pax);
    tcTarget.ID = 0;
    tcTarget.RTTI = 0;
    TCFromObject(&tcDest, trn);
    if (!QueueMegaMission(MISSION_ENTER, &tcTarget,
                          (unsigned int)tcWhom.ID, tcWhom.RTTI, &tcDest))
        return 0;
    LogParts("  release wave=", ItoA(wave, b1),
             " unit=", HtoA8((unsigned)pax, b2),
             " -> trn=", HtoA8((unsigned)trn, b3),
             " tSame=", ItoA(tSame, b4),
             " dist=", ItoA(dist, b5), NULL);
    return 1;
}

/* 下发一对 Move 行军事件 (先走后进): 乘客走到载具所在格附近待命,
 * 不进链 (Move 不发 RequestLoading), 故多人并行行军不抢槽。
 * 事件布局与原生点地移动一致: Target={0,0}空, Destination=格子TC
 * (ID=X+1000*Y, RTTI=11=Cell)。返回 1 = 已入队。 */
static int IssueMarch(void* pax, int cellX, int cellY, int wave)
{
    TargetClass tcWhom, tcDest, tcTarget;
    char b1[16], b2[16], b3[16], b4[16];
    TCFromObject(&tcWhom, pax);
    tcTarget.ID = 0;
    tcTarget.RTTI = 0;
    tcDest.ID = cellX + 1000 * cellY;
    tcDest.RTTI = RTTI_CELL;
    if (!QueueMegaMission(MISSION_MOVE, &tcTarget,
                          (unsigned int)tcWhom.ID, tcWhom.RTTI, &tcDest))
        return 0;
    LogParts("  march wave=", ItoA(wave, b1),
             " unit=", HtoA8((unsigned)pax, b2),
             " -> cell=", ItoA(cellX, b3),
             ",", ItoA(cellY, b4), NULL);
    return 1;
}

/* 载具所在格 (lepton>>8, 供 Move 目的地; 规划当帧取值, 载具静止时精确) */
static void TransportCell(const void* trn, int* cx, int* cy)
{
    const int* lc = (const int*)((const char*)trn + OFF_LOCATION);
    *cx = lc[0] >> 8;
    *cy = lc[1] >> 8;
}

/* 补发行军队所有待发 Move (失败即停, 下 tick 重试)。 */
static void PumpMoves(ALSession* s, int L)
{
    while (s->laneMoveNext[L] < s->laneN[L])
    {
        void* pax = s->lanePax[L][s->laneMoveNext[L]];
        int cx, cy;
        if (!ObjAliveInTechnoArray(s->laneTrn[L]))
            return; /* 载具没了, 调用方判死车道 */
        if (!ObjAliveInTechnoArray(pax))
        {
            char b1[16];
            LOG2("  march skip dead/gone passenger: obj=",
                 HtoA8((unsigned)pax, b1));
            s->laneMoveNext[L]++;
            continue;
        }
        TransportCell(s->laneTrn[L], &cx, &cy);
        if (!IssueMarch(pax, cx, cy, s->laneMoveNext[L]))
            return; /* OutList 满, 下 tick 重试 */
        s->laneMoveNext[L]++;
    }
}

/* New session: plan -> first Enter wave + Move marches ->
 * occupancy-gated Enter. 并发模型: 新 session 独占一个 ALSession 槽位,
 * 旧 session 原样保留继续推进; isReplace=1 (有旧 session 仍活着) 时
 * 保留 released 集 (其 Enter 有效), isReplace=0 (全新) 时清空,
 * 否则上轮单位会被永久跳过。快照 snapItems/snapCount 来自按下瞬间拷贝。 */
static void StartSessionFromSnapshot(int isReplace, void** snapItems, int snapCount)
{
    int L, released0 = 0;
    int idx;
    ALSession* s;
    char b1[16], b2[16], b3[16];
    g_passActive = 0;
    g_doneCount = 0;
    g_asgCount = 0;
    ClearTypeAsg();
    g_planCount = 0;
    if (!isReplace && g_relCount > 0)
    {
        LOG2("  released set cleared (fresh session): count=",
             ItoA(g_relCount, b1));
        g_relCount = 0;
    }
    LOGS("--- auto-load session planning ---");
    DoAutoLoad(1, snapItems, snapCount);
    if (g_planCount <= 0)
    {
        LOGS("auto-load session: nothing assigned, no session");
        EndPass();
        return;
    }
    idx = AllocSession();
    if (idx < 0)
    {
        LOGS("auto-load session: all slots busy, new press dropped (old sessions kept)");
        EndPass();
        return;
    }
    s = &g_sess[idx];
    s->active = 0;
    s->sessFrame = 0;
    s->sessReleased = 0;
    s->nlanes = 0;
    BuildLanes(s);
    LogParts("--- auto-load session started [", ItoA(idx, b3),
             "] lanes=", ItoA(s->nlanes, b1),
             " pairs=", ItoA(g_planCount, b2), " ---", NULL);
    s->active = 1;
    for (L = 0; L < s->nlanes; L++)
    {
        /* 首波 (最近者) 直接拿 Enter (空槽直链, 第一个上); 其余 PumpMoves
         * 行军待命, occ 上涨才逐个转 Enter (SessionTick 门控)。 */
        if (!ObjAliveInTechnoArray(s->laneTrn[L]) ||
            !ObjAliveInTechnoArray(s->lanePax[L][0]))
        {
            LogParts("  lane trn=", HtoA8((unsigned)s->laneTrn[L], b1),
                     " wave0 skipped (stale)", NULL, NULL);
            s->laneNext[L] = 1;
            s->laneGate[L] = 0;
            PumpMoves(s, L);
            continue;
        }
        if (IssuePlanned(s->lanePax[L][0], s->laneTrn[L], 0,
                         s->laneSame[L][0], s->laneDist[L][0]))
        {
            MarkReleased(s->lanePax[L][0]);
            released0++;
            s->laneNext[L] = 1;
            s->laneActive[L] = s->lanePax[L][0];
            s->laneGate[L] = GetUsedSlots(s->laneTrn[L]);
            s->laneStall[L] = 0;
        }
        else
        {
            LOGS("  wave0 event queue full - retry next frame");
            s->laneGate[L] = 0;
        }
        PumpMoves(s, L);
    }
    s->sessReleased = released0;
}

/* 每帧推进单个 session: 到期的车道放下一波 (先校验指针, 死亡/消失则丢弃)。
 * 全部车道放完或超时则结束。 */
static void SessionTickOne(ALSession* s, int idx)
{
    int L, allDone;
    char b1[16], b3[16];
    s->sessFrame++;
    if (s->sessFrame > SESS_MAX_FRAMES)
    {
        SessionEnd(s, idx, "timeout");
        return;
    }
    for (L = 0; L < s->nlanes; L++)
    {
        if (s->laneNext[L] >= s->laneN[L] && s->laneMoveNext[L] >= s->laneN[L])
            continue; /* 本车道 Enter/Move 全发完 */
        if (!ObjAliveInTechnoArray(s->laneTrn[L]))
        {
            LogParts("  [", ItoA(idx, b3), "] lane trn=",
                     HtoA8((unsigned)s->laneTrn[L], b1),
                     " transport gone, lane dropped", NULL, NULL);
            s->laneNext[L] = s->laneN[L];
            s->laneMoveNext[L] = s->laneN[L];
            continue;
        }
        /* 先补发行军, 再看 Enter 门 (每 tick 每车道最多转一个 Enter) */
        PumpMoves(s, L);
        if (s->laneNext[L] > 0 && s->laneNext[L] < s->laneN[L])
            s->laneStall[L]++; /* 现任无进展计数 */
        if (s->laneNext[L] < s->laneN[L])
        {
            /* Enter 门: 首波未发直接发; 否则等 occ 上涨 (有人上车, 槽空出来) /
             * 现任死亡 / 超时。门内同一时刻最多一个未上车的 Enter 持有者,
             * 槽交接无竞争, 顺序=车道顺序。 */
            void* pax = s->lanePax[L][s->laneNext[L]];
            int occNow = GetUsedSlots(s->laneTrn[L]);
            int activeDead = (s->laneNext[L] > 0 && s->laneActive[L] &&
                              !ObjAliveInTechnoArray(s->laneActive[L]));
            int stallOut = (s->laneStall[L] > STALL_MAX_FRAMES);
            int gateOpen = (s->laneNext[L] == 0 || occNow > s->laneGate[L] ||
                            activeDead || stallOut);
            if (gateOpen)
            {
                if (!ObjAliveInTechnoArray(pax))
                {
                    LOG2("  skip dead/gone passenger: obj=",
                         HtoA8((unsigned)pax, b1));
                    s->laneNext[L]++;
                    s->laneStall[L] = 0;
                }
                else
                {
                    if (activeDead)
                        LOGS("  lane advance: active dead");
                    else if (stallOut && occNow <= s->laneGate[L])
                        LOGS("  lane advance: stall timeout");
                    if (IssuePlanned(pax, s->laneTrn[L], s->laneNext[L],
                                     s->laneSame[L][s->laneNext[L]],
                                     s->laneDist[L][s->laneNext[L]]))
                    {
                        MarkReleased(pax);
                        s->sessReleased++;
                        s->laneActive[L] = pax;
                        s->laneNext[L]++;
                        s->laneGate[L] = occNow;
                        s->laneStall[L] = 0;
                    }
                    /* 入队失败 (OutList 满): 不推进, 下帧重试 */
                }
            }
        }
    }
    allDone = 1;
    for (L = 0; L < s->nlanes; L++)
        if (s->laneNext[L] < s->laneN[L] ||
            s->laneMoveNext[L] < s->laneN[L]) { allDone = 0; break; }
    if (allDone)
        SessionEnd(s, idx, "all released");
}

/* 每帧推进所有活跃 session (各管各的车道, 互不覆盖)。 */
static void SessionTickAll(void)
{
    int i, any = 0;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].active) { any = 1; break; }
    if (!any) return;
    PruneReleased();
    for (i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].active) SessionTickOne(&g_sess[i], i);
}

/* ---------------- Syringe 接口 ---------------- */

typedef struct
{
    int cbSize;
    int num_hooks;
    unsigned int checksum;
    DWORD exeFilesize;
    DWORD exeTimestamp;
    unsigned int exeCRC;
    int cchMessage;
    char* Message;
} SyringeHandshakeInfo;

__declspec(dllexport) HRESULT __cdecl SyringeHandshake(SyringeHandshakeInfo* pInfo)
{
    if (!pInfo || pInfo->cbSize < (int)sizeof(SyringeHandshakeInfo))
        return E_FAIL;
    /* 不做 exeCRC 校验: 引擎映像相同的重打包变体 CRC 也可能不同 (如
     * gamemd.exe 0x54CC0A13 vs gamemd-spawn.exe 0x098465B3, 时间戳同为
     * 0x3BDF544E, 见 §19), 误拒会让整个 DLL 不加载。本 DLL 只被 Syringe
     * 按钩子声明注入游戏引擎, 帧钩子另有 gamemd.exe/gamemd-spawn.exe
     * 宿主兜底, 不会影响其它程序。 */
    LogInit();
    ReadConfig();
    {
        char b[16];
        LOG2("handshake OK, exe crc=", HtoA8(pInfo->exeCRC, b));
    }
    if (pInfo->Message && pInfo->cchMessage > 0)
    {
        const char* head = "AutoLoad: press ";
        const char* mid = " to load (march+stagger); ";
        const char* tail = " to select same IFV variant (T-filtered).";
        int i = 0, k;
        for (k = 0; head[k] && i < pInfo->cchMessage - 1; k++) pInfo->Message[i++] = head[k];
        for (k = 0; g_hotkeyDesc[k] && i < pInfo->cchMessage - 1; k++) pInfo->Message[i++] = g_hotkeyDesc[k];
        for (k = 0; mid[k] && i < pInfo->cchMessage - 1; k++) pInfo->Message[i++] = mid[k];
        for (k = 0; g_vDesc[k] && i < pInfo->cchMessage - 1; k++) pInfo->Message[i++] = g_vDesc[k];
        for (k = 0; tail[k] && i < pInfo->cchMessage - 1; k++) pInfo->Message[i++] = tail[k];
        pInfo->Message[i] = '\0';
    }
    return S_OK;
}

/* 每帧钩子: Syringe 在游戏主循环帧首调用, 返回后由 Syringe 恢复寄存器并执行原指令 */
__declspec(dllexport) DWORD __cdecl AutoLoad_FrameHook(void* regs)
{
    static int s_armed = 0;      /* legacy Stagger=0 用的延迟触发 */
    static int s_countdown = 0;  /* 还剩几个延迟帧 */
    static DWORD s_lastVariantTick = 0; /* 变体选择双击计时 */
    int pi;
    LONG nVar, nLoad;
    (void)regs;
    /* 宿主兜底校验: 仅在游戏引擎进程里工作 (Syringe 未调用 handshake 时保证安全)。
     * 引擎有两种文件名: gamemd.exe (经典/MO) 与 gamemd-spawn.exe (CnCNet 联机
     * 客户端, 见 §19: 同一时间戳/同基址/关键地址字节级一致, 仅 CRC 不同)。
     * GetModuleHandleA(NULL) 即本进程映像句柄, 恒非空, 仅用于卫语句完整性。 */
    if (!GetModuleHandleA("gamemd.exe") && !GetModuleHandleA("gamemd-spawn.exe"))
        return 0;
    LogInit();
    EnsurePollThread();
    nVar = InterlockedExchange(&g_wantVariant, 0);
    while (nVar-- > 0)
    {
        DWORD now = GetTickCount();
        int wholeMap = (s_lastVariantTick != 0 && now - s_lastVariantTick <= VARIANT_DBL_MS);
        s_lastVariantTick = now;
        LOGS(wholeMap ? "--- variant-select pass (MAP, double-press) ---"
                      : "--- variant-select pass (SCREEN, single-press) ---");
        DoVariantSelect(wholeMap);
    }
    nLoad = InterlockedExchange(&g_wantLoad, 0);
    if (g_stagger)
    {
        /* 先到期先起 session (上帧按下的快照, 延迟 1 帧, 与旧 arming 一致);
         * 再把本帧新按下的快照入队, 留到下帧规划 —— 保证"按下当帧的其它输入"
         * 避让, 且第二次按下拿的是新选中, 不偷第一次的快照。 */
        for (pi = 0; pi < MAX_PENDING; pi++)
        {
            if (g_pend[pi].used && --g_pend[pi].delay <= 0)
            {
                int isReplace = AnySessionActive() ? 1 : 0;
                g_pend[pi].used = 0;
                StartSessionFromSnapshot(isReplace,
                                         g_pend[pi].objs, g_pend[pi].count);
            }
        }
    }
    if (nLoad)
    {
        if (g_stagger)
        {
            /* Stagger 并发模型: 按下瞬间快照选中并排队;
             * 每个快照独立起 session, 不顶掉旧 session。同帧内多次按下
             * (nLoad>1) 快照相同, 合并为一次。 */
            void** liveItems = *(void***)OFF_SELECTION_ITEMS;
            int liveCount = *(int*)OFF_SELECTION_COUNT;
            char b1[16];
            if (nLoad > 1)
                LogParts("hotkey coalesced presses in one frame: ",
                         ItoA((int)nLoad, b1), NULL, NULL);
            if (!liveItems || liveCount <= 0)
            {
                LOGS("hotkey pressed but selection empty, nothing queued");
            }
            else
            {
                int slot = EnqueuePending(liveItems, liveCount);
                if (slot < 0)
                    LOGS("pending queue full, new press dropped (old sessions kept)");
                else
                    LogParts("hotkey snapshot queued: slot=",
                             ItoA(slot, b1), NULL, NULL);
            }
        }
        else if (g_passActive)
        {
            /* 上一轮未完成的延续: 不再延迟, 立即继续 */
            DoAutoLoad(0, NULL, 0);
            if (g_passActive)
                InterlockedExchange(&g_wantLoad, 1); /* 未完, 下一帧继续 */
        }
        else
        {
            /* 新触发: 延迟 1 帧再下发, 避开按下 Ctrl+D 当帧的其它输入 */
            s_armed = 1;
            s_countdown = 1;
        }
    }
    else if (!g_stagger && s_armed && --s_countdown <= 0)
    {
        s_armed = 0;
        {
            g_passActive = 1;
            g_doneCount = 0;
            g_asgCount = 0;
            ClearTypeAsg(); /* 同类型计数与本轮预算同寿命 */
            LOGS("--- auto-load pass started ---");
            DoAutoLoad(0, NULL, 0);
            if (g_passActive)
                InterlockedExchange(&g_wantLoad, 1); /* 未完, 下一帧继续 */
        }
    }
    if (g_stagger)
    {
        SessionTickAll(); /* sessions run on the presser's machine only */
    }
    return 0; /* 0 = 恢复原指令并正常继续 */
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hSelf = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
