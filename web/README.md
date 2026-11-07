# Web client

A browser UI for the chat room. Nobody has to install anything: they just open a web page.

```
browser  <--WebSocket-->  bridge.py  <--TCP-->  server.exe
```

`bridge.py` is a small standard-library Python program (Python 3.8+, no `pip install`).
It serves `index.html` and turns each browser connection into a normal chat client, so web
users and `client.exe` users share the same room.

## Run

1. Start the chat server: `server.exe 9090`
2. Start the bridge (from this folder): `python bridge.py --chat-port 9090`
3. Open <http://127.0.0.1:8000> and pick a name.

### Let other people join

```
python bridge.py --chat-port 9090 --bind 0.0.0.0
```

The bridge prints an address like `http://192.168.1.20:8000`. Anyone on the same network can open it.
You may need to allow Python through Windows Firewall (port 8000). Only do this on a network you trust:
there are no passwords, and anyone who can reach the page can join.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--chat-host` | `127.0.0.1` | where `server.exe` runs |
| `--chat-port` | `8080` | the port `server.exe` listens on |
| `--http-port` | `8000` | the port for the web page |
| `--bind` | `127.0.0.1` | `0.0.0.0` to accept other devices |

## Features

- Any name you like; duplicates get a suffix (`Maria_2`)
- Live "online" list, timestamps, coloured names, your own messages on the right
- `/users`, `/help`, `/quit`
- Automatic reconnect with backoff if the connection drops
- Light and dark theme, works on phones
