#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdio.h>
#include "resource.h"

#define REG_PATH_INPUT      L"SOFTWARE\\Microsoft\\InputMethod\\Settings\\CHS"
#define REG_VAL_INPUT       L"Enable Double Pinyin"
#define REG_PATH_RUN        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_VAL_RUN         L"PinshiftAutoStart"
#define REG_PATH_APP        L"Software\\Pinshift"
#define REG_VAL_HIDE_TRAY   L"HideTrayIcon"

#define WM_TRAYICON         (WM_USER + 100)
#define WM_USER_SYNC_UI     (WM_USER + 779)
#define WM_USER_TOGGLE_MODE (WM_USER + 777)
#define WM_USER_TOGGLE_HIDE (WM_USER + 778)

#define TIMER_DPI_PROBE_ID  1
#define HOTKEY_TOGGLE_MODE  1
#define HOTKEY_TOGGLE_HIDE  2

typedef struct {
    HWND    hwnd;
    HWND    cachedTrayHwnd;
    HICON   hIconQuan;
    HICON   hIconShuang;
    int     currentIconSize;
    bool    isTrayVisible;
    HANDLE  hRegThread;
    HANDLE  hStopEvent;
    DWORD   lastToggleTick;
    DWORD   lastToggleHideTick;
} AppState;

static AppState g_app = {0};
static const wchar_t* g_className = L"PinshiftHotkeyWindow_Unique_Class";
static const wchar_t* g_mutexName = L"Local\\PinshiftUniqueMutexSecure";

DWORD GetIMEMode(void) {
    HKEY hKey;
    DWORD mode = 0;
    DWORD size = sizeof(mode);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, REG_VAL_INPUT, NULL, NULL, (LPBYTE)&mode, &size);
        RegCloseKey(hKey);
    }
    return mode;
}

void AsyncRefreshActiveWindowIME(void) {
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

void ShellNotify(DWORD action) {
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = g_app.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    DWORD mode = GetIMEMode();
    if (mode == 1) {
        nid.hIcon = g_app.hIconShuang;
        wcscpy_s(nid.szTip, 128, L"Pinshift: 双拼模式");
    } else {
        nid.hIcon = g_app.hIconQuan;
        wcscpy_s(nid.szTip, 128, L"Pinshift: 全拼模式");
    }

    if (!Shell_NotifyIconW(action, &nid) && action == NIM_MODIFY) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
}

bool SetIMEMode(DWORD mode) {
    if (GetIMEMode() == mode) return false;

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, REG_VAL_INPUT, 0, REG_DWORD, (const BYTE*)&mode, sizeof(mode));
        RegCloseKey(hKey);

        if (g_app.isTrayVisible) ShellNotify(NIM_MODIFY);
        AsyncRefreshActiveWindowIME();
        return true;
    }
    return false;
}

void ToggleIMEMode(void) {
    DWORD now = GetTickCount();
    if (now - g_app.lastToggleTick < 200) return;
    g_app.lastToggleTick = now;

    DWORD target = (GetIMEMode() == 1) ? 0 : 1;
    SetIMEMode(target);
}

int GetTaskbarSmallIconSize(void) {
    UINT dpi = 0;

    if (g_app.cachedTrayHwnd) {
        typedef UINT (WINAPI *GetDpiForWindowFunc)(HWND);
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        GetDpiForWindowFunc pGetDpi = (GetDpiForWindowFunc)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pGetDpi) dpi = pGetDpi(g_app.cachedTrayHwnd);
        if (dpi == 0) g_app.cachedTrayHwnd = NULL;
    }

    if (dpi == 0) {
        g_app.cachedTrayHwnd = FindWindowW(L"Shell_TrayWnd", NULL);
        if (g_app.cachedTrayHwnd) {
            typedef UINT (WINAPI *GetDpiForWindowFunc)(HWND);
            HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
            GetDpiForWindowFunc pGetDpi = (GetDpiForWindowFunc)GetProcAddress(hUser32, "GetDpiForWindow");
            if (pGetDpi) dpi = pGetDpi(g_app.cachedTrayHwnd);
        }
    }

    if (dpi == 0) {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
        }
    }

    if (dpi == 0) dpi = 96;

    typedef int (WINAPI *GetSystemMetricsForDpiFunc)(int, UINT);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    GetSystemMetricsForDpiFunc pGetMetrics = (GetSystemMetricsForDpiFunc)GetProcAddress(hUser32, "GetSystemMetricsForDpi");
    if (pGetMetrics) {
        int sz = pGetMetrics(SM_CXSMICON, dpi);
        if (sz > 0) return sz;
    }

    return (int)(16 * dpi / 96);
}

