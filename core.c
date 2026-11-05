#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include "resource.h"

// 补全高分屏与热键兼容宏
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000

// 注册表配置路径
#define REG_PATH_INPUT      L"SOFTWARE\\Microsoft\\InputMethod\\Settings\\CHS"
#define REG_VAL_INPUT       L"Enable Double Pinyin"
#define REG_PATH_RUN        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VAL_RUN         L"PinShift"
#define REG_PATH_APP        L"Software\\PinShift"
#define REG_VAL_HIDE_TRAY   L"HideTrayIcon"

// 消息与定时器 ID
#define WM_TRAYICON         (WM_APP + 1)
#define HOTKEY_TOGGLE_MODE  1
#define HOTKEY_TOGGLE_HIDE  2
#define TIMER_DPI_SETTLE_ID 1

// 全局静态状态（单线程驱动）
static HWND  s_hwnd = NULL;
static HICON s_iconQuan = NULL;
static HICON s_iconShuang = NULL;
static int   s_iconSize = 0;
static bool  s_trayVisible = false;
static DWORD s_currentMode = 0;
static UINT  s_msgTaskbarCreated = 0;

static const wchar_t* const CLASS_NAME = L"PinShiftHotkeyWindow_Class";
static const wchar_t* const MUTEX_NAME = L"Local\\PinShiftUniqueMutexSecure";

// --- 前置函数原型 ---
static void UpdateTrayIcon(void);

// --- 输入法状态管理 ---
static DWORD GetIMEMode(void) {
    HKEY hKey;
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, REG_VAL_INPUT, NULL, NULL, (LPBYTE)&mode, &size);
        RegCloseKey(hKey);
    }
    return mode;
}

static void NotifyActiveWindowIME(void) {
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD_PTR result;
        SendMessageTimeoutW(
            fg, WM_SETTINGCHANGE, 0,
            (LPARAM)L"Control Panel\\Input Method",
            SMTO_ABORTIFHUNG, 50, &result
        );
    }
}

static void SetIMEMode(DWORD mode) {
    if (s_currentMode == mode) return;

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VAL_INPUT, 0, REG_DWORD, (const BYTE*)&mode, sizeof(mode));
        RegCloseKey(hKey);

        s_currentMode = mode;
        if (s_trayVisible) UpdateTrayIcon();
        NotifyActiveWindowIME();
    }
}

static void ToggleIMEMode(void) {
    SetIMEMode((s_currentMode == 1) ? 0 : 1);
}

// --- 高分屏适配与高清图标加载 ---
static int QueryTaskbarIconMetric(void) {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = 96;

    typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
    typedef int  (WINAPI *GetSystemMetricsForDpiProc)(int, UINT);

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowProc pGetDpi = (GetDpiForWindowProc)GetProcAddress(hUser32, "GetDpiForWindow");
    GetSystemMetricsForDpiProc pGetMetrics = (GetSystemMetricsForDpiProc)GetProcAddress(hUser32, "GetSystemMetricsForDpi");

    if (hTray && pGetDpi) {
        dpi = pGetDpi(hTray);
    } else {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
        }
    }

    if (dpi == 0) dpi = 96;
    if (pGetMetrics) {
        int sz = pGetMetrics(SM_CXSMICON, dpi);
        if (sz > 0) return sz;
    }
    return (int)(16 * dpi / 96);
}

static HICON LoadIconSharp(HINSTANCE hInst, int resID, int targetSize) {
    HICON hIcon = NULL;
    HMODULE hComCtl = LoadLibraryW(L"comctl32.dll");

    if (hComCtl) {
        typedef HRESULT (WINAPI *PFN_LoadIconWithScaleDown)(HINSTANCE, PCWSTR, int, int, HICON*);
        PFN_LoadIconWithScaleDown pfnScaleDown =
            (PFN_LoadIconWithScaleDown)GetProcAddress(hComCtl, "LoadIconWithScaleDown");

        if (pfnScaleDown) {
            if (SUCCEEDED(pfnScaleDown(hInst, MAKEINTRESOURCEW(resID), targetSize, targetSize, &hIcon))) {
                FreeLibrary(hComCtl);
                return hIcon;
            }
        }
        FreeLibrary(hComCtl);
    }

    return (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(resID), IMAGE_ICON, targetSize, targetSize, LR_DEFAULTCOLOR);
}

