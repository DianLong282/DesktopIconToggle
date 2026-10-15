// ============================================================================
//  DesktopIconToggle  —  双击桌面空白处，切换桌面图标的显示 / 隐藏
//
//  设计目标：极低性能开销。
//    * 纯 Win32，无框架、无运行时依赖（静态 CRT），单进程。
//    * 只有一个从不显示的顶层窗口（托盘图标宿主）+ 一个消息循环，空闲时 0% CPU。
//    * 一个 WH_MOUSE_LL 低级鼠标钩子，回调里只判断一个 wParam，其余一概不做，
//      真正的判断/切换全部 PostMessage 到自己的窗口过程里执行，
//      这样钩子回调永远是"立即返回"，绝不会触发系统的 LowLevelHooksTimeout 而被摘除。
//    * 只有真正检测到"桌面空白处双击"时才做跨进程命中测试，其余时间零额外工作。
//
//  平台：Windows 7 / 10 / 11，x64。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
static const wchar_t* const kAppName   = L"桌面图标开关";
static const wchar_t* const kWinClass  = L"DesktopIconToggleWnd";
static const wchar_t* const kMutexName = L"Local\\DesktopIconToggle.SingleInstance";
static const wchar_t* const kRunKey    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* const kRunValue  = L"DesktopIconToggle";
static const wchar_t* const kCfgKey    = L"Software\\DesktopIconToggle";

// 桌面右键菜单里 "查看 -> 显示桌面图标" 的命令 ID，发给 SHELLDLL_DefView 即为切换
#define CMD_TOGGLE_DESKTOP_ICONS 0x7402

#define IDI_APPICON   1
#define IDM_TOGGLE    1001
#define IDM_AUTOSTART 1002
#define IDM_EXIT      1003

#define WM_TRAY_CALLBACK (WM_APP + 1)
#define WM_DO_TOGGLE     (WM_APP + 2)
#define WM_APP_QUIT      (WM_APP + 3)

// ---------------------------------------------------------------------------
// 全局
// ---------------------------------------------------------------------------
static HINSTANCE g_hinst   = nullptr;
static HWND      g_hwnd    = nullptr;
static HHOOK     g_hook    = nullptr;
static UINT      g_msgTaskbarCreated = 0;
static NOTIFYICONDATAW g_nid = {};
static bool      g_trayAdded = false;
static POINT     g_clickPt = { 0, 0 };   // 钩子回调与窗口过程同线程，无需加锁

// ---------------------------------------------------------------------------
// 诊断日志（--diag / --click 模式写入 exe 同目录 diag.txt）
// ---------------------------------------------------------------------------
static wchar_t g_log[65536];
static size_t  g_logLen = 0;

static void LogW(const wchar_t* fmt, ...)
{
    if (g_logLen >= ARRAYSIZE(g_log) - 4) return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(g_log + g_logLen, ARRAYSIZE(g_log) - g_logLen - 2,
                          _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) g_logLen += (size_t)n;
    g_log[g_logLen++] = L'\r';
    g_log[g_logLen++] = L'\n';
    g_log[g_logLen]   = L'\0';
}

// 把 fileName 拼成 exe 所在目录下的绝对路径。
// 注意：不能用相对路径——那会按"当前工作目录"解析，而不是 exe 目录，
// 从别处启动时就会写到/删到别的地方去。
static bool PathBesideExe(const wchar_t* fileName, wchar_t* out, size_t cch)
{
    DWORD n = GetModuleFileNameW(nullptr, out, (DWORD)cch);
    if (n == 0 || n >= cch) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    if (wcslen(out) + wcslen(fileName) >= cch) return false;
    wcscat_s(out, cch, fileName);
    return true;
}

static void WriteLogFile(const wchar_t* fileName, bool append)
{
    if (g_logLen == 0) return;

    wchar_t path[MAX_PATH];
    if (!PathBesideExe(fileName, path, ARRAYSIZE(path))) return;

    int bytes = WideCharToMultiByte(CP_UTF8, 0, g_log, (int)g_logLen,
                                    nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        char* utf8 = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)bytes);
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, g_log, (int)g_logLen, utf8, bytes, nullptr, nullptr);
            HANDLE f = CreateFileW(path,
                                   append ? FILE_APPEND_DATA : GENERIC_WRITE,
                                   FILE_SHARE_READ, nullptr,
                                   append ? OPEN_ALWAYS : CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(f, utf8, (DWORD)bytes, &written, nullptr);
                CloseHandle(f);
            }
            HeapFree(GetProcessHeap(), 0, utf8);
        }
    }
    g_logLen = 0;
    g_log[0] = L'\0';
}

// 覆盖写（--diag / --click 用）
static void LogFlush(const wchar_t* fileName) { WriteLogFile(fileName, false); }
// 追加写（--trace 用）
static void LogAppend(const wchar_t* fileName) { WriteLogFile(fileName, true); }

