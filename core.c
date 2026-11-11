#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef REG_NOTIFY_THREAD_AGNOSTIC
#define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif
#ifndef SPI_SETLOGICALDPIOVERRIDE
#define SPI_SETLOGICALDPIOVERRIDE 0x009F
#endif

#define REG_PATH_INPUT      L"SOFTWARE\\Microsoft\\InputMethod\\Settings\\CHS"
#define REG_VAL_INPUT       L"Enable Double Pinyin"
#define REG_PATH_RUN        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_PATH_APP        L"Software\\PinShift"
#define REG_VAL_RUN         L"PinShift"
#define REG_VAL_HIDE_TRAY   L"HideTrayIcon"

#define WM_TRAYICON         (WM_APP + 1)
#define HOTKEY_TOGGLE_MODE  1
#define HOTKEY_TOGGLE_HIDE  2
#define TIMER_DPI_SETTLE_ID 1
#define EXT_PATH_LEN        1024

static HWND  s_hwnd = NULL;
static HICON s_iconQuan = NULL;
static HICON s_iconShuang = NULL;
static int   s_iconSize = 0;
static bool  s_trayVisible = false;
static bool  s_helpDialogOpen = false;
static DWORD s_currentMode = 0;
static UINT  s_msgTaskbarCreated = 0;

static const wchar_t* const CLASS_NAME = L"PinShiftHotkeyWindow_Class";
static const wchar_t* const MUTEX_NAME = L"Local\\PinShiftUniqueMutexSecure";

static void UpdateTrayIcon(void);

static bool GetCurrentExePath(wchar_t* buffer, DWORD cchSize) {
    if (!buffer || cchSize == 0) return false;
    DWORD len = GetModuleFileNameW(NULL, buffer, cchSize);
    if (len == 0 || len >= cchSize) {
        buffer[0] = L'\0';
        return false;
    }
    return true;
}

static DWORD GetIMEMode(void) {
    HKEY hKey;
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_VAL_INPUT, NULL, &type, (LPBYTE)&mode, &size) != ERROR_SUCCESS || type != REG_DWORD) {
            mode = 0;
        }
        RegCloseKey(hKey);
    }
    return (mode != 0) ? 1 : 0;
}

static void NotifyActiveWindowIME(void) {
    HWND fg = GetForegroundWindow();
    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    DWORD_PTR result = 0;

    if (fg && fg != s_hwnd && fg != hTray) {
        SendMessageTimeoutW(
            fg, WM_SETTINGCHANGE, 0,
            (LPARAM)L"Control Panel\\Input Method",
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 50, &result
        );
    } else {
        SendMessageTimeoutW(
            HWND_BROADCAST, WM_SETTINGCHANGE, 0,
            (LPARAM)L"Control Panel\\Input Method",
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 30, &result
        );
    }
}

static void SetIMEMode(DWORD mode) {
    mode = (mode != 0) ? 1 : 0;
    if (s_currentMode == mode) return;

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        if (RegSetValueExW(hKey, REG_VAL_INPUT, 0, REG_DWORD, (const BYTE*)&mode, sizeof(mode)) == ERROR_SUCCESS) {
            s_currentMode = mode;
            if (s_trayVisible) UpdateTrayIcon();
            NotifyActiveWindowIME();
        }
        RegCloseKey(hKey);
    }
}

static void ToggleIMEMode(void) {
    SetIMEMode((s_currentMode == 1) ? 0 : 1);
}

static int QueryTaskbarIconMetric(void) {
    HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
    UINT dpi = 96;

    typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
    typedef int  (WINAPI *GetSystemMetricsForDpiProc)(int, UINT);

    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    GetDpiForWindowProc pGetDpi = hUser32 ? (GetDpiForWindowProc)GetProcAddress(hUser32, "GetDpiForWindow") : NULL;
    GetSystemMetricsForDpiProc pGetMetrics = hUser32 ? (GetSystemMetricsForDpiProc)GetProcAddress(hUser32, "GetSystemMetricsForDpi") : NULL;

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
    HMODULE hComCtl = LoadLibraryExW(L"comctl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

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

static void UpdateTrayIcon(void) {
    if (!s_trayVisible) return;

    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = s_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (s_currentMode == 1) ? s_iconShuang : s_iconQuan;
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), (s_currentMode == 1) ? L"PinShift: 双拼模式" : L"PinShift: 全拼模式");

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

static bool IsAutoStartEnabled(void) {
    wchar_t currentExe[EXT_PATH_LEN];
    if (!GetCurrentExePath(currentExe, EXT_PATH_LEN)) return false;

    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_RUN, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t regPath[EXT_PATH_LEN + 32] = { 0 };
        DWORD size = sizeof(regPath) - sizeof(wchar_t);
        DWORD type = 0;

        if (RegQueryValueExW(hKey, REG_VAL_RUN, NULL, &type, (LPBYTE)regPath, &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            regPath[size / sizeof(wchar_t)] = L'\0';

            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(regPath, &argc);
            if (argv) {
                if (argc > 0 && _wcsicmp(argv[0], currentExe) == 0) {
                    enabled = true;
                }
                LocalFree(argv);
            }
        }
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
            wchar_t exe[EXT_PATH_LEN];
            if (GetCurrentExePath(exe, EXT_PATH_LEN)) {
                wchar_t path[EXT_PATH_LEN + 32];
                swprintf_s(path, ARRAYSIZE(path), L"\"%s\" --autostart", exe);
                RegSetValueExW(
                    hKey, REG_VAL_RUN, 0, REG_SZ,
                    (const BYTE*)path,
                    (DWORD)((wcslen(path) + 1) * sizeof(wchar_t))
                );
            }
        }
        RegCloseKey(hKey);
    }
}

