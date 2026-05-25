# Multithreaded Reverse Port Forwarder

A multithreaded reverse port forwarder written in C++ that works like SSH reverse port forwarding (`ssh -R`). It has two modes — **server** and **client** — that communicate over a custom text-based control protocol. Both Windows and Linux implementations are provided.

## Files

| File | Description |
|------|-------------|
| `windows/ReversePortForward/ReversePortForward.cpp` | Windows implementation (Winsock2, Win32 threads) |
| `linux/reverse_port_forward_linux.cpp` | Linux implementation (POSIX sockets, pthreads) |
| `*server_settings.txt` | Server mode configuration |
| `*client_settings.txt` | Client mode configuration with forwarding rules |

## How It Works

The reverse port forwarder allows a client behind a firewall/NAT to expose local services through a publicly accessible server — exactly like `ssh -R`.

```
                         Internet / LAN
                              │
   External User ──►  Server :8080  ◄──── Control Channel ────  Client
                         │                                        │
                         │   CONNECT 8080 42                      │
                         │ ─────────────────────────────────────► │
                         │                                        │
                         │   DATA 42 (new TCP connection)         │
                         │ ◄───────────────────────────────────── │
                         │                                        │
                    ┌────┴────┐                              ┌────┴────┐
                    │ Relay   │◄────── bidirectional ───────►│ Relay   │
                    │ Thread  │         data flow            │ Thread  │
                    └────┬────┘                              └────┬────┘
                         │                                        │
                    External User                          Local Service
                                                          (127.0.0.1:80)
```

### SSH Equivalent

```bash
# SSH reverse port forward:
ssh -R 8080:127.0.0.1:80 -R 2222:127.0.0.1:22 user@server

# Equivalent with this tool:
# On server:  ReversePortForward.exe server_settings.txt
# On client:  ReversePortForward.exe client_settings.txt
```

## Control Protocol

The server and client communicate over a text-based TCP protocol:

### Connection Types

Every TCP connection to the server's control port starts with a single identifying line:

| First Line | Purpose |
|------------|---------|
| `CONTROL\n` | Identifies a control connection (long-lived) |
| `DATA <session_id>\n` | Identifies a data channel for a specific tunnel session |

### Control Channel Messages

```
Phase 1: Setup
  Client → Server:  CONTROL
  Client → Server:  FORWARD 8080 127.0.0.1 80
  Client → Server:  FORWARD 2222 127.0.0.1 22
  Client → Server:  READY
  Server → Client:  OK 8080
  Server → Client:  OK 2222

Phase 2: Tunneling (repeats for each incoming connection)
  Server → Client:  CONNECT 8080 42        (someone connected to server:8080)
  Client opens new TCP to server, sends:  DATA 42
  Server pairs the DATA socket with the incoming socket
  Bidirectional relay begins

Phase 2: Error handling
  Server → Client:  ERROR 8080 bind_failed (if server can't bind the port)
```

### Message Reference

| Direction | Message | Description |
|-----------|---------|-------------|
| C → S | `CONTROL` | First line — identifies control connection |
| C → S | `FORWARD <rport> <lhost> <lport>` | Request server to listen on `<rport>`, forward to client's `<lhost>:<lport>` |
| C → S | `READY` | All rules sent, begin listening |
| S → C | `OK <rport>` | Server successfully bound and listening on `<rport>` |
| S → C | `ERROR <rport> <reason>` | Server failed to set up forwarding for `<rport>` |
| S → C | `CONNECT <rport> <session_id>` | New incoming connection on `<rport>`, client should open data channel |
| C → S | `DATA <session_id>` | First line on a new TCP connection — claims a pending session |

## Detailed Flow

### Startup Sequence

```
1. Server starts, listens on control port (e.g., 9000)
2. Client connects to server:9000
3. Client sends: CONTROL\n
4. Client sends: FORWARD 8080 127.0.0.1 80\n
5. Client sends: FORWARD 2222 127.0.0.1 22\n
6. Client sends: READY\n
7. Server spawns a listener thread for each FORWARD rule
8. Server binds :8080 and :2222
9. Server sends: OK 8080\n   OK 2222\n
10. Control channel stays open (heartbeat/disconnect detection)
```

