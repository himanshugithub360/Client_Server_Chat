// Shared networking helpers for the chat client and server.
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  // Vista+, needed for getaddrinfo in MinGW headers
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>  // _beginthreadex
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")  // MSVC; with g++ link using -lws2_32
#endif

const unsigned short DEFAULT_PORT = 8080;
const uint32_t MAX_MESSAGE_SIZE = 64 * 1024;  // 64 KB per message

// The server sends this as its first message so the client can tell it reached
// the chat server and not some other program that happens to use the port.
const char* const SERVER_HELLO = "CHAT/1 HELLO";
const DWORD HELLO_TIMEOUT_MS = 5000;

#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif

// Print which call failed along with the Winsock error code.
inline void printSocketError(const char* what) {
    std::cerr << what << " failed with error " << WSAGetLastError() << "\n";
}

// send() may transmit fewer bytes than asked, so keep going until everything is sent.
inline bool sendAll(SOCKET s, const char* data, int len) {
    while (len > 0) {
        int sent = send(s, data, len, 0);
        if (sent == SOCKET_ERROR) return false;
        data += sent;
        len -= sent;
    }
    return true;
}

// recv() may return fewer bytes than asked, so keep going until len bytes have arrived.
// Returns false if the peer closed the connection or an error occurred.
inline bool recvAll(SOCKET s, char* data, int len) {
    while (len > 0) {
        int got = recv(s, data, len, 0);
        if (got <= 0) return false;  // 0 = peer closed, SOCKET_ERROR = failure
        data += got;
        len -= got;
    }
    return true;
}

// Framing: every message is sent as a 4-byte big-endian length followed by the bytes.
// This keeps message boundaries intact even though TCP is a byte stream.
inline bool sendMessage(SOCKET s, const std::string& msg) {
    if (msg.size() > MAX_MESSAGE_SIZE) return false;
    uint32_t netLen = htonl(static_cast<uint32_t>(msg.size()));
    return sendAll(s, reinterpret_cast<const char*>(&netLen), sizeof(netLen)) &&
           sendAll(s, msg.data(), static_cast<int>(msg.size()));
}

inline bool recvMessage(SOCKET s, std::string& out) {
    uint32_t netLen = 0;
    if (!recvAll(s, reinterpret_cast<char*>(&netLen), sizeof(netLen))) return false;
    uint32_t len = ntohl(netLen);
    if (len > MAX_MESSAGE_SIZE) return false;  // refuse oversized/corrupt frames
    out.assign(len, '\0');
    return len == 0 || recvAll(s, &out[0], static_cast<int>(len));
}

// ---------------------------------------------------------------------------
// Full-duplex chat: a background thread receives and prints messages while the
// main thread reads the keyboard and sends. Either side can type at any time.
// Uses the Win32 thread API because older MinGW builds lack std::thread.
// ---------------------------------------------------------------------------

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

const char* const PROMPT = "You: ";

struct ChatSession {
    SOCKET sock;
    std::string peerName;
    CRITICAL_SECTION outLock;    // keeps output from the two threads from interleaving
    volatile LONG peerDone;      // set by the receiver when the peer leaves or disconnects
    volatile LONG localQuit;     // set by the main thread when we leave
    bool vtEnabled;              // console understands ANSI escape codes
    bool prefixMessages;         // show "peerName: msg"; false when the peer already labels messages
    CRITICAL_SECTION sendLock;   // the main thread and the receiver (PONG replies) both send
    volatile LONG endReason;     // CHAT_* result, written once by whichever thread notices first
};

// How runChat ended.
const int CHAT_USER_QUIT = 0;      // we typed "exit" (or input ended)
const int CHAT_SERVER_ENDED = 1;   // the peer sent "exit": a deliberate end, do not reconnect
const int CHAT_CONNECTION_LOST = 2; // socket error or heartbeat timeout: a reconnect is worthwhile

// Heartbeat: the server sends PING every HEARTBEAT_INTERVAL_MS and clients answer PONG.
// Either side that hears nothing for HEARTBEAT_TIMEOUT_MS treats the connection as dead.
// The leading 0x01 byte keeps these from ever matching something a user could type.
const char* const PING_MSG = "\x01PING";
const char* const PONG_MSG = "\x01PONG";
// Control frame listing who is online: "\x01USERS" followed by "\t<name>" for each user.
// Console clients ignore it; the web client shows it as a live user list.
const char* const USERS_MSG = "\x01USERS";
const DWORD HEARTBEAT_INTERVAL_MS = 5000;
const DWORD HEARTBEAT_TIMEOUT_MS = 15000;

inline bool lockedSend(ChatSession& cs, const std::string& msg) {
    EnterCriticalSection(&cs.sendLock);
    bool ok = sendMessage(cs.sock, msg);
    LeaveCriticalSection(&cs.sendLock);
    return ok;
}

// Record why the chat ended; only the first caller wins.
inline void setEndReason(ChatSession& cs, int reason) {
    InterlockedCompareExchange(&cs.endReason, reason, -1);
}

// Print a full line, first clearing whatever is on the current (prompt) line,
// then optionally redraw the prompt so the user knows they can keep typing.
inline void printLine(ChatSession& cs, const std::string& line, bool reprompt) {
    EnterCriticalSection(&cs.outLock);
    std::cout << (cs.vtEnabled ? "\r\x1b[K" : "\n") << line << "\n";
    if (reprompt) std::cout << PROMPT;
    std::cout << std::flush;
    LeaveCriticalSection(&cs.outLock);
}