static void ReloadIconResources(int targetSize) {
    if (targetSize <= 0) targetSize = 16;
    if (s_iconSize == targetSize && s_iconQuan && s_iconShuang) return;

    HINSTANCE hInst = GetModuleHandleW(NULL);
    HICON newQuan = LoadIconSharp(hInst, IDI_ICON_QUAN, targetSize);
    HICON newShuang = LoadIconSharp(hInst, IDI_ICON_SHUANG, targetSize);

    if (newQuan && newShuang) {
        if (s_iconQuan) DestroyIcon(s_iconQuan);
        if (s_iconShuang) DestroyIcon(s_iconShuang);
        s_iconQuan = newQuan;
        s_iconShuang = newShuang;
        s_iconSize = targetSize;
    } else {
        if (newQuan) DestroyIcon(newQuan);
        if (newShuang) DestroyIcon(newShuang);
    }
}

// --- 托盘管理 ---
static void UpdateTrayIcon(void) {
    if (!s_trayVisible) return;

    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = s_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (s_currentMode == 1) ? s_iconShuang : s_iconQuan;
    wcscpy_s(nid.szTip, 128, (s_currentMode == 1) ? L"PinShift: 双拼模式" : L"PinShift: 全拼模式");

    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
}

static void RemoveTrayIcon(void) {
    if (!s_trayVisible) return;
    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = s_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    s_trayVisible = false;
}