### Tunnel Establishment

```
1. External user connects to server:8080
2. Server assigns session_id=42, stores the incoming socket
3. Server sends on control channel: CONNECT 8080 42\n
4. Client opens a NEW TCP connection to server:9000
5. Client sends: DATA 42\n
6. Server matches session_id=42, pairs the two sockets
7. Server spawns a relay thread: incoming_socket ↔ data_socket
8. Client connects to 127.0.0.1:80 (the local service)
9. Client spawns a relay thread: data_socket ↔ local_socket
10. Bidirectional data flows: external_user ↔ server ↔ client ↔ local_service
```

### Auto-Reconnect (Client)

When the server disconnects or becomes unreachable, the client automatically reconnects:

```
1. Control channel recv() returns 0 (server gone)
2. run_client() returns
3. Main loop waits reconnect_interval seconds (default: 5)
4. run_client() called again — reconnects and re-registers all rules
5. Repeat indefinitely
```

## Configuration

### Server Settings File

```ini
# Reverse Port Forwarder Settings — SERVER mode

mode=server

# Control port where clients connect to register forwarding rules
listen_address=0.0.0.0
listen_port=9000

# Global settings
max_threads=200
buffer_size=65536
connect_timeout=10
recv_timeout=60
logging=1
log_file=rpf_server.log
```

### Client Settings File

```ini
# Reverse Port Forwarder Settings — CLIENT mode

mode=client

# Server to connect to (the reverse port forward server)
server_address=192.168.56.101
server_port=9000

# Global settings
max_threads=200
buffer_size=65536
connect_timeout=10
recv_timeout=60
reconnect_interval=5
logging=1
log_file=rpf_client.log

# Forwarding rules:
#   RemotePort:LocalAddress:LocalPort
#
# "Ask the server to listen on RemotePort, and when
# someone connects there, forward traffic to LocalAddress:LocalPort
# which is reachable from THIS client machine."
#
# Equivalent to: ssh -R RemotePort:LocalAddress:LocalPort user@server

[rules]
8080:127.0.0.1:80
2222:127.0.0.1:22
9443:127.0.0.1:443
```

### Settings Reference

#### Server-only Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `mode` | — | Must be `server` |
| `listen_address` | `0.0.0.0` | Bind address for the control port |
| `listen_port` | `9000` | Control port where clients connect |

#### Client-only Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `mode` | — | Must be `client` |
| `server_address` | — | Server IP or hostname to connect to |
| `server_port` | `9000` | Server's control port |
| `reconnect_interval` | `5` | Seconds to wait before reconnecting after disconnect |

#### Common Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `max_threads` | `200` | Maximum concurrent relay threads |
| `buffer_size` | `65536` | Read/write buffer size in bytes |
| `connect_timeout` | `10` | Timeout in seconds for outbound connections |
| `recv_timeout` | `60` | Socket receive/send timeout in seconds |
| `logging` | `1` | Enable (`1`) or disable (`0`) logging |
| `log_file` | *(empty)* | Log file path. Empty = stdout only. |

### Forwarding Rules Format (Client only)

Rules are defined under the `[rules]` section:

```
RemotePort:LocalAddress:LocalPort
```

| Component | Description | Examples |
|-----------|-------------|----------|
| `RemotePort` | Port the server will listen on | `8080`, `2222` |
| `LocalAddress` | Target address reachable from the client | `127.0.0.1`, `192.168.1.50` |
| `LocalPort` | Target port on LocalAddress | `80`, `22`, `443` |

## Building

### Windows (MSVC)

```cmd
cl /EHsc /O2 ReversePortForward.cpp /link ws2_32.lib
```

### Windows (MinGW g++)

```cmd
g++ -O2 -o ReversePortForward.exe ReversePortForward.cpp -lws2_32
```

### Linux (g++)

```bash
g++ -std=c++11 -O2 -pthread -o reverse_port_forward reverse_port_forward_linux.cpp
```