HICON LoadIconScaled(int resID, int size) {
    HICON hIco = (HICON)LoadImageW(
        GetModuleHandleW(NULL), MAKEINTRESOURCEW(resID),
        IMAGE_ICON, size, size, LR_DEFAULTCOLOR
    );
    if (!hIco) {
        hIco = (HICON)LoadImageW(
            GetModuleHandleW(NULL), MAKEINTRESOURCEW(resID),
            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR
        );
    }
    return hIco;
}

void ReloadIcons(int targetSize) {
    if (targetSize <= 0) targetSize = 16;
    if (g_app.currentIconSize == targetSize && g_app.hIconQuan && g_app.hIconShuang) {
        return;
    }

    HICON newQuan = LoadIconScaled(IDI_ICON_QUAN, targetSize);
    HICON newShuang = LoadIconScaled(IDI_ICON_SHUANG, targetSize);

    if (!newQuan || !newShuang) {
        if (newQuan) DestroyIcon(newQuan);
        if (newShuang) DestroyIcon(newShuang);
        return;
    }

    if (g_app.hIconQuan) DestroyIcon(g_app.hIconQuan);
    if (g_app.hIconShuang) DestroyIcon(g_app.hIconShuang);

    g_app.hIconQuan = newQuan;
    g_app.hIconShuang = newShuang;
    g_app.currentIconSize = targetSize;
}

bool IsAutoStart(void) {
    HKEY hKey;
    bool enabled = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_RUN, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, REG_VAL_RUN, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            enabled = true;
        }
        RegCloseKey(hKey);
    }
    return enabled;
}