// --trace：常驻实例把每一次双击的判定过程追加写进 trace.log，便于排查问题
static bool g_trace = false;

static void Trace(const wchar_t* fmt, ...)
{
    if (!g_trace) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    LogW(L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[1024];
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    LogW(L"%s", buf);
    LogAppend(L"trace.log");
}

// ---------------------------------------------------------------------------
// 定位桌面图标列表视图
//    经典结构： Progman \ SHELLDLL_DefView \ SysListView32
//    部分系统： 某个顶层 WorkerW \ SHELLDLL_DefView \ SysListView32
// ---------------------------------------------------------------------------
static BOOL CALLBACK FindDefViewEnumProc(HWND hwnd, LPARAM lp)
{
    HWND child = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (child) {
        *(HWND*)lp = child;
        return FALSE;
    }
    return TRUE;
}

static HWND FindDefView()
{
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND dv = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (dv) return dv;
    }
    HWND found = nullptr;
    EnumWindows(FindDefViewEnumProc, (LPARAM)&found);
    return found;
}

static HWND FindDesktopListView()
{
    HWND dv = FindDefView();
    if (!dv) return nullptr;
    return FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
}

static bool IsDesktopContainer(HWND h)
{
    wchar_t cls[64];
    for (HWND w = h; w; w = GetParent(w)) {
        if (!GetClassNameW(w, cls, ARRAYSIZE(cls))) break;
        if (!wcscmp(cls, L"SHELLDLL_DefView") ||
            !wcscmp(cls, L"Progman") ||
            !wcscmp(cls, L"WorkerW")) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 跨进程命中测试： LVM_HITTEST 需要目标进程地址空间里的 LVHITTESTINFO
//   只在真的双击时才用，开销可忽略（一次 OpenProcess + 一次分配，可复用）
// ---------------------------------------------------------------------------
struct RemoteSession {
    HANDLE proc = nullptr;
    void*  mem  = nullptr;
    DWORD  pid  = 0;
    bool   ok   = false;
};

static bool RemoteOpen(RemoteSession& s, HWND hLV)
{
    if (s.ok) return true;
    s.pid = 0;
    GetWindowThreadProcessId(hLV, &s.pid);
    if (!s.pid) return false;
    s.proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                         FALSE, s.pid);
    if (!s.proc) return false;
    s.mem = VirtualAllocEx(s.proc, nullptr, sizeof(LVHITTESTINFO),
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s.mem) {
        CloseHandle(s.proc);
        s.proc = nullptr;
        return false;
    }
    s.ok = true;
    return true;
}

static void RemoteClose(RemoteSession& s)
{
    if (s.mem && s.proc) VirtualFreeEx(s.proc, s.mem, 0, MEM_RELEASE);
    if (s.proc) CloseHandle(s.proc);
    s = RemoteSession();
}

// 返回： >=0 命中的图标序号； -1 空白； -2 无法判定
static int RemoteHitTest(RemoteSession& s, HWND hLV, POINT pt)
{
    if (!RemoteOpen(s, hLV)) return -2;

    LVHITTESTINFO hti;
    ZeroMemory(&hti, sizeof(hti));
    hti.pt = pt;
    // LVM_HITTEST 要的是控件客户区坐标；桌面列表视图的客户区原点通常就在屏幕 (0,0)，
    // 但多显示器（主屏不在原点）时必须换算，否则会判断错。
    MapWindowPoints(nullptr, hLV, &hti.pt, 1);
    hti.iItem = -1;

    if (!WriteProcessMemory(s.proc, s.mem, &hti, sizeof(hti), nullptr)) return -2;

    DWORD_PTR res = 0;
    if (!SendMessageTimeoutW(hLV, LVM_HITTEST, 0, (LPARAM)s.mem,
                             SMTO_ABORTIFHUNG | SMTO_NORMAL, 250, &res)) {
        return -2;
    }

    LVHITTESTINFO out;
    ZeroMemory(&out, sizeof(out));
    if (!ReadProcessMemory(s.proc, s.mem, &out, sizeof(out), nullptr)) return -2;
    return out.iItem;
}

// 命中测试不可用时的保守兜底：热项或存在选中项 => 认为点在图标上
static bool PointOnIconFallback(HWND hLV)
{
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hLV, LVM_GETHOTITEM, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &res)) {
        if ((int)(INT_PTR)res >= 0) return true;
    }
    res = 0;
    if (SendMessageTimeoutW(hLV, LVM_GETSELECTEDCOUNT, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 200, &res)) {
        if ((LONG_PTR)res > 0) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 图标显示状态
// ---------------------------------------------------------------------------
static bool RegHideIcons(bool* found)
{
    bool hidden = false;
    *found = false;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"HideIcons", nullptr, &type, (LPBYTE)&v, &sz) == ERROR_SUCCESS
            && type == REG_DWORD) {
            hidden = (v != 0);
            *found = true;
        }
        RegCloseKey(k);
    }
    return hidden;
}