**Minimum C++ standard:** C++11

**Linux note:** The Linux version uses `pthread_timedjoin_np` (GNU extension, available on glibc). For musl or other libc, replace with `pthread_join` or detach listener threads.

## Running

### Server

```cmd
# Windows
ReversePortForward.exe                      # uses settings.txt in current directory
ReversePortForward.exe server_settings.txt  # custom settings file for server mode

# Linux
./reverse_port_forward                      # uses settings.txt in current directory
./reverse_port_forward server_settings.txt  # custom settings file for server mode
```

### Client

```cmd
# Windows
ReversePortForward.exe                      # uses settings.txt in current directory
ReversePortForward.exe client_settings.txt  # custom settings file for client mode

# Linux
./reverse_port_forward                      # uses settings.txt in current directory
./reverse_port_forward client_settings.txt  # custom settings file for client mode
```

The default settings file is `settings.txt` if no argument is provided.

## Use Cases

### Expose a local web server through a public server

**Client settings (behind NAT/firewall):**
```ini
mode=client
server_address=public-server.example.com
server_port=9000

[rules]
8080:127.0.0.1:80
```

Anyone connecting to `public-server.example.com:8080` reaches the client's local web server on port 80.

### Expose SSH access to a machine behind a firewall

**Client settings:**
```ini
[rules]
2222:127.0.0.1:22
```

`ssh -p 2222 public-server.example.com` connects through to the client machine's SSH server.

### Expose a service on another machine on the client's LAN

**Client settings:**
```ini
[rules]
5433:192.168.1.50:5432
```

Connects `public-server:5433` to a PostgreSQL server at `192.168.1.50:5432` on the client's local network.

### Multiple services at once

**Client settings:**
```ini
[rules]
8080:127.0.0.1:80
8443:127.0.0.1:443
2222:127.0.0.1:22
3390:127.0.0.1:3389
```

## Platform Differences

| Aspect | Windows (`ReversePortForward.cpp`) | Linux (`reverse_port_forward_linux.cpp`) |
|--------|--------------------------------------|------------------------------------------|
| Sockets | Winsock2 (`SOCKET`, `closesocket`, `SD_BOTH`) | POSIX (`int` fd, `close`, `SHUT_RDWR`) |
| Threading | `CreateThread` / `CloseHandle` | `pthread_create` with `PTHREAD_CREATE_DETACHED` |
| Thread count | `InterlockedIncrement` / `InterlockedDecrement` | `std::atomic<int>` |
| Session ID | `volatile DWORD` + `InterlockedIncrement` | `std::atomic<unsigned int>` |
| Log mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Control mutex | `CRITICAL_SECTION` (per client) | `pthread_mutex_t` (per client) |
| Session mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Non-blocking | `ioctlsocket(FIONBIO)` | `fcntl(O_NONBLOCK)` |
| Socket timeout | `DWORD` milliseconds | `struct timeval` seconds |
| select() nfds | `0` (ignored on Windows) | `max_fd + 1` (required on Linux) |
| Log file open | `_fsopen(..., _SH_DENYNO)` (shared access) | `fopen()` (no locking issues) |
| Log file flush | `fflush` + `_commit` | `fflush` |
| Reconnect sleep | `Sleep(ms)` | `sleep(sec)` |
| Wait for threads | `WaitForMultipleObjects` | `pthread_timedjoin_np` |
| Signal handling | N/A | `signal(SIGPIPE, SIG_IGN)` |
| Initialization | `WSAStartup` / `WSACleanup` | None needed |

## Log Output

### Server Log