void ToggleAutoStart(void) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_RUN, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (IsAutoStart()) {
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

bool IsTrayHidden(void) {
    HKEY hKey;
    DWORD val = 0;
    DWORD size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_APP, 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, REG_VAL_HIDE_TRAY, NULL, NULL, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return val == 1;
}

void SetTrayHidden(bool hide) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_APP, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = hide ? 1 : 0;
        RegSetValueExW(hKey, REG_VAL_HIDE_TRAY, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
}

void ToggleTrayVisibility(void) {
    DWORD now = GetTickCount();
    if (now - g_app.lastToggleHideTick < 500) return;
    g_app.lastToggleHideTick = now;

    bool hidden = !IsTrayHidden();
    SetTrayHidden(hidden);

    if (hidden) {
        ShellNotify(NIM_DELETE);
        g_app.isTrayVisible = false;
    } else {
        ShellNotify(NIM_ADD);
        g_app.isTrayVisible = true;
    }
}

void ShowTrayMenu(void) {
    SetForegroundWindow(g_app.hwnd);
    HMENU hMenu = CreatePopupMenu();

    DWORD mode = GetIMEMode();
    UINT quanFlag = MF_STRING | ((mode == 0) ? MF_CHECKED : 0);
    UINT shuangFlag = MF_STRING | ((mode == 1) ? MF_CHECKED : 0);
    UINT autoFlag = MF_STRING | (IsAutoStart() ? MF_CHECKED : 0);

    AppendMenuW(hMenu, quanFlag, IDM_FULL_PINYIN, L"全拼输入");
    AppendMenuW(hMenu, shuangFlag, IDM_DOUBLE_PINYIN, L"双拼输入");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, autoFlag, IDM_AUTO_START, L"开机启动");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_HELP, L"快捷键说明");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"退出程序");

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, NULL);
    PostMessageW(g_app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

DWORD WINAPI WatchRegistryThread(LPVOID param) {
    (void)param;
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_INPUT, 0, NULL, 0, KEY_NOTIFY | KEY_QUERY_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return 0;
    }

    HANDLE hNotifyEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE handles[2] = { hNotifyEvent, g_app.hStopEvent };

    #define REG_NOTIFY_THREAD_AGNOSTIC 0x10000000
    DWORD filter = REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC;
    if (RegNotifyChangeKeyValue(hKey, FALSE, filter, hNotifyEvent, TRUE) != ERROR_SUCCESS) {
        filter &= ~REG_NOTIFY_THREAD_AGNOSTIC;
        RegNotifyChangeKeyValue(hKey, FALSE, filter, hNotifyEvent, TRUE);
    }

    DWORD lastMode = GetIMEMode();
    DWORD waitTimeout = INFINITE;
    bool hasPendingUpdate = false;

    while (true) {
        DWORD event = WaitForMultipleObjects(2, handles, FALSE, waitTimeout);
        if (event == WAIT_OBJECT_0 + 1) break;

        if (event == WAIT_OBJECT_0) {
            RegNotifyChangeKeyValue(hKey, FALSE, filter, hNotifyEvent, TRUE);
            waitTimeout = 150;
            hasPendingUpdate = true;
        } else if (event == WAIT_TIMEOUT) {
            waitTimeout = INFINITE;
            if (hasPendingUpdate) {
                hasPendingUpdate = false;
                DWORD currentMode = GetIMEMode();
                if (currentMode != lastMode) {
                    lastMode = currentMode;
                    PostMessageW(g_app.hwnd, WM_USER_SYNC_UI, 0, 0);
                }
            }
        }
    }

    CloseHandle(hNotifyEvent);
    RegCloseKey(hKey);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static UINT s_taskbarCreatedMsg = 0;

    if (msg == s_taskbarCreatedMsg && s_taskbarCreatedMsg != 0) {
        if (!IsTrayHidden()) {
            ReloadIcons(GetTaskbarSmallIconSize());
            ShellNotify(NIM_ADD);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        s_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_DPI_PROBE_ID && g_app.isTrayVisible) {
            int newSize = GetTaskbarSmallIconSize();
            if (newSize != g_app.currentIconSize) {
                ReloadIcons(newSize);
                ShellNotify(NIM_MODIFY);
            }
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (g_app.isTrayVisible) {
            int newSize = GetTaskbarSmallIconSize();
            if (newSize != g_app.currentIconSize) {
                ReloadIcons(newSize);
                ShellNotify(NIM_MODIFY);
            }
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == HOTKEY_TOGGLE_MODE) ToggleIMEMode();
        else if (wParam == HOTKEY_TOGGLE_HIDE) ToggleTrayVisibility();
        return 0;

    case WM_USER_SYNC_UI:
        if (g_app.isTrayVisible) ShellNotify(NIM_MODIFY);
        return 0;

    case WM_USER_TOGGLE_MODE:
        ToggleIMEMode();
        return 0;

    case WM_USER_TOGGLE_HIDE:
        ToggleTrayVisibility();
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ToggleIMEMode();
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FULL_PINYIN:   SetIMEMode(0); break;
        case IDM_DOUBLE_PINYIN: SetIMEMode(1); break;
        case IDM_AUTO_START:    ToggleAutoStart(); break;
        case IDM_HELP:
            MessageBoxW(
                hwnd,
                L"【快捷键说明】\n\n"
                L"Shift+Ctrl+Y：切换全拼/双拼\n"
                L"Shift+Ctrl+Win+Y：显示/隐藏托盘图标\n"
                L"Shift+双击程序：显示/隐藏托盘图标",
                L"Pinshift", MB_OK | MB_ICONINFORMATION
            );
            break;
        case IDM_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, TIMER_DPI_PROBE_ID);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_MODE);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_HIDE);
        if (g_app.isTrayVisible) ShellNotify(NIM_DELETE);

        SetEvent(g_app.hStopEvent);
        WaitForSingleObject(g_app.hRegThread, 1000);
        CloseHandle(g_app.hRegThread);
        CloseHandle(g_app.hStopEvent);

        if (g_app.hIconQuan) DestroyIcon(g_app.hIconQuan);
        if (g_app.hIconShuang) DestroyIcon(g_app.hIconShuang);

        DestroyWindow(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    HANDLE hMutex = CreateMutexW(NULL, FALSE, g_mutexName);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND oldHwnd = FindWindowW(g_className, NULL);
        if (oldHwnd) {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
                PostMessageW(oldHwnd, WM_USER_TOGGLE_HIDE, 0, 0);
            } else {
                PostMessageW(oldHwnd, WM_USER_TOGGLE_MODE, 0, 0);
            }
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = g_className;
    RegisterClassExW(&wc);

    g_app.hwnd = CreateWindowExW(
        0, g_className, L"PinshiftHotkey",
        0, 0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (!g_app.hwnd) {
        CloseHandle(hMutex);
        return 0;
    }

    g_app.hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_app.hRegThread = CreateThread(NULL, 0, WatchRegistryThread, NULL, 0, NULL);

    RegisterHotKey(g_app.hwnd, HOTKEY_TOGGLE_MODE, MOD_CONTROL | MOD_SHIFT, 'Y');
    RegisterHotKey(g_app.hwnd, HOTKEY_TOGGLE_HIDE, MOD_CONTROL | MOD_SHIFT | MOD_WIN, 'Y');

    SetTimer(g_app.hwnd, TIMER_DPI_PROBE_ID, 1500, NULL);

    ReloadIcons(GetTaskbarSmallIconSize());
    if (!IsTrayHidden()) {
        g_app.isTrayVisible = true;
        ShellNotify(NIM_ADD);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(hMutex);
    return (int)msg.wParam;
}
