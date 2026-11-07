#include "common.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <vector>

using namespace std;

// Shared state for the chatroom. All access to `clients` and console output
// goes through `lock`; sends are made while holding it so that two threads can
// never interleave the bytes of two framed messages on the same socket.
struct Room {
    SOCKET listener;
    CRITICAL_SECTION lock;
    vector<SOCKET> clients;
    vector<string> names;    // parallel to `clients`
    int nextId;
    volatile LONG active;    // number of running client-handler threads
    volatile LONG stopping;  // set when the operator types "exit"
};

struct ClientArg {
    Room* room;
    SOCKET sock;
    int id;
};

static void logLine(Room& room, const string& line) {
    EnterCriticalSection(&room.lock);
    cout << line << endl;
    LeaveCriticalSection(&room.lock);
}

// Send to every connected client except `except` (pass INVALID_SOCKET for all).
static void broadcast(Room& room, const string& msg, SOCKET except) {
    EnterCriticalSection(&room.lock);
    for (SOCKET c : room.clients) {
        if (c != except) sendMessage(c, msg);  // a dead peer is cleaned up by its own handler
    }
    LeaveCriticalSection(&room.lock);
}

// Block until `s` has data (or the peer closed), polling so a shutdown is noticed within
// ~100 ms. shutdown()/closesocket() from another thread can't be relied on to wake a
// blocked recv() on Windows. Returns false on shutdown, socket error, or when nothing
// (not even a heartbeat reply) has arrived since `lastSeen` for HEARTBEAT_TIMEOUT_MS.
static bool waitReadable(Room& room, SOCKET s, DWORD lastSeen) {
    while (!InterlockedCompareExchange(&room.stopping, 0, 0)) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval tv = {0, 100 * 1000};
        int ready = select(0, &readable, nullptr, nullptr, &tv);
        if (ready > 0) return true;
        if (ready == SOCKET_ERROR) return false;
        if (GetTickCount() - lastSeen >= HEARTBEAT_TIMEOUT_MS) return false;
    }
    return false;
}

// Trim, drop control characters, cap at 20 chars; fall back to "Client N" if nothing is left.
static string cleanName(const string& raw, int id) {
    string out;
    for (char ch : raw) {
        if (static_cast<unsigned char>(ch) >= 32 && ch != 127) out += ch;
    }
    size_t b = out.find_first_not_of(' ');
    size_t e = out.find_last_not_of(' ');
    out = (b == string::npos) ? "" : out.substr(b, e - b + 1);
    if (out.size() > 20) out.resize(20);
    return out.empty() ? "Client " + to_string(id) : out;
}

static bool sameName(const string& a, const string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Must be called with room.lock held. "Server" is reserved for the operator console.
static string uniqueName(Room& room, const string& wanted) {
    string candidate = wanted;
    for (int n = 2;; ++n) {
        bool taken = sameName(candidate, "Server");
        for (const string& used : room.names) taken = taken || sameName(candidate, used);
        if (!taken) return candidate;
        candidate = wanted + "_" + to_string(n);
    }
}

static string timestamp() {
    time_t now = time(nullptr);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", localtime(&now));
    return buf;
}

// Send to a single client; holds the lock so frames from different threads can't interleave.
static void sendTo(Room& room, SOCKET s, const string& msg) {
    EnterCriticalSection(&room.lock);
    sendMessage(s, msg);
    LeaveCriticalSection(&room.lock);
}

static string commandReply(Room& room, const string& cmd) {
    if (cmd == "/help") {
        return "Commands: /help  show this help | /users  list who is online | /quit  leave the chat";
    }
    if (cmd == "/users") {
        EnterCriticalSection(&room.lock);
        string out = "Online (" + to_string(room.names.size()) + "):";
        for (const string& n : room.names) out += " " + n;
        LeaveCriticalSection(&room.lock);
        return out;
    }
    return "Unknown command '" + cmd + "'. Type /help for the list.";
}

static unsigned __stdcall clientThread(void* param) {
    ClientArg arg = *static_cast<ClientArg*>(param);
    delete static_cast<ClientArg*>(param);
    Room& room = *arg.room;
    string name;
    string msg;
    DWORD lastSeen = GetTickCount();  // last time this client sent us anything, PONGs included

    // Handshake: we send HELLO, the client answers with its desired username.
    if (sendMessage(arg.sock, SERVER_HELLO) && waitReadable(room, arg.sock, lastSeen) &&
        recvMessage(arg.sock, msg)) {
        lastSeen = GetTickCount();
        // Register the name and the socket together so two clients can't claim one name.
        EnterCriticalSection(&room.lock);
        name = uniqueName(room, cleanName(msg, arg.id));
        room.clients.push_back(arg.sock);
        room.names.push_back(name);
        LeaveCriticalSection(&room.lock);

        sendMessage(arg.sock, "Welcome, " + name + "! Type /help for commands, 'exit' to leave.");
        logLine(room, name + " joined");
        broadcast(room, name + " joined the chat", arg.sock);

        while (waitReadable(room, arg.sock, lastSeen) && recvMessage(arg.sock, msg) && msg != "exit") {
            lastSeen = GetTickCount();
            if (msg == PONG_MSG) continue;  // heartbeat reply, nothing to relay
            if (!msg.empty() && msg[0] == '/') {  // chat command, answered to the sender only
                if (msg == "/quit") break;
                sendTo(room, arg.sock, commandReply(room, msg));
                continue;
            }
            string line = "[" + timestamp() + "] " + name + ": " + msg;
            logLine(room, line);
            broadcast(room, line, arg.sock);
        }

        EnterCriticalSection(&room.lock);
        for (size_t i = 0; i < room.clients.size(); ++i) {
            if (room.clients[i] == arg.sock) {
                room.clients.erase(room.clients.begin() + i);
                room.names.erase(room.names.begin() + i);
                break;
            }
        }
        LeaveCriticalSection(&room.lock);

        if (!InterlockedCompareExchange(&room.stopping, 0, 0)) {
            bool timedOut = GetTickCount() - lastSeen >= HEARTBEAT_TIMEOUT_MS;
            string how = timedOut ? " timed out" : " left";
            logLine(room, name + how);
            broadcast(room, name + how + (timedOut ? "" : " the chat"), INVALID_SOCKET);
        }
    }

    closesocket(arg.sock);
    InterlockedDecrement(&room.active);
    return 0;
}

// Accepts connections until `stopping` is set at shutdown.
static unsigned __stdcall acceptThread(void* param) {
    Room& room = *static_cast<Room*>(param);
    // closesocket() on a listening socket does not reliably wake a blocked accept()
    // on Windows, so poll with a short select() timeout and re-check `stopping`.
    DWORD lastPing = GetTickCount();
    while (!InterlockedCompareExchange(&room.stopping, 0, 0)) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(room.listener, &readable);
        timeval tv = {0, 100 * 1000};  // 100 ms
        int ready = select(0, &readable, nullptr, nullptr, &tv);
        if (GetTickCount() - lastPing >= HEARTBEAT_INTERVAL_MS) {  // doubles as the heartbeat timer
            broadcast(room, PING_MSG, INVALID_SOCKET);
            lastPing = GetTickCount();
        }
        if (ready == 0) continue;
        if (ready == SOCKET_ERROR) {
            if (InterlockedCompareExchange(&room.stopping, 0, 0)) return 0;
            printSocketError("select");
            Sleep(100);
            continue;
        }
        SOCKET c = accept(room.listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            printSocketError("accept");
            continue;
        }
        ClientArg* arg = new ClientArg{&room, c, ++room.nextId};
        InterlockedIncrement(&room.active);
        HANDLE h = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, clientThread, arg, 0, nullptr));
        if (h == nullptr) {
            cerr << "Could not start thread for new client.\n";
            InterlockedDecrement(&room.active);
            closesocket(c);
            delete arg;
        } else {
            CloseHandle(h);  // the handler thread cleans up after itself
        }
    }
    return 0;
}