```
[2026-05-14 10:00:01] === Reverse Port Forwarder — SERVER mode ===
[2026-05-14 10:00:01] [S] Server listening on 0.0.0.0:9000
[2026-05-14 10:00:05] [S] Client connected from 192.168.1.10
[2026-05-14 10:00:05] [S] Client 192.168.1.10 requests forward on port 8080 -> 127.0.0.1:80
[2026-05-14 10:00:05] [S] Client 192.168.1.10 requests forward on port 2222 -> 127.0.0.1:22
[2026-05-14 10:00:05] [S:8080] Listening for incoming connections
[2026-05-14 10:00:05] [S:2222] Listening for incoming connections
[2026-05-14 10:00:05] [S] Client 192.168.1.10: all rules registered, 2 listeners active
[2026-05-14 10:00:12] [S:8080] Incoming from 10.0.0.5:43210, session 2
[2026-05-14 10:00:12] [S] Data channel established for session 2
[2026-05-14 10:00:12] [S] Session 2: relaying
[2026-05-14 10:00:18] [S] Session 2: done
```

### Client Log

```
[2026-05-14 10:00:05] === Reverse Port Forwarder — CLIENT mode ===
[2026-05-14 10:00:05] [C] Connecting to server 192.168.56.101:9000 ...
[2026-05-14 10:00:05] [C] Connected to server
[2026-05-14 10:00:05] [C] Requested: server:8080 -> 127.0.0.1:80
[2026-05-14 10:00:05] [C] Requested: server:2222 -> 127.0.0.1:22
[2026-05-14 10:00:05] [C] All rules sent, waiting for server responses...
[2026-05-14 10:00:05] [C] Server confirmed port 8080
[2026-05-14 10:00:05] [C] Server confirmed port 2222
[2026-05-14 10:00:12] [C] Server requests tunnel: port 8080, session 2
[2026-05-14 10:00:12] [C] Session 2 (port 8080): connecting to local 127.0.0.1:80
[2026-05-14 10:00:12] [C] Session 2: tunnel active (server:8080 <-> 127.0.0.1:80)
[2026-05-14 10:00:18] [C] Session 2: tunnel closed
[2026-05-14 15:30:00] [C] Server disconnected
[2026-05-14 15:30:00] [C] Reconnecting in 5 seconds...
[2026-05-14 15:30:05] [C] Connecting to server 192.168.56.101:9000 ...
```

## Comparison with Other Tools

| Feature | Reverse Port Forwarder | Port Forwarder | SSH -R | Proxy Server |
|---------|----------------------|----------------|--------|--------------|
| Direction | Remote → Local (through NAT) | Local → Remote | Remote → Local | Dynamic (from URL) |
| NAT traversal | Yes | No | Yes | No |
| Requires server component | Yes (custom) | No | SSH server | No |
| Protocol awareness | None (raw TCP) | None (raw TCP) | None (raw TCP) | HTTP/HTTPS |
| Auto-reconnect | Yes | N/A | No (without tools) | N/A |
| Multiple rules | Yes | Yes | Yes | N/A |
| Encryption | No | No | Yes (SSH) | No |
| Authentication | No | No | Yes (SSH) | No |

## Security Considerations

- **No encryption** — All traffic (including the control protocol) is sent in plaintext. For production use over untrusted networks, tunnel through a VPN or SSH.
- **No authentication** — Any client can connect to the server and register forwarding rules. For production use, add authentication or restrict access via firewall rules.
- The control protocol is text-based and simple, making it easy to monitor and debug but also easy to tamper with on untrusted networks.

## Firewall Notes

### Server

The server needs inbound access on:
1. The **control port** (e.g., 9000)
2. All **forwarded ports** requested by clients (e.g., 8080, 2222)

```bash
# Linux: firewalld
sudo firewall-cmd --add-port=9000/tcp --add-port=8080/tcp --add-port=2222/tcp --permanent
sudo firewall-cmd --reload

# Linux: iptables
sudo iptables -I INPUT -p tcp --dport 9000 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 8080 -j ACCEPT
sudo iptables -I INPUT -p tcp --dport 2222 -j ACCEPT

# Linux: ufw
sudo ufw allow 9000/tcp
sudo ufw allow 8080/tcp
sudo ufw allow 2222/tcp
```

```cmd
:: Windows
netsh advfirewall firewall add rule name="RPF Server" dir=in action=allow protocol=tcp localport=9000,8080,2222
```

### Client

The client only makes **outbound** connections, so typically no firewall changes are needed. It needs outbound TCP access to the server's control port.