inline void printPrompt(ChatSession& cs) {
    EnterCriticalSection(&cs.outLock);
    std::cout << PROMPT << std::flush;
    LeaveCriticalSection(&cs.outLock);
}

// Push an Enter key press into the console so a blocked getline() returns.
// Returns false when stdin is not a console (e.g. input is piped).
inline bool wakeConsoleInput() {
    INPUT_RECORD rec[2];
    ZeroMemory(rec, sizeof(rec));
    for (int i = 0; i < 2; ++i) {
        rec[i].EventType = KEY_EVENT;
        rec[i].Event.KeyEvent.bKeyDown = (i == 0);
        rec[i].Event.KeyEvent.wRepeatCount = 1;
        rec[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
        rec[i].Event.KeyEvent.uChar.AsciiChar = '\r';
    }
    DWORD written = 0;
    return WriteConsoleInputA(GetStdHandle(STD_INPUT_HANDLE), rec, 2, &written) && written == 2;
}

inline unsigned __stdcall receiverThread(void* arg) {
    ChatSession& cs = *static_cast<ChatSession*>(arg);
    std::string msg;
    DWORD lastHeard = GetTickCount();
    while (true) {
        // Poll instead of blocking in recv() so a silent (dead) peer is noticed and
        // so we can stop promptly when the user leaves.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(cs.sock, &readable);
        timeval tv = {0, 200 * 1000};
        int ready = select(0, &readable, nullptr, nullptr, &tv);
        if (InterlockedCompareExchange(&cs.localQuit, 0, 0)) return 0;  // we left; stay quiet

        bool ok = true;
        bool timedOut = false;
        if (ready == 0) {
            if (GetTickCount() - lastHeard < HEARTBEAT_TIMEOUT_MS) continue;
            ok = false;
            timedOut = true;
        } else if (ready == SOCKET_ERROR) {
            ok = false;
        } else {
            ok = recvMessage(cs.sock, msg);
            if (InterlockedCompareExchange(&cs.localQuit, 0, 0)) return 0;
            lastHeard = GetTickCount();
            if (ok && msg == PING_MSG) { lockedSend(cs, PONG_MSG); continue; }
            if (ok && msg == PONG_MSG) continue;
            if (ok && !msg.empty() && msg[0] == '\x01') continue;  // other control frames (USERS...)
        }

        if (!ok || msg == "exit") {
            setEndReason(cs, ok ? CHAT_SERVER_ENDED : CHAT_CONNECTION_LOST);
            InterlockedExchange(&cs.peerDone, 1);
            std::string why = ok ? " ended the chat."
                                 : (timedOut ? " stopped responding." : " disconnected.");
            bool woke = wakeConsoleInput();  // unblock the main thread's getline()
            printLine(cs, cs.peerName + why + (woke ? "" : " Press Enter to continue."), false);
            return 0;
        }
        printLine(cs, cs.prefixMessages ? cs.peerName + ": " + msg : msg, true);
    }
}

// Run an interactive chat over a connected socket until either side types
// "exit" or the connection drops. Does not close the socket. Returns a CHAT_* code.
inline int runChat(SOCKET sock, const std::string& peerName, bool prefixMessages = true) {
    ChatSession cs;
    cs.sock = sock;
    cs.peerName = peerName;
    cs.prefixMessages = prefixMessages;
    cs.peerDone = 0;
    cs.localQuit = 0;
    cs.endReason = -1;
    InitializeCriticalSection(&cs.outLock);
    InitializeCriticalSection(&cs.sendLock);

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    cs.vtEnabled = GetConsoleMode(out, &mode) &&
                   SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, receiverThread, &cs, 0, nullptr));
    if (thread == nullptr) {
        std::cerr << "Could not start receiver thread.\n";
        DeleteCriticalSection(&cs.outLock);
        DeleteCriticalSection(&cs.sendLock);
        return CHAT_USER_QUIT;
    }

    std::string msg;
    while (true) {
        printPrompt(cs);
        bool gotLine = static_cast<bool>(std::getline(std::cin, msg));
        if (InterlockedCompareExchange(&cs.peerDone, 0, 0)) break;  // peer already left
        if (!gotLine) msg = "exit";                                 // Ctrl+Z / end of input
        if (msg.empty()) continue;
        if (msg.size() > MAX_MESSAGE_SIZE) {
            printLine(cs, "Message too long (max 64 KB), not sent.", false);
            continue;
        }

        if (msg == "exit") InterlockedExchange(&cs.localQuit, 1);
        if (!lockedSend(cs, msg)) {
            if (!InterlockedCompareExchange(&cs.peerDone, 0, 0)) {
                setEndReason(cs, CHAT_CONNECTION_LOST);
                printLine(cs, "Send failed: connection lost.", false);
            }
            break;
        }
        if (msg == "exit") {
            setEndReason(cs, CHAT_USER_QUIT);
            break;
        }
    }

    // Stop the receiver; it polls, so it notices localQuit within ~200 ms.
    InterlockedExchange(&cs.localQuit, 1);
    shutdown(sock, SD_BOTH);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    DeleteCriticalSection(&cs.outLock);
    DeleteCriticalSection(&cs.sendLock);
    int reason = static_cast<int>(cs.endReason);
    return reason < 0 ? CHAT_USER_QUIT : reason;
}