static bool IsTrayHiddenConfig(void) {
    HKEY hKey;
    DWORD val = 0;
    DWORD size = sizeof(val);
    DWORD type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_APP, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_VAL_HIDE_TRAY, NULL, &type, (LPBYTE)&val, &size) != ERROR_SUCCESS || type != REG_DWORD) {
            val = 0;
        }
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
    if (!hMenu) return;

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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == s_msgTaskbarCreated && s_msgTaskbarCreated != 0) {
        SetTimer(hwnd, TIMER_DPI_SETTLE_ID, 300, NULL);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam == TRUE) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        SetTimer(hwnd, TIMER_DPI_SETTLE_ID, 300, NULL);
        return 0;

    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA || wParam == SPI_SETLOGICALDPIOVERRIDE ||
			(!IS_INTRESOURCE(lParam) && _wcsicmp((const wchar_t*)lParam, L"WindowMetrics") == 0)) {
            SetTimer(hwnd, TIMER_DPI_SETTLE_ID, 300, NULL);
        }
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
            if (!s_helpDialogOpen) {
                s_helpDialogOpen = true;
                MessageBoxW(
                    hwnd,
                    L"【快捷操作指南】\n\n"
                    L"• 切换输入模式：Ctrl + Shift + Y\n"
                    L"• 显隐托盘图标：Ctrl + Shift + Win + Y\n"
                    L"• 显隐托盘图标：按住Shift + 双击程序",
                    L"PinShift", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND
                );
                s_helpDialogOpen = false;
            }
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32) {
        typedef BOOL (WINAPI *SetDefaultDllDirectoriesProc)(DWORD);
        SetDefaultDllDirectoriesProc setDllDirs =
            (SetDefaultDllDirectoriesProc)GetProcAddress(hKernel32, "SetDefaultDllDirectories");
        if (setDllDirs) setDllDirs(LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        typedef BOOL (WINAPI *SetDpiAwarenessProc)(DPI_AWARENESS_CONTEXT);
        SetDpiAwarenessProc setDpi = (SetDpiAwarenessProc)GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (setDpi) setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }

    HANDLE hMutex = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        bool isAutoStart = false;

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (_wcsicmp(argv[i], L"--autostart") == 0) {
                    isAutoStart = true;
                    break;
                }
            }
            LocalFree(argv);
        }

        if (!isAutoStart) {
            HWND oldHwnd = FindWindowW(CLASS_NAME, NULL);
            if (oldHwnd) {
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                    PostMessageW(oldHwnd, WM_HOTKEY, HOTKEY_TOGGLE_HIDE, 0);
                } else {
                    PostMessageW(oldHwnd, WM_HOTKEY, HOTKEY_TOGGLE_MODE, 0);
                }
            }
        }

        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    HICON hAppIconSm = (HICON)LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR
    );

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = hAppIconSm;
    RegisterClassExW(&wc);

    s_hwnd = CreateWindowExW(0, CLASS_NAME, L"PinShift", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!s_hwnd) {
        if (hAppIconSm) DestroyIcon(hAppIconSm);
        CloseHandle(hMutex);
        return 0;
    }

    if (hUser) {
        typedef BOOL (WINAPI *ChangeWindowMessageFilterExProc)(HWND, UINT, DWORD, PVOID);
        ChangeWindowMessageFilterExProc pChangeFilter =
            (ChangeWindowMessageFilterExProc)GetProcAddress(hUser, "ChangeWindowMessageFilterEx");
        if (pChangeFilter) {
            pChangeFilter(s_hwnd, WM_HOTKEY, MSGFLT_ALLOW, NULL);
        }
    }

    RegisterHotKey(s_hwnd, HOTKEY_TOGGLE_MODE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Y');
    RegisterHotKey(s_hwnd, HOTKEY_TOGGLE_HIDE, MOD_CONTROL | MOD_SHIFT | MOD_WIN | MOD_NOREPEAT, 'Y');

    HANDLE hRegEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!hRegEvent) {
        DestroyWindow(s_hwnd);
        if (hAppIconSm) DestroyIcon(hAppIconSm);
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
    if (hAppIconSm) DestroyIcon(hAppIconSm);
    CloseHandle(hMutex);

    return (int)msg.wParam;
}