// Usage: server.exe [port]
static int runServer(unsigned short port) {
    Room room;
    room.nextId = 0;
    room.active = 0;
    room.stopping = 0;
    InitializeCriticalSection(&room.lock);

    room.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (room.listener == INVALID_SOCKET) {
        printSocketError("socket");
        return 1;
    }

    // Claim the port exclusively. Without this, Windows lets another program bind
    // 127.0.0.1 on the same port and silently steal local connections.
    BOOL exclusive = TRUE;
    if (setsockopt(room.listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
        printSocketError("setsockopt(SO_EXCLUSIVEADDRUSE)");
        closesocket(room.listener);
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(room.listener, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        printSocketError("bind (is the port already in use?)");
        closesocket(room.listener);
        return 1;
    }

    if (listen(room.listener, SOMAXCONN) == SOCKET_ERROR) {
        printSocketError("listen");
        closesocket(room.listener);
        return 1;
    }
    cout << "Chatroom listening on port " << port
         << ". Type a message to broadcast it, or 'exit' to shut down.\n";

    HANDLE acceptor = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, acceptThread, &room, 0, nullptr));
    if (acceptor == nullptr) {
        cerr << "Could not start accept thread.\n";
        closesocket(room.listener);
        return 1;
    }

    // Operator console: whatever is typed here goes to everyone as "Server: ...".
    string line;
    while (getline(cin, line) && line != "exit") {
        if (line.empty()) continue;
        if (line.size() > MAX_MESSAGE_SIZE) {
            logLine(room, "Message too long (max 64 KB), not sent.");
            continue;
        }
        broadcast(room, "[" + timestamp() + "] Server: " + line, INVALID_SOCKET);
    }

    // Shut down: stop accepting, tell clients, wait for every handler to notice `stopping`.
    InterlockedExchange(&room.stopping, 1);
    WaitForSingleObject(acceptor, INFINITE);  // exits within ~100 ms
    CloseHandle(acceptor);
    closesocket(room.listener);

    broadcast(room, "exit", INVALID_SOCKET);
    while (InterlockedCompareExchange(&room.active, 0, 0) > 0) Sleep(10);

    DeleteCriticalSection(&room.lock);
    return 0;
}

int main(int argc, char* argv[]) {
    unsigned short port = DEFAULT_PORT;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p <= 0 || p > 65535) {
            cerr << "Invalid port: " << argv[1] << "\n";
            return 1;
        }
        port = static_cast<unsigned short>(p);
    }

    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        cerr << "WSAStartup failed with error " << rc << "\n";
        return 1;
    }

    int result = runServer(port);
    WSACleanup();
    return result;
}
