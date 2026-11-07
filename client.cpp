#include "common.h"

using namespace std;

const int MAX_RECONNECT_ATTEMPTS = 10;

// Resolve host:port and connect, trying each address returned.
static SOCKET connectTo(const char* host, const char* port, bool verbose) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    int rc = getaddrinfo(host, port, &hints, &results);
    if (rc != 0) {
        if (verbose) cerr << "Could not resolve " << host << ":" << port << " (error " << rc << ")\n";
        return INVALID_SOCKET;
    }

    SOCKET s = INVALID_SOCKET;
    int lastError = 0;
    for (addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) {
            lastError = WSAGetLastError();
            continue;
        }
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        lastError = WSAGetLastError();  // save before closesocket() resets it
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(results);

    if (s == INVALID_SOCKET && verbose) {
        cerr << "connect to " << host << ":" << port << " failed with error " << lastError
             << " (is the server running?)\n";
    }
    return s;
}

// Connect, verify the server's HELLO and register `name`. Returns a connected socket ready
// for chatting, or INVALID_SOCKET. `verbose` is false while retrying after a lost connection.
static SOCKET joinServer(const char* host, const char* port, const string& name,
                         bool verbose, string& welcome) {
    SOCKET s = connectTo(host, port, verbose);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // Make sure we reached the chat server: it must greet us within a few seconds.
    DWORD timeout = HELLO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    string hello;
    if (!recvMessage(s, hello) || hello != SERVER_HELLO) {
        if (verbose) {
            cerr << "Reached " << host << ":" << port << ", but the chat server did not answer.\n"
                 << "Is server.exe running there, or is another program using that port?\n";
        }
        closesocket(s);
        return INVALID_SOCKET;
    }

    // Tell the server who we are; it replies with a welcome (it may adjust the name).
    if (!sendMessage(s, name) || !recvMessage(s, welcome)) {
        if (verbose) cerr << "Server closed the connection during login.\n";
        closesocket(s);
        return INVALID_SOCKET;
    }
    timeout = 0;  // back to blocking forever for the chat itself
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

static int runClient(const char* host, const char* port, string name) {
    // Any name is fine; keep asking until something non-blank is typed.
    while (name.find_first_not_of(' ') == string::npos) {
        cout << "Enter your name: " << flush;
        if (!getline(cin, name)) return 1;
    }

    string welcome;
    SOCKET s = joinServer(host, port, name, true, welcome);
    if (s == INVALID_SOCKET) return 1;  // first connection must succeed

    while (true) {
        cout << "Connected to " << host << ":" << port << ". " << welcome << "\n";

        // The server labels every relayed message ("alice: hi"), so print them as received.
        int result = runChat(s, "Server", false);
        closesocket(s);
        if (result != CHAT_CONNECTION_LOST) return 0;  // we quit, or the server ended the chat

        // Connection dropped: retry with a growing delay (2s, 4s, 6s ... capped at 10s).
        s = INVALID_SOCKET;
        for (int attempt = 1; attempt <= MAX_RECONNECT_ATTEMPTS && s == INVALID_SOCKET; ++attempt) {
            DWORD delay = (attempt * 2000 < 10000) ? attempt * 2000 : 10000;
            cout << "Connection lost. Reconnecting in " << delay / 1000 << "s (attempt " << attempt
                 << "/" << MAX_RECONNECT_ATTEMPTS << ")..." << endl;
            Sleep(delay);
            s = joinServer(host, port, name, false, welcome);
        }
        if (s == INVALID_SOCKET) {
            cerr << "Could not reconnect. Giving up.\n";
            return 1;
        }
        cout << "Reconnected.\n";
    }
}

// Usage: client.exe [host] [port] [name]   (defaults: 127.0.0.1 8080; asks for a name if not given)
int main(int argc, char* argv[]) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    string defaultPort = to_string(DEFAULT_PORT);
    const char* port = argc > 2 ? argv[2] : defaultPort.c_str();
    string name = argc > 3 ? argv[3] : "";

    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        cerr << "WSAStartup failed with error " << rc << "\n";
        return 1;
    }

    int result = runClient(host, port, name);
    WSACleanup();
    return result;
}
