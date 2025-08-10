// lidlock.cpp
// Build (x86 or x64) with: cl /EHsc /MT /DUNICODE /D_UNICODE lidlock.cpp user32.lib shell32.lib advapi32.lib

#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <shlwapi.h> // PathAppend if needed
#include <stdint.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// Define GUID_LIDSWITCH_STATE_CHANGE if not defined in headers
// {BA3E0F4D-B817-4094-A2D1-D56379E6A0F3}
DEFINE_GUID(GUID_LIDSWITCH_STATE_CHANGE,
    0xBA3E0F4D, 0xB817, 0x4094, 0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3);

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 9001
#define ID_TRAY_SHOW 9002
#define AUTOSTART_REGPATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALNAME L"LidLockApp"

// Globals
HINSTANCE g_hInst = NULL;
HPOWERNOTIFY g_hPowerNotify = NULL;
NOTIFYICONDATAW g_nid = {};
HWND g_hwnd = NULL;

void AddSelfToStartup()
{
    WCHAR path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return;

    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_REGPATH, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, AUTOSTART_VALNAME, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

void RemoveSelfFromStartup()
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_REGPATH, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, AUTOSTART_VALNAME);
        RegCloseKey(hKey);
    }
}

// Create tray icon and menu
void CreateTrayIcon(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = (HICON)LoadIcon(NULL, IDI_APPLICATION);
    StringCchCopyW(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"LidLock (locks on lid close)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void DestroyTrayIcon()
{
    if (g_nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
}

// Show context menu on tray right-click
void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, L"Exit");
    // Add a toggle autostart (simple approach)
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_SHOW, L"Add to Startup");

    // Set foreground and track
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// Lock the workstation (runs only if process is on interactive desktop)
void DoLock()
{
    // As per Win32 docs LockWorkStation is asynchronous; check return for initiation
    BOOL ok = LockWorkStation();
    // We don't retry; but for debugging you could call GetLastError if ok==FALSE
    (void)ok;
}

// Power setting handler: looks for GUID_LIDSWITCH_STATE_CHANGE
void OnPowerSettingChange(PPOWERBROADCAST_SETTING pbs)
{
    if (!pbs) return;

    if (IsEqualGUID(pbs->PowerSetting, GUID_LIDSWITCH_STATE_CHANGE)) {
        // According to docs value is a DWORD: 0 = lid closed? 1/open? We'll interpret below:
        // Many reports: 0 = closed -> 1 = open (but this can vary by platform); We'll lock on transition to closed (0).
        if (pbs->DataLength >= sizeof(DWORD)) {
            DWORD state = *(DWORD*)(pbs->Data);
            // Guard: if state == 0 => lid closed (common), lock immediately
            if (state == 0) {
                DoLock();
            }
            else {
                // state == 1: lid opened; nothing to do
            }
        }
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
        // Register for lid change notifications
        g_hPowerNotify = RegisterPowerSettingNotification(hwnd, &GUID_LIDSWITCH_STATE_CHANGE, DEVICE_NOTIFY_WINDOW_HANDLE);
        break;
    case WM_DESTROY:
        if (g_hPowerNotify) UnregisterPowerSettingNotification(g_hPowerNotify);
        DestroyTrayIcon();
        PostQuitMessage(0);
        break;
    case WM_POWERBROADCAST:
        if (wParam == PBT_POWERSETTINGCHANGE) {
            PPOWERBROADCAST_SETTING pbs = (PPOWERBROADCAST_SETTING)lParam;
            OnPowerSettingChange(pbs);
        }
        return TRUE;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        case ID_TRAY_SHOW:
            // Toggle: attempt to add to startup
            AddSelfToStartup();
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    g_hInst = hInstance;

    // Register window class (invisible window)
    const wchar_t CLASS_NAME[] = L"LidLockHiddenWindowClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    // Create hidden window
    g_hwnd = CreateWindowExW(0, CLASS_NAME, L"LidLockHiddenWindow", 0, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);

    if (!g_hwnd) return 0;

    // Add tray icon
    CreateTrayIcon(g_hwnd);

    // Optionally: add to startup on first run (uncomment if desired)
    // AddSelfToStartup();

    // Message loop ¡ª minimal CPU usage
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
