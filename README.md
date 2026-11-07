<div align="center">

# 💬 Client–Server Chat

**A multi-user chat room in C++ and Winsock, with a console client and a browser client.**

![C++](https://img.shields.io/badge/C%2B%2B-11-00599C?logo=cplusplus&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Windows-0078D6?logo=windows&logoColor=white)
![Python](https://img.shields.io/badge/bridge-Python%203.8%2B-3776AB?logo=python&logoColor=white)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)

</div>

A small but complete TCP chat system. One server hosts a room, and any number of people can join it
from the **console** (`client.exe`) or from a **web browser** (through a tiny bridge). Everyone ends up in the
same conversation.

```
 ┌────────────┐   TCP    ┌─────────────┐   TCP    ┌──────────────┐  WebSocket  ┌──────────┐
 │ client.exe │ ───────► │ server.exe  │ ◄─────── │  bridge.py   │ ◄────────── │ Browser  │
 └────────────┘          │ (chat room) │          └──────────────┘             └──────────┘
                         └─────────────┘
```

---

## Contents

[Features](#-features) · [Quick start](#-quick-start) · [Web client](#-web-client) ·
[Using the chat](#-using-the-chat) · [How it works](#-how-it-works) · [Project layout](#-project-layout) ·
[Security notes](#-security-notes) · [Roadmap](#-roadmap)

---

## ✨ Features

| | |
|---|---|
| 👥 **Multi-user room** | Many clients at once, one thread each. Messages are relayed to everyone else. |
| 🏷️ **Usernames** | Type any name you like. Duplicates get a suffix (`Maria_2`). `Server` is reserved. |
| 🕒 **Timestamps** | Chat lines are stamped, for example `[21:45] Maria: hello`. |
| ⌨️ **Commands** | `/users`, `/help`, `/quit`, answered privately to you. |
| ❤️ **Heartbeat** | The server pings every 5 s. Anything silent for 15 s is treated as dead and dropped. |
| 🔁 **Auto-reconnect** | A client that loses its connection retries with backoff and rejoins under the same name. |
| 🌐 **Browser client** | Responsive web UI with a live online list, light and dark themes, and phone support. |
| 🔒 **Safe port binding** | The server claims its port exclusively and clients verify a handshake, so another program on the same port can't silently hijack the chat. |
| 📦 **Zero dependencies** | C++ standard library plus Winsock, and a standard-library-only Python bridge. |

---

## 🚀 Quick start

### Requirements

- Windows 10 or 11
- A C++11 compiler: [MinGW-w64](https://www.mingw-w64.org/) `g++`, or MSVC
- Python 3.8+ (only for the web client)

### Build

**MinGW / g++**
```bash
g++ -std=c++11 -o server.exe server.cpp -lws2_32
g++ -std=c++11 -o client.exe client.cpp -lws2_32
```

**MSVC** (Developer Command Prompt)
```bat
cl /EHsc /std:c++14 server.cpp
cl /EHsc /std:c++14 client.cpp
```

### Run

**1. Start the server** (default port `8080`, or pass your own):
```bash
./server.exe 9090
```

**2. Join from one or more consoles:**
```bash
./client.exe 127.0.0.1 9090
```
The client asks for your name. You can also pass it directly: `./client.exe 127.0.0.1 9090 Maria`.

| Program | Usage |
|---|---|
| `server.exe` | `server.exe [port]`  (default `8080`) |
| `client.exe` | `client.exe [host] [port] [name]`  (defaults: `127.0.0.1`, `8080`, asks for a name) |

> **Port already in use?** Error `10048` means another program is on that port. Pick a different one, such as `9090`.

---

## 🌐 Web client

No install is needed for the people who join: they just open a web page. The bridge is plain Python with no `pip install`.

```bash
python web/bridge.py --chat-port 9090
```

Open **http://127.0.0.1:8000**, choose a name, and chat.

**Let others on your network join:**
```bash
python web/bridge.py --chat-port 9090 --bind 0.0.0.0
```
The bridge prints a link such as `http://192.168.1.20:8000` to share. You may need to allow Python through
Windows Firewall.

| Bridge option | Default | Meaning |
|---|---|---|
| `--chat-host` | `127.0.0.1` | where `server.exe` runs |
| `--chat-port` | `8080` | the port `server.exe` listens on |
| `--http-port` | `8000` | the port serving the web page |
| `--bind` | `127.0.0.1` | use `0.0.0.0` to accept other devices |

More detail is in [web/README.md](web/README.md).

---

## 🗨️ Using the chat

| Type | Result |
|---|---|
| any text | sent to everyone in the room |
| `/users` | who is online (only you see the reply) |
| `/help` | list of commands |
| `/quit` or `exit` | leave the chat |

The **server console** is part of the room too: whatever you type there is broadcast as `Server: ...`, and
typing `exit` shuts the room down for everyone.

Closing the server doesn't strand people. Clients retry automatically (2 s, 4 s, 6 s, up to 10 s between
attempts, 10 tries) and rejoin when the server comes back. A deliberate `exit` from either side does not
trigger a reconnect.

---

## 🔧 How it works

### Threads

- **Server:** the main thread reads the operator's console, an accept thread takes new connections and sends
  heartbeat pings, and every client gets its own handler thread. All sends happen under one lock, so two
  messages can never interleave on a socket.
- **Console client:** the main thread reads the keyboard, and a receiver thread prints incoming messages.
  Either side can talk at any time.
- Blocking calls are replaced by short `select()` polls (100 to 200 ms), so shutdown and dead-peer
  detection are reliable on Windows.

### Wire protocol

Every message is a **4-byte big-endian length** followed by that many bytes of UTF-8, so message boundaries
survive TCP's byte-stream nature. The maximum size is 64 KB.

```
 server → client   "CHAT/1 HELLO"                       handshake: you reached the chat server
 client → server   "<desired name>"
 server → client   "Welcome, <name>! Type /help ..."    the name may have been adjusted
        ...
 server → client   "[21:45] Maria: hello"               relayed chat line
 server → client   "\x01PING"      client → "\x01PONG"   heartbeat
 server → client   "\x01USERS\tMaria\tRaj"              live online list
 either → other    "exit"                               leave / end of chat
```

Control frames start with the byte `0x01`, so a user can never type one by accident.

### Web bridge

`web/bridge.py` accepts a browser WebSocket, performs the same handshake as a console client, answers
heartbeats on the browser's behalf, and converts frames to small JSON messages. The page itself is a single
`index.html` file with no build step.

---

## 📁 Project layout

```
.
├── common.h          shared helpers: framing, heartbeat, full-duplex chat loop
├── server.cpp        multi-client chatroom server
├── client.cpp        console client with auto-reconnect
├── server.exe        prebuilt binaries
├── client.exe
└── web/
    ├── bridge.py     HTTP + WebSocket bridge (Python, stdlib only)
    ├── index.html    browser UI
    └── README.md
```

---

## 🔐 Security notes

This is a learning project, so keep these limits in mind before exposing it:

- **No encryption.** Traffic is plain text, so use it on networks you trust.
- **No passwords.** Anyone who can reach the server or the web page can join under any free name.
- The bridge only listens on `127.0.0.1` unless you pass `--bind 0.0.0.0`, and it rejects WebSocket
  connections from other websites (origin check).
- The browser UI renders messages as text, never as HTML.

---

## 🗺️ Roadmap

Done: ~~multiple clients~~ · ~~usernames and timestamps~~ · ~~length-prefixed framing~~ · ~~full-duplex chat~~ ·
~~heartbeat and auto-reconnect~~ · ~~browser client~~

Ideas for next:

- [ ] Chat history: save messages to a log file and send the last N to new joiners
- [ ] Coloured console output for users and system messages
- [ ] File transfer on top of the existing framing
- [ ] Login with a username and password file
- [ ] TLS encryption (OpenSSL or Windows SChannel)
- [ ] Linux and macOS support (POSIX sockets, CMake)

---

## 🛠️ Troubleshooting

| Problem | Fix |
|---|---|
| `bind ... failed with error 10048` | The port is taken. Run `./server.exe 9090` (any free port). |
| `connect ... failed (is the server running?)` | Start `server.exe` first, and check the host and port. |
| `the chat server did not answer` | Something else is using that port. Use a different one. |
| Browser says it can't connect | Make sure `bridge.py` is running and `--chat-port` matches the server. |
| Others can't open the web link | Start the bridge with `--bind 0.0.0.0` and allow Python through the firewall. |
