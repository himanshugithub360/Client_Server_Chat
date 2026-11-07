#include "common.h"

using namespace std;

// Resolve host:port and connect, trying each address returned.
static SOCKET connectTo(const char* host, const char* port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    int rc = getaddrinfo(host, port, &hints, &results);
    if (rc != 0) {
        cerr << "Could not resolve " << host << ":" << port << " (error " << rc << ")\n";
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

    if (s == INVALID_SOCKET) {
        cerr << "connect to " << host << ":" << port << " failed with error " << lastError
             << " (is the server running?)\n";
    }
    return s;
}

static int runClient(const char* host, const char* port, string name) {
    SOCKET clientSocket = connectTo(host, port);
    if (clientSocket == INVALID_SOCKET) return 1;

    // Make sure we reached the chat server: it must greet us within a few seconds.
    DWORD timeout = HELLO_TIMEOUT_MS;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    string hello;
    if (!recvMessage(clientSocket, hello) || hello != SERVER_HELLO) {
        cerr << "Reached " << host << ":" << port << ", but the chat server did not answer.\n"
             << "Is server.exe running there, or is another program using that port?\n";
        closesocket(clientSocket);
        return 1;
    }

    // Tell the server who we are; it replies with a welcome (it may adjust the name).
    // Any name is fine; keep asking until something non-blank is typed.
    while (name.find_first_not_of(' ') == string::npos) {
        cout << "Enter your name: " << flush;
        if (!getline(cin, name)) {
            closesocket(clientSocket);
            return 1;
        }
    }
    if (!sendMessage(clientSocket, name)) {
        printSocketError("send name");
        closesocket(clientSocket);
        return 1;
    }
    string welcome;
    if (!recvMessage(clientSocket, welcome)) {
        cerr << "Server closed the connection.\n";
        closesocket(clientSocket);
        return 1;
    }
    timeout = 0;  // back to blocking forever for the chat itself
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    cout << "Connected to " << host << ":" << port << ". " << welcome << "\n";

    // The server labels every relayed message ("alice: hi"), so print them as received.
    runChat(clientSocket, "Server", false);
    closesocket(clientSocket);
    return 0;
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