// --- 开机自启与配置 ---
static bool IsAutoStartEnabled(void) {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_RUN, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        enabled = (RegQueryValueExW(hKey, REG_VAL_RUN, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(hKey);
    }
    return enabled;
}

static void ToggleAutoStart(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_RUN, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (IsAutoStartEnabled()) {
            RegDeleteValueW(hKey, REG_VAL_RUN);
        } else {
            wchar_t path[MAX_PATH + 2];
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            swprintf_s(path, MAX_PATH + 2, L"\"%s\"", exe);
            RegSetValueExW(hKey, REG_VAL_RUN, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }
}

static bool IsTrayHiddenConfig(void) {
    HKEY hKey;
    DWORD val = 0;
    DWORD size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_APP, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, REG_VAL_HIDE_TRAY, NULL, NULL, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return (val == 1);
}

static void ToggleTrayVisibility(void) {
    bool hide = !IsTrayHiddenConfig();
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_APP, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = hide ? 1 : 0;
        RegSetValueExW(hKey, REG_VAL_HIDE_TRAY, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }

    if (hide) {
        RemoveTrayIcon();
    } else {
        s_trayVisible = true;
        ReloadIconResources(QueryTaskbarIconMetric());
        UpdateTrayIcon();
    }
}

static void ShowContextMenu(void) {
    SetForegroundWindow(s_hwnd);
    HMENU hMenu = CreatePopupMenu();

    UINT quanFlag   = MF_STRING | ((s_currentMode == 0) ? MF_CHECKED : 0);
    UINT shuangFlag = MF_STRING | ((s_currentMode == 1) ? MF_CHECKED : 0);
    UINT autoFlag   = MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0);

    AppendMenuW(hMenu, quanFlag,   IDM_FULL_PINYIN,   L"全拼输入");
    AppendMenuW(hMenu, shuangFlag, IDM_DOUBLE_PINYIN, L"双拼输入");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, autoFlag,   IDM_AUTO_START,    L"开机启动");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING,  IDM_HELP,          L"快捷键说明");
    AppendMenuW(hMenu, MF_STRING,  IDM_EXIT,          L"退出程序");

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, s_hwnd, NULL);
    PostMessageW(s_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// --- 窗口消息回调 ---
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == s_msgTaskbarCreated && s_msgTaskbarCreated != 0) {
        SetTimer(hwnd, TIMER_DPI_SETTLE_ID, 300, NULL);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        SetTimer(hwnd, TIMER_DPI_SETTLE_ID, 300, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DPI_SETTLE_ID) {
            KillTimer(hwnd, TIMER_DPI_SETTLE_ID);
            if (!IsTrayHiddenConfig()) {
                s_trayVisible = true;
                int newSize = QueryTaskbarIconMetric();
                if (newSize != s_iconSize || !s_iconQuan || !s_iconShuang) {
                    ReloadIconResources(newSize);
                }
                UpdateTrayIcon();
            }
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE_MODE) {
            ToggleIMEMode();
        } else if (wParam == HOTKEY_TOGGLE_HIDE) {
            ToggleTrayVisibility();
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ToggleIMEMode();
        } else if (lParam == WM_RBUTTONUP) {
            ShowContextMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FULL_PINYIN:
            SetIMEMode(0);
            break;
        case IDM_DOUBLE_PINYIN:
            SetIMEMode(1);
            break;
        case IDM_AUTO_START:
            ToggleAutoStart();
            break;
        case IDM_HELP:
            MessageBoxW(
                hwnd,
                L"【快捷操作指南】\n\n"
                L"• 切换输入模式：Ctrl + Shift + Y\n"
                L"• 显隐托盘图标：Ctrl + Shift + Win + Y\n"
                L"• 显隐托盘图标：按住Shift + 双击程序",
                L"PinShift", MB_OK | MB_ICONINFORMATION
            );
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_DPI_SETTLE_ID);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_MODE);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_HIDE);
        RemoveTrayIcon();

        if (s_iconQuan) {
            DestroyIcon(s_iconQuan);
            s_iconQuan = NULL;
        }
        if (s_iconShuang) {
            DestroyIcon(s_iconShuang);
            s_iconShuang = NULL;
        }

        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- 纯单线程入口与事件泵 ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *SetDpiAwarenessProc)(DPI_AWARENESS_CONTEXT);
        SetDpiAwarenessProc setDpi = (SetDpiAwarenessProc)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (setDpi) setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    HANDLE hMutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND oldHwnd = FindWindowW(CLASS_NAME, NULL);
        if (oldHwnd) {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                PostMessageW(oldHwnd, WM_HOTKEY, HOTKEY_TOGGLE_HIDE, 0);
            } else {
                PostMessageW(oldHwnd, WM_HOTKEY, HOTKEY_TOGGLE_MODE, 0);
            }
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassExW(&wc);

    s_hwnd = CreateWindowExW(0, CLASS_NAME, L"PinShift", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!s_hwnd) {
        CloseHandle(hMutex);
        return 0;
    }

    RegisterHotKey(s_hwnd, HOTKEY_TOGGLE_MODE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Y');
    RegisterHotKey(s_hwnd, HOTKEY_TOGGLE_HIDE, MOD_CONTROL | MOD_SHIFT | MOD_WIN | MOD_NOREPEAT, 'Y');

    HANDLE hRegEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!hRegEvent) {
        CloseHandle(hMutex);
        return 1;
    }

    HKEY hRegKey = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, NULL, 0, KEY_NOTIFY | KEY_QUERY_VALUE, NULL, &hRegKey, NULL) == ERROR_SUCCESS) {
        RegNotifyChangeKeyValue(hRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, hRegEvent, TRUE);
    }

    s_currentMode = GetIMEMode();
    if (!IsTrayHiddenConfig()) {
        s_trayVisible = true;
        ReloadIconResources(QueryTaskbarIconMetric());
        UpdateTrayIcon();
    }

    MSG msg = { 0 };
    bool running = true;
    while (running) {
        DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &hRegEvent, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (waitResult == WAIT_OBJECT_0) {
            if (hRegKey) {
                RegNotifyChangeKeyValue(hRegKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC, hRegEvent, TRUE);
            }
            DWORD newMode = GetIMEMode();
            if (newMode != s_currentMode) {
                s_currentMode = newMode;
                UpdateTrayIcon();
            }
        } 
        else if (waitResult == WAIT_FAILED) {
            running = false; 
        } 
        else {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    if (hRegKey) RegCloseKey(hRegKey);
    if (hRegEvent) CloseHandle(hRegEvent);
    CloseHandle(hMutex);

    return (int)msg.wParam;
}