// 桌面图标当前是否隐藏
static bool IconsHidden()
{
    HWND lv = FindDesktopListView();
    if (lv) return !IsWindowVisible(lv);
    bool found = false;
    return RegHideIcons(&found) && found;
}

// ---------------------------------------------------------------------------
// 执行切换
// ---------------------------------------------------------------------------
struct ToggleReport {
    bool dvFound      = false;
    bool lvFound      = false;
    bool visibleBefore= true;
    bool visibleAfter = true;
    bool cmdSent      = false;
    bool fallbackUsed = false;
};

static ToggleReport ToggleDesktopIcons()
{
    ToggleReport r;

    HWND dv = FindDefView();
    r.dvFound = (dv != nullptr);
    HWND lv = dv ? FindWindowExW(dv, nullptr, L"SysListView32", nullptr) : nullptr;
    r.lvFound = (lv != nullptr);
    if (lv) r.visibleBefore = (IsWindowVisible(lv) != FALSE);
    if (!dv) return r;

    DWORD_PTR res = 0;
    r.cmdSent = (SendMessageTimeoutW(dv, WM_COMMAND, CMD_TOGGLE_DESKTOP_ICONS, 0,
                                     SMTO_ABORTIFHUNG | SMTO_NORMAL, 400, &res) != 0);

    // 重新取句柄：Explorer 有可能重建了窗口
    HWND dv2 = FindDefView();
    HWND lv2 = dv2 ? FindWindowExW(dv2, nullptr, L"SysListView32", nullptr) : nullptr;
    if (lv2) {
        r.visibleAfter = (IsWindowVisible(lv2) != FALSE);
        if (r.visibleAfter == r.visibleBefore) {
            // 命令没有生效（不同系统版本行为可能有差异）—— 直接切列表视图可见性兜底
            ShowWindow(lv2, r.visibleBefore ? SW_HIDE : SW_SHOW);
            r.fallbackUsed = true;
            r.visibleAfter = (IsWindowVisible(lv2) != FALSE);
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// 双击事件处理
// ---------------------------------------------------------------------------
static void HandleDesktopDoubleClick(POINT pt)
{
    HWND lv = FindDesktopListView();
    if (!lv) {
        Trace(L"双击 (%d, %d) -> 未找到桌面图标列表视图，忽略", pt.x, pt.y);
        return;
    }

    HWND hit = WindowFromPoint(pt);
    if (!hit) {
        Trace(L"双击 (%d, %d) -> WindowFromPoint 无结果，忽略", pt.x, pt.y);
        return;
    }

    wchar_t cls[64] = L"<未知>";
    GetClassNameW(hit, cls, ARRAYSIZE(cls));

    if (hit == lv) {
        // 点在桌面图标列表视图上：区分"图标"与"空白"
        RemoteSession s;
        int idx = RemoteHitTest(s, lv, pt);
        RemoteClose(s);
        if (idx >= 0) {
            Trace(L"双击 (%d, %d) -> 桌面图标 #%d，交给系统打开", pt.x, pt.y, idx);
            return;
        }
        if (idx == -2 && PointOnIconFallback(lv)) {
            Trace(L"双击 (%d, %d) -> 命中测试不可用，兜底判定为图标，忽略", pt.x, pt.y);
            return;
        }
    } else if (!IsDesktopContainer(hit)) {
        Trace(L"双击 (%d, %d) -> 窗口类=%s，不是桌面，忽略", pt.x, pt.y, cls);
        return;
    }

    ToggleReport r = ToggleDesktopIcons();
    Trace(L"双击 (%d, %d) -> 桌面空白，已切换：列表视图可见 %d -> %d（命令=%d 兜底=%d）",
          pt.x, pt.y, r.visibleBefore, r.visibleAfter, r.cmdSent, r.fallbackUsed);
}

// ---------------------------------------------------------------------------
// 自己判定"双击"，而不是依赖系统的 WM_LBUTTONDBLCLK 消息。
//   原因：系统只在鼠标下的窗口"带了 CS_DBLCLKS 类风格"时才生成那条消息。
//   桌面图标隐藏后，鼠标底下是 SHELLDLL_DefView / WorkerW，无法保证它们带这个风格；
//   一旦不带，图标隐藏后就再也无法用双击显示回来了。
//   这里按系统同样的参数（双击间隔 + 双击矩形）自行配对，行为与系统一致。
// ---------------------------------------------------------------------------
static DWORD g_lastDownTick = 0;
static POINT g_lastDownPt   = { 0, 0 };
static bool  g_clickArmed   = false;

static bool IsDoubleClick(const MSLLHOOKSTRUCT* ms)
{
    if (g_clickArmed) {
        const DWORD dt = ms->time - g_lastDownTick;          // 无符号回绕安全
        const int   tx = GetSystemMetrics(SM_CXDOUBLECLK) / 2;
        const int   ty = GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        if (dt <= GetDoubleClickTime() &&
            abs(ms->pt.x - g_lastDownPt.x) <= tx &&
            abs(ms->pt.y - g_lastDownPt.y) <= ty) {
            g_clickArmed = false;                            // 配对成功，避免三击重复触发
            return true;
        }
    }
    g_clickArmed   = true;
    g_lastDownTick = ms->time;
    g_lastDownPt   = ms->pt;
    return false;
}

// 低级鼠标钩子：回调必须极快 —— 只做配对判断并转发坐标，绝不做文件 I/O 或跨进程调用
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
        if (IsDoubleClick(ms)) {
            g_clickPt = ms->pt;
            if (g_hwnd) PostMessageW(g_hwnd, WM_DO_TOGGLE, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 开机自启（HKCU Run）
// ---------------------------------------------------------------------------
static bool IsAutoStartEnabled()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    bool on = false;
    if (RegQueryValueExW(k, kRunValue, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[sz / sizeof(wchar_t)] = L'\0';
        on = (buf[0] != L'\0');
    }
    RegCloseKey(k);
    return on;
}

static void SetAutoStart(bool enable)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &k, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t path[MAX_PATH + 4];
        DWORD n = GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            path[0] = L'"';
            path[n + 1] = L'"';
            path[n + 2] = L'\0';
            RegSetValueExW(k, kRunValue, 0, REG_SZ,
                           (const BYTE*)path,
                           (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        }
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// 托盘图标
// ---------------------------------------------------------------------------
static void AddTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_CALLBACK;
    g_nid.hIcon            = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, 0, 0,
                                               LR_DEFAULTSIZE | LR_SHARED);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, kAppName);
    g_trayAdded = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
}

static void RemoveTrayIcon()
{
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

// 托盘气泡提示（托盘图标不存在时静默忽略）
static void ShowBalloon(const wchar_t* title, const wchar_t* text)
{
    if (!g_trayAdded) return;
    g_nid.uFlags |= NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, title);
    wcscpy_s(g_nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

// ---------------------------------------------------------------------------
// 配置文件 config.ini（exe 同目录）
//
// 特意不使用 GetPrivateProfileIntW：那套 API 会按文件路径做进程级缓存，
// 外部改了文件再读很可能拿到的还是旧值；而 WritePrivateProfileString(NULL,...)
// 这种"清缓存"的写法又会把缓存内容写回磁盘，覆盖掉用户刚做的修改。
// 这里自己读取 + 解析，行为完全可预期。
// ---------------------------------------------------------------------------
static const wchar_t* const kCfgFileName = L"config.ini";
static const wchar_t* const kCfgSection  = L"General";
static const wchar_t* const kCfgTrayKey  = L"ShowTrayIcon";

struct AppConfig {
    bool showTrayIcon = true;      // 缺省显示托盘图标：这是"改错了也能恢复"的安全默认
};

static AppConfig g_cfg;
static wchar_t  g_cfgPath[MAX_PATH] = L"";

static bool BuildConfigPath()
{
    if (g_cfgPath[0]) return true;
    return PathBesideExe(kCfgFileName, g_cfgPath, ARRAYSIZE(g_cfgPath));
}

// 模板必须是 UTF-8 且【不带 BOM】（编译器 /utf-8 已保证执行字符集为 UTF-8）。
// 带 BOM 的话首行会变成 "\xEF\xBB\xBF[General]"，节名匹配不上。
static const char kCfgTemplate[] =
    "; ============================================================\r\n"
    ";  桌面图标开关  配置文件\r\n"
    ";\r\n"
    ";  * 本文件只在程序【启动时】读一次，运行期间不再访问。\r\n"
    ";    改完保存后，重新双击一次 exe 即可生效：正在运行的实例会自动退出，\r\n"
    ";    由新实例接管，不需要去任务管理器。\r\n"
    ";  * 删除本文件后，下次启动程序会自动重新生成。\r\n"
    ";  * 格式：[节名] 下面是 键=值；以 ; 或 # 开头的行是注释。\r\n"
    "; ============================================================\r\n"
    "\r\n"
    "[General]\r\n"
    "; 是否在任务栏通知区域（托盘）显示图标\r\n"
    ";   1 = 显示（默认）\r\n"
    ";   0 = 隐藏\r\n"
    ";\r\n"
    "; 隐藏托盘图标后，双击桌面空白处的功能不受影响，只是没有托盘菜单了。\r\n"
    "; 想恢复或退出：把下面这项改回 1 并保存，再双击一次 exe，托盘图标就回来了，\r\n"
    "; 然后右键图标 → 退出。\r\n"
    "ShowTrayIcon=1\r\n";

static void WriteConfigTemplate()
{
    HANDLE f = CreateFileW(g_cfgPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Trace(L"[配置] 无法创建 %s（错误码 %lu），本次使用默认设置", g_cfgPath, GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(f, kCfgTemplate, (DWORD)(sizeof(kCfgTemplate) - 1), &written, nullptr);
    CloseHandle(f);
}

// 把整个配置文件读进内存并转成宽字符。兼容 UTF-16LE(BOM) / UTF-8(BOM 可有可无) / ANSI。
// 键名都是 ASCII，所以即使文件是 GBK 也能正确匹配到键。
static wchar_t* ReadConfigText()
{
    HANDLE f = CreateFileW(g_cfgPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(f);
        return nullptr;
    }
    DWORD bytes = (DWORD)size.QuadPart;
    BYTE* raw = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bytes + 2);
    if (!raw) { CloseHandle(f); return nullptr; }

    DWORD got = 0;
    BOOL ok = ReadFile(f, raw, bytes, &got, nullptr);
    CloseHandle(f);
    if (!ok || got == 0) { HeapFree(GetProcessHeap(), 0, raw); return nullptr; }
    raw[got] = 0;
    raw[got + 1] = 0;
    bytes = got;

    wchar_t* text = nullptr;
    if (bytes >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {          // UTF-16LE
        size_t chars = (bytes - 2) / 2;
        text = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(wchar_t));
        if (text) {
            memcpy(text, raw + 2, chars * sizeof(wchar_t));
            text[chars] = L'\0';
        }
    } else {
        DWORD off = (bytes >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        int chars = MultiByteToWideChar(CP_UTF8, 0, (const char*)raw + off,
                                        (int)(bytes - off), nullptr, 0);
        if (chars > 0) {
            text = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, ((size_t)chars + 1) * sizeof(wchar_t));
            if (text) {
                MultiByteToWideChar(CP_UTF8, 0, (const char*)raw + off,
                                    (int)(bytes - off), text, chars);
                text[chars] = L'\0';
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, raw);
    return text;
}

static int ParseIniInt(const wchar_t* text, const wchar_t* section,
                       const wchar_t* key, int defValue)
{
    if (!text) return defValue;

    wchar_t want[128];
    _snwprintf_s(want, ARRAYSIZE(want), _TRUNCATE, L"[%s]", section);
    const size_t wantLen = wcslen(want);
    const size_t keyLen  = wcslen(key);

    bool inSection = false;
    for (const wchar_t* p = text; *p; ) {
        const wchar_t* lineEnd = wcspbrk(p, L"\r\n");
        const wchar_t* s = p;
        size_t len = lineEnd ? (size_t)(lineEnd - p) : wcslen(p);

        while (len && (*s == L' ' || *s == L'\t')) { ++s; --len; }
        while (len && (s[len - 1] == L' ' || s[len - 1] == L'\t')) --len;

        if (len > 0 && *s != L';' && *s != L'#') {
            if (*s == L'[') {
                inSection = (len == wantLen && _wcsnicmp(s, want, wantLen) == 0);
            } else if (inSection) {
                const wchar_t* eq = nullptr;
                for (size_t i = 0; i < len; ++i) {
                    if (s[i] == L'=') { eq = s + i; break; }
                }
                if (eq) {
                    size_t klen = (size_t)(eq - s);
                    while (klen && (s[klen - 1] == L' ' || s[klen - 1] == L'\t')) --klen;
                    if (klen == keyLen && _wcsnicmp(s, key, keyLen) == 0) {
                        const wchar_t* v = eq + 1;
                        const wchar_t* vEnd = s + len;
                        while (v < vEnd && (*v == L' ' || *v == L'\t')) ++v;
                        wchar_t buf[32];
                        size_t bi = 0;
                        while (v < vEnd && bi < ARRAYSIZE(buf) - 1 &&
                               (*v == L'-' || (*v >= L'0' && *v <= L'9'))) {
                            buf[bi++] = *v++;
                        }
                        buf[bi] = L'\0';
                        return bi ? _wtoi(buf) : defValue;
                    }
                }
            }
        }

        if (!lineEnd) break;
        p = lineEnd;
        while (*p == L'\r' || *p == L'\n') ++p;
    }
    return defValue;
}

static void LoadConfig()
{
    if (!BuildConfigPath()) return;
    if (GetFileAttributesW(g_cfgPath) == INVALID_FILE_ATTRIBUTES) {
        WriteConfigTemplate();
    }
    wchar_t* text = ReadConfigText();
    const int v = ParseIniInt(text, kCfgSection, kCfgTrayKey, 1);
    if (text) HeapFree(GetProcessHeap(), 0, text);
    g_cfg.showTrayIcon = (v != 0);
}

// 按配置让托盘图标与实际状态一致（幂等，可反复调用）
static void ApplyTrayVisibility()
{
    if (g_cfg.showTrayIcon) {
        if (!g_trayAdded) AddTrayIcon();
    } else {
        RemoveTrayIcon();
    }
}

static void ShowTrayMenu()
{
    HMENU m = CreatePopupMenu();
    if (!m) return;

    bool hidden = IconsHidden();
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, hidden ? L"显示桌面图标" : L"隐藏桌面图标");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                IDM_AUTOSTART, L"开机自动启动");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);   // 让菜单正确关闭
}

// 首次运行提示
static void MaybeShowFirstRunTip()
{
    HKEY k = nullptr;
    DWORD shown = 0, sz = sizeof(shown), type = 0;
    bool needTip = true;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, L"TipShown", nullptr, &type, (LPBYTE)&shown, &sz) == ERROR_SUCCESS
            && shown == 1) {
            needTip = false;
        }
        RegCloseKey(k);
    }
    // 托盘图标被配置隐藏时不要消费掉这个提示，等图标出现再说
    if (!needTip || !g_trayAdded) return;
    ShowBalloon(kAppName,
                L"已启动：双击桌面空白处即可隐藏 / 显示桌面图标。\n"
                L"右键托盘图标可退出或设置开机自动启动。");

    if (RegCreateKeyExW(HKEY_CURRENT_USER, kCfgKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                        &k, nullptr) == ERROR_SUCCESS) {
        DWORD one = 1;
        RegSetValueExW(k, L"TipShown", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DO_TOGGLE:
        HandleDesktopDoubleClick(g_clickPt);
        return 0;

    case WM_TRAY_CALLBACK:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ToggleDesktopIcons();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TOGGLE:
            ToggleDesktopIcons();
            return 0;
        case IDM_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        return 0;

    case WM_APP_QUIT:
        // 另一个实例要求接管（用户改了 config.ini 后重新双击了 exe）
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    // Explorer 重启后重新挂托盘图标（是否挂由配置决定）
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        g_trayAdded = false;
        ApplyTrayVisibility();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 命令行工具模式（便于脚本调用 / 自检）
// ---------------------------------------------------------------------------
static void DiagProbe()
{
    bool regFound = false;
    bool regHidden = RegHideIcons(&regFound);

    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND dv = FindDefView();
    HWND lv = FindDesktopListView();

    wchar_t cls[64] = L"<无>";
    if (dv) GetClassNameW(dv, cls, ARRAYSIZE(cls));

    LogW(L"== 状态 ==");
    LogW(L"Progman      = 0x%p", (void*)progman);
    LogW(L"DefView      = 0x%p  (类=%s)", (void*)dv, cls);
    LogW(L"ListView     = 0x%p", (void*)lv);
    LogW(L"ListView 可见 = %s", (lv && IsWindowVisible(lv)) ? L"是" : L"否");
    LogW(L"注册表 HideIcons = %s", regFound ? (regHidden ? L"1 (隐藏)" : L"0 (显示)") : L"不存在");
    LogW(L"结论 IconsHidden = %s", IconsHidden() ? L"隐藏" : L"显示");
    LogW(L"配置文件     = %s", g_cfgPath[0] ? g_cfgPath : L"<不可用>");
    LogW(L"ShowTrayIcon = %d  (托盘图标应为%s)", g_cfg.showTrayIcon ? 1 : 0,
         g_cfg.showTrayIcon ? L"显示" : L"隐藏");

    if (!lv) { LogFlush(L"diag.txt"); return; }

    // 栅格扫描。命中测试与窗口遮挡无关（LVM_HITTEST 是控件内部的逻辑测试），
    // 所以即使桌面被别的窗口盖住，也能验证"图标 / 空白"的判别是否正确。
    const int X0 = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int Y0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int W  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int H  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int STEP = 48;

    RemoteSession s;
    int scanned = 0, topmost = 0, blank = 0, icon = 0, unknown = 0;
    POINT blankSamples[5];   int nBlank = 0;
    POINT iconSamples[3];    int nIcon  = 0;
    POINT usableSamples[3];  int nUsable = 0;   // 既空白、又在最上层（真正能触发切换）的点

    for (int y = Y0 + STEP / 2; y < Y0 + H; y += STEP) {
        for (int x = X0 + STEP / 2; x < X0 + W; x += STEP) {
            POINT p = { x, y };
            ++scanned;

            bool isTop = (WindowFromPoint(p) == lv);
            if (isTop) ++topmost;

            int r = RemoteHitTest(s, lv, p);
            if (r >= 0) {
                ++icon;
                if (nIcon < 3) iconSamples[nIcon++] = p;
            } else if (r == -1) {
                ++blank;
                if (nBlank < 5) blankSamples[nBlank++] = p;
                if (isTop && nUsable < 3) usableSamples[nUsable++] = p;
            } else {
                ++unknown;
            }
        }
    }
    RemoteClose(s);

    LogW(L"== 栅格扫描 (%dx%d, 步长 %d, 共 %d 点) ==", W, H, STEP, scanned);
    LogW(L"命中判别: 空白 %d / 图标 %d / 无法判定 %d", blank, icon, unknown);
    LogW(L"其中当前处于最上层(未被遮挡)的桌面点: %d", topmost);
    for (int i = 0; i < nBlank; ++i)
        LogW(L"  空白点示例: (%d, %d)", blankSamples[i].x, blankSamples[i].y);
    for (int i = 0; i < nIcon; ++i)
        LogW(L"  图标点示例: (%d, %d)", iconSamples[i].x, iconSamples[i].y);
    for (int i = 0; i < nUsable; ++i)
        LogW(L"  可直接实测的空白点: (%d, %d)", usableSamples[i].x, usableSamples[i].y);

    POINT cur;
    if (GetCursorPos(&cur)) {
        HWND hit = WindowFromPoint(cur);
        wchar_t c2[64] = L"<无>";
        if (hit) GetClassNameW(hit, c2, ARRAYSIZE(c2));
        RemoteSession s2;
        int r = (hit == lv) ? RemoteHitTest(s2, lv, cur) : -3;
        RemoteClose(s2);
        const wchar_t* verdict = (r >= 0) ? L"图标" : (r == -1) ? L"空白" :
                                 (r == -2) ? L"无法判定" : L"非桌面列表视图";
        LogW(L"当前光标 (%d, %d) -> 窗口类=%s, 判定=%s", cur.x, cur.y, c2, verdict);
    }

    LogFlush(L"diag.txt");
}

static void ClickProbe(int x, int y)
{
    POINT pt = { x, y };
    HWND lv = FindDesktopListView();
    HWND hit = WindowFromPoint(pt);
    wchar_t c[64] = L"<无>";
    if (hit) GetClassNameW(hit, c, ARRAYSIZE(c));

    LogW(L"== 点击判定 (%d, %d) ==", x, y);
    LogW(L"WindowFromPoint = 0x%p (类=%s)", (void*)hit, c);
    LogW(L"桌面 ListView   = 0x%p", (void*)lv);

    bool blankDecision = false;
    if (!lv) {
        LogW(L"未找到桌面 ListView");
    } else if (hit == lv) {
        RemoteSession s;
        int r = RemoteHitTest(s, lv, pt);
        RemoteClose(s);
        LogW(L"LVM_HITTEST iItem = %d  (>=0 图标, -1 空白, -2 失败)", r);
        if (r >= 0) blankDecision = false;
        else if (r == -1) blankDecision = true;
        else blankDecision = !PointOnIconFallback(lv);
    } else {
        blankDecision = IsDesktopContainer(hit);
        LogW(L"命中窗口是否属于桌面容器 = %s", blankDecision ? L"是" : L"否");
    }

    LogW(L"判定结果: %s", blankDecision ? L"桌面空白 -> 会切换" : L"不是桌面空白 -> 不动作");
    LogFlush(L"diag.txt");

    if (blankDecision) {
        ToggleReport r = ToggleDesktopIcons();
        LogW(L"已切换: dv=%d lv=%d 可见 %d->%d 命令=%d 兜底=%d",
             r.dvFound, r.lvFound, r.visibleBefore, r.visibleAfter,
             r.cmdSent, r.fallbackUsed);
        LogFlush(L"diag.txt");
    }
}

// 合成一次真实的双击，用来端到端验证钩子链路
static void SimulateDoubleClick(int x, int y)
{
    POINT saved;
    GetCursorPos(&saved);

    SetCursorPos(x, y);
    Sleep(40);

    INPUT in;
    ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;

    int sent = 0;
    for (int i = 0; i < 2; ++i) {
        in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        sent += (int)SendInput(1, &in, sizeof(INPUT));
        Sleep(50);
        in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        sent += (int)SendInput(1, &in, sizeof(INPUT));
        Sleep(50);
    }

    Sleep(350);            // 给常驻实例的处理留出时间
    SetCursorPos(saved.x, saved.y);

    POINT now;
    GetCursorPos(&now);
    LogW(L"SendInput 注入成功 %d/4 个事件 (GetLastError=%lu)", sent, GetLastError());
    LogW(L"已在 (%d, %d) 合成双击；光标现位于 (%d, %d)", x, y, now.x, now.y);
    bool f = false;
    LogW(L"之后 HideIcons = %s", RegHideIcons(&f) ? L"1" : L"0");
    LogW(L"之后 ListView 可见 = %s",
         (FindDesktopListView() && IsWindowVisible(FindDesktopListView())) ? L"是" : L"否");
    LogFlush(L"diag.txt");
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
static const wchar_t* SkipSpaces(const wchar_t* p)
{
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

// 必须让进程"DPI 感知"：低级鼠标钩子回传的是物理像素坐标，
// 而 DPI 不感知的进程里 WindowFromPoint / GetCursorPos 用的是被虚拟化的坐标。
// 125% 缩放时两者相差 1.25 倍，命中判断会落在完全错误的位置上。
// 这里动态调用，保持对老系统的兼容（没有该导出函数就逐级降级）。
static void EnableDpiAwareness()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        SetCtxFn setCtx = (SetCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2（避免 SDK 版本宏差异）
        if (setCtx && setCtx((HANDLE)(INT_PTR)-4)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetAwareFn)(int);
        SetAwareFn setAware = (SetAwareFn)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (setAware && SUCCEEDED(setAware(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) return;
    }
    SetProcessDPIAware();   // Vista+ 兜底
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int)
{
    g_hinst = hInstance;
    EnableDpiAwareness();
    LoadConfig();          // 首次运行会在 exe 同目录生成 config.ini

    // ---- 命令行模式 ----
    const wchar_t* cmd = SkipSpaces(lpCmdLine ? lpCmdLine : L"");
    if (wcsstr(cmd, L"--trace")) g_trace = true;   // 常驻 + 记录判定过程

    if (*cmd == L'-' && !g_trace) {
        if (!wcsncmp(cmd, L"--toggle", 8)) {
            ToggleDesktopIcons();
            return 0;
        }
        if (!wcsncmp(cmd, L"--diag", 6)) {
            DiagProbe();
            return 0;
        }
        if (!wcsncmp(cmd, L"--click", 7)) {
            int x = 0, y = 0;
            if (swscanf_s(cmd + 7, L"%d %d", &x, &y) == 2) ClickProbe(x, y);
            return 0;
        }
        if (!wcsncmp(cmd, L"--dblclick", 10)) {
            int x = 0, y = 0;
            if (swscanf_s(cmd + 10, L"%d %d", &x, &y) == 2) SimulateDoubleClick(x, y);
            return 0;
        }
    }

    // ---- 常驻模式 ----
    // 配置只在启动时读一次，运行期间不再碰配置文件。
    // 所以"改了配置要生效"就靠重新启动：如果已经有实例在跑，先请它退出，由本次启动接管。
    // 这样即使托盘图标被配置隐藏、没有菜单可点，也只要再双击一次 exe 就能生效。
    HWND existing = FindWindowW(kWinClass, kAppName);
    if (existing) {
        PostMessageW(existing, WM_APP_QUIT, 0, 0);
        for (int i = 0; i < 50 && FindWindowW(kWinClass, kAppName); ++i) {
            Sleep(40);                       // 最多等 2 秒，避免死等
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"上一个实例没有正常退出，请稍后重试，或在任务管理器里结束 DesktopIconToggle.exe。",
                    kAppName, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kWinClass;
    if (!RegisterClassExW(&wc)) return 1;

    // 一个从不显示的顶层窗口：托盘图标宿主 + 消息投递目标
    // WS_EX_TOOLWINDOW 保证它永远不会出现在 Alt+Tab / 任务栏里
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWinClass, kAppName, WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ApplyTrayVisibility();          // 按配置决定是否挂托盘图标（只在启动时决定一次）
    MaybeShowFirstRunTip();

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
    if (!g_hook) {
        MessageBoxW(nullptr, L"安装鼠标钩子失败，程序无法工作。", kAppName,
                    MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }

    if (g_trace) {
        wchar_t tracePath[MAX_PATH];
        if (PathBesideExe(L"trace.log", tracePath, ARRAYSIZE(tracePath))) {
            DeleteFileW(tracePath);
        }
        HICON probe = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
        Trace(L"[启动] 钩子安装成功，窗口=0x%p", (void*)g_hwnd);
        Trace(L"[启动] 配置文件 %s", g_cfgPath[0] ? g_cfgPath : L"<不可用>");
        Trace(L"[启动] 自定义图标资源 %s，托盘图标 %s",
              probe ? L"可用" : L"缺失（回退到系统默认图标）",
              g_trayAdded ? L"已添加" : L"按配置已隐藏");
        Trace(L"[启动] 双击桌面空白处即可切换图标");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    if (mutex) CloseHandle(mutex);
    return 0;
}
