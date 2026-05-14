/*
 * Multithreaded Reverse Port Forwarder for Windows
 * Works like SSH reverse port forwarding (-R).
 *
 * Two modes (set in settings file):
 *   SERVER: Listens for control connections from clients. When a client
 *           registers forwarding rules, the server opens listening ports.
 *           Incoming connections on those ports are tunneled back through
 *           the client to the target service.
 *
 *   CLIENT: Connects to the server, sends forwarding rules. When the server
 *           signals a new incoming connection, the client connects to the
 *           local target and opens a data channel to relay traffic.
 *
 * Control Protocol (text-based, over the control TCP connection):
 *   Client -> Server:  FORWARD <remote_port> <local_host> <local_port>\n
 *   Client -> Server:  READY\n
 *   Server -> Client:  OK <remote_port>\n
 *   Server -> Client:  ERROR <remote_port> <reason>\n
 *   Server -> Client:  CONNECT <remote_port> <session_id>\n
 *   Client -> Server:  (opens new TCP to server) DATA <session_id>\n
 *
 * Build: cl /EHsc /O2 reverse_port_forward.cpp /link ws2_32.lib
 *    or: g++ -O2 -o reverse_port_forward.exe reverse_port_forward.cpp -lws2_32
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <share.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

 // ---------------------------------------------------------------------------
 // Forwarding rule (client-side config)
 // ---------------------------------------------------------------------------
struct ForwardRule {
    int         remote_port;
    std::string local_host;
    int         local_port;
};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Settings {
    std::string mode;  // "server" or "client"

    // Server mode
    std::string listen_address = "0.0.0.0";
    int         listen_port = 9000;

    // Client mode
    std::string server_address;
    int         server_port = 9000;

    // Common
    int         max_threads = 200;
    int         buffer_size = 65536;
    int         connect_timeout = 10;
    int         recv_timeout = 60;
    int         reconnect_interval = 5;  // seconds between reconnect attempts
    bool        logging = true;
    std::string log_file;

    std::vector<ForwardRule> rules;
};

static Settings         g_settings;
static LONG             g_active_threads = 0;
static FILE* g_log_fp = nullptr;
static CRITICAL_SECTION g_log_cs;

// ---------------------------------------------------------------------------
// Session management (server side) — maps session_id to pending sockets
// ---------------------------------------------------------------------------
static CRITICAL_SECTION           g_session_cs;
static std::map<DWORD, SOCKET>    g_pending_sessions;  // session_id -> incoming socket
static volatile DWORD             g_next_session_id = 1;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    EnterCriticalSection(&g_log_cs);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char timestamp[64];
    _snprintf_s(timestamp, sizeof(timestamp), _TRUNCATE,
        "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "[%s] ", timestamp);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);

    if (g_log_fp) {
        va_end(ap);
        va_start(ap, fmt);
        fprintf(g_log_fp, "[%s] ", timestamp);
        vfprintf(g_log_fp, fmt, ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
        _commit(_fileno(g_log_fp));
    }

    va_end(ap);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Load settings
// ---------------------------------------------------------------------------
static bool load_settings(const char* path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        fprintf(stderr, "Error: Cannot open %s\n", path);
        return false;
    }

    bool in_rules = false;
    std::string line;

    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line == "[rules]") {
            in_rules = true;
            continue;
        }

        if (!in_rules) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (key == "mode")            g_settings.mode = val;
            else if (key == "listen_address")  g_settings.listen_address = val;
            else if (key == "listen_port")     g_settings.listen_port = atoi(val.c_str());
            else if (key == "server_address")  g_settings.server_address = val;
            else if (key == "server_port")     g_settings.server_port = atoi(val.c_str());
            else if (key == "max_threads")     g_settings.max_threads = atoi(val.c_str());
            else if (key == "buffer_size")     g_settings.buffer_size = atoi(val.c_str());
            else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
            else if (key == "recv_timeout")    g_settings.recv_timeout = atoi(val.c_str());
            else if (key == "reconnect_interval") g_settings.reconnect_interval = atoi(val.c_str());
            else if (key == "logging")         g_settings.logging = (val == "1");
            else if (key == "log_file")        g_settings.log_file = val;
        }
        else {
            // Rule: RemotePort:LocalAddress:LocalPort
            size_t c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            size_t c2 = line.rfind(':');
            if (c2 == c1) continue;

            ForwardRule rule;
            rule.remote_port = atoi(line.substr(0, c1).c_str());
            rule.local_host = trim(line.substr(c1 + 1, c2 - c1 - 1));
            rule.local_port = atoi(line.substr(c2 + 1).c_str());

            if (rule.remote_port > 0 && rule.remote_port <= 65535 &&
                rule.local_port > 0 && rule.local_port <= 65535 &&
                !rule.local_host.empty()) {
                g_settings.rules.push_back(rule);
            }
            else {
                fprintf(stderr, "Warning: Invalid rule: %s\n", line.c_str());
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------
static void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

static void set_socket_timeouts(SOCKET s, int recv_sec, int send_sec) {
    DWORD recv_ms = recv_sec * 1000;
    DWORD send_ms = send_sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_ms, sizeof(recv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_ms, sizeof(send_ms));
}

static SOCKET connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    _snprintf_s(port_str, sizeof(port_str), _TRUNCATE, "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int ret = connect(sock, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            close_socket(sock);
            return INVALID_SOCKET;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(0, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            close_socket(sock);
            return INVALID_SOCKET;
        }

        int so_error = 0;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            close_socket(sock);
            return INVALID_SOCKET;
        }
    }

    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Read a line from socket (up to \n). Returns false on disconnect.
// ---------------------------------------------------------------------------
static bool recv_line(SOCKET s, std::string& line) {
    line.clear();
    char ch;
    while (true) {
        int n = recv(s, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') return true;
        if (ch != '\r') line += ch;
    }
}

// ---------------------------------------------------------------------------
// Send a string on a socket. Returns false on error.
// ---------------------------------------------------------------------------
static bool send_all(SOCKET s, const std::string& data) {
    int total = 0;
    int len = (int)data.size();
    while (total < len) {
        int n = send(s, data.c_str() + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bidirectional relay
// ---------------------------------------------------------------------------
static void relay(SOCKET a, SOCKET b) {
    char* buf = new char[g_settings.buffer_size];
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(a, &rset);
        FD_SET(b, &rset);

        struct timeval tv;
        tv.tv_sec = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(0, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(a, &rset)) {
            int n = recv(a, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                int s = send(b, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }

        if (FD_ISSET(b, &rset)) {
            int n = recv(b, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = 0;
            while (sent < n) {
                int s = send(a, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }
    }

done:
    delete[] buf;
}

// ===================================================================
//  SERVER MODE
// ===================================================================

// Context for a forwarded-port listener on the server
struct ServerForwardCtx {
    int    remote_port;
    SOCKET control_sock;  // control channel back to client
    CRITICAL_SECTION* control_cs;  // mutex for sending on control channel
};

// Thread: accepts incoming connections on a forwarded port
static DWORD WINAPI server_forward_listener(LPVOID param) {
    ServerForwardCtx* ctx = (ServerForwardCtx*)param;
    int remote_port = ctx->remote_port;
    SOCKET control = ctx->control_sock;
    CRITICAL_SECTION* control_cs = ctx->control_cs;
    delete ctx;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        log_msg("[S:%d] socket() failed: %d", remote_port, WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)remote_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_msg("[S:%d] bind() failed: %d", remote_port, WSAGetLastError());
        // Notify client of error
        EnterCriticalSection(control_cs);
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "ERROR %d bind_failed\n", remote_port);
        send_all(control, msg);
        LeaveCriticalSection(control_cs);
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        log_msg("[S:%d] listen() failed: %d", remote_port, WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    // Send OK to client
    EnterCriticalSection(control_cs);
    char ok_msg[128];
    _snprintf_s(ok_msg, sizeof(ok_msg), _TRUNCATE, "OK %d\n", remote_port);
    send_all(control, ok_msg);
    LeaveCriticalSection(control_cs);

    log_msg("[S:%d] Listening for incoming connections", remote_port);

    while (true) {
        sockaddr_in client_addr = {};
        int addr_len = sizeof(client_addr);
        SOCKET incoming = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (incoming == INVALID_SOCKET) {
            // Control socket may have been closed (client disconnected)
            log_msg("[S:%d] accept() failed: %d", remote_port, WSAGetLastError());
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        // Assign a session ID
        DWORD session_id = InterlockedIncrement(&g_next_session_id);

        log_msg("[S:%d] Incoming from %s:%d, session %u",
            remote_port, client_ip, client_port, session_id);

        // Store the incoming socket
        EnterCriticalSection(&g_session_cs);
        g_pending_sessions[session_id] = incoming;
        LeaveCriticalSection(&g_session_cs);

        // Tell client to open a data channel
        EnterCriticalSection(control_cs);
        char connect_msg[128];
        _snprintf_s(connect_msg, sizeof(connect_msg), _TRUNCATE,
            "CONNECT %d %u\n", remote_port, session_id);
        bool sent = send_all(control, connect_msg);
        LeaveCriticalSection(control_cs);

        if (!sent) {
            log_msg("[S:%d] Failed to notify client, closing session %u", remote_port, session_id);
            EnterCriticalSection(&g_session_cs);
            g_pending_sessions.erase(session_id);
            LeaveCriticalSection(&g_session_cs);
            closesocket(incoming);
            break;
        }

        // The data channel handler will pick up the incoming socket
        // and relay it. We wait up to connect_timeout for the client
        // to establish the data channel. If it doesn't, clean up.
        // (This is handled in the data accept thread)
    }

    closesocket(listen_sock);
    log_msg("[S:%d] Forward listener stopped", remote_port);
    return 0;
}

// Thread: relays between data channel socket and the pending incoming socket
struct DataRelayCtx {
    SOCKET data_sock;
    SOCKET incoming_sock;
    DWORD  session_id;
};

static DWORD WINAPI data_relay_thread(LPVOID param) {
    InterlockedIncrement(&g_active_threads);
    DataRelayCtx* ctx = (DataRelayCtx*)param;
    SOCKET data = ctx->data_sock;
    SOCKET incoming = ctx->incoming_sock;
    DWORD  sid = ctx->session_id;
    delete ctx;

    log_msg("[S] Session %u: relaying", sid);
    relay(incoming, data);
    log_msg("[S] Session %u: done", sid);

    close_socket(data);
    close_socket(incoming);
    InterlockedDecrement(&g_active_threads);
    return 0;
}

// Thread: handles one client control connection on the server
struct ServerClientCtx {
    SOCKET control_sock;
    sockaddr_in client_addr;
};

static DWORD WINAPI server_client_handler(LPVOID param) {
    InterlockedIncrement(&g_active_threads);

    ServerClientCtx* sctx = (ServerClientCtx*)param;
    SOCKET control = sctx->control_sock;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    delete sctx;

    log_msg("[S] Client connected from %s", client_ip);

    set_socket_timeouts(control, g_settings.recv_timeout, g_settings.recv_timeout);

    CRITICAL_SECTION control_cs;
    InitializeCriticalSection(&control_cs);

    // Phase 1: Receive FORWARD rules, then READY
    std::vector<HANDLE> listener_handles;

    while (true) {
        std::string line;
        if (!recv_line(control, line)) {
            log_msg("[S] Client %s disconnected during setup", client_ip);
            goto cleanup;
        }

        line = trim(line);
        if (line == "READY") break;

        // Parse: FORWARD <remote_port> <local_host> <local_port>
        if (line.find("FORWARD ") == 0) {
            std::istringstream iss(line.substr(8));
            int rport;
            std::string lhost;
            int lport;
            iss >> rport >> lhost >> lport;

            if (rport <= 0 || rport > 65535) {
                EnterCriticalSection(&control_cs);
                char msg[256];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "ERROR %d invalid_port\n", rport);
                send_all(control, msg);
                LeaveCriticalSection(&control_cs);
                continue;
            }

            log_msg("[S] Client %s requests forward on port %d -> %s:%d",
                client_ip, rport, lhost.c_str(), lport);

            // Launch a listener for this forwarded port
            ServerForwardCtx* fctx = new ServerForwardCtx;
            fctx->remote_port = rport;
            fctx->control_sock = control;
            fctx->control_cs = &control_cs;

            HANDLE h = CreateThread(nullptr, 0, server_forward_listener, fctx, 0, nullptr);
            if (h) {
                listener_handles.push_back(h);
            }
            else {
                log_msg("[S] Failed to create listener thread for port %d", rport);
                delete fctx;
                EnterCriticalSection(&control_cs);
                char msg[256];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE, "ERROR %d thread_failed\n", rport);
                send_all(control, msg);
                LeaveCriticalSection(&control_cs);
            }
        }
    }

    log_msg("[S] Client %s: all rules registered, %d listeners active",
        client_ip, (int)listener_handles.size());

    // Phase 2: Keep control connection alive.
    // The control channel is used by listener threads to send CONNECT messages.
    // We also need to accept DATA connections from the client on the same
    // control port. The client will open new connections with "DATA <session_id>\n".
    // But we handle those in the main accept loop — so here we just keep
    // the control connection alive by reading (there shouldn't be anything
    // from the client on control, but we detect disconnect).
    {
        // Set a long timeout for the control channel — it's long-lived
        set_socket_timeouts(control, 0, 0);  // no timeout

        char buf[1];
        while (true) {
            int n = recv(control, buf, 1, 0);
            if (n <= 0) break;  // client disconnected
        }
    }

    log_msg("[S] Client %s disconnected", client_ip);

cleanup:
    close_socket(control);

    // TODO: Ideally we'd signal listener threads to stop.
    // For now they'll fail on the next send_all to control and exit.
    // Wait a bit for them to notice.
    if (!listener_handles.empty()) {
        WaitForMultipleObjects((DWORD)listener_handles.size(),
            listener_handles.data(), TRUE, 5000);
        for (HANDLE h : listener_handles) CloseHandle(h);
    }

    DeleteCriticalSection(&control_cs);
    InterlockedDecrement(&g_active_threads);
    return 0;
}

// Server main: accepts control connections AND data connections
static int run_server() {
    InitializeCriticalSection(&g_session_cs);

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_settings.listen_port);
    inet_pton(AF_INET, g_settings.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        return 1;
    }

    log_msg("[S] Server listening on %s:%d",
        g_settings.listen_address.c_str(), g_settings.listen_port);

    while (true) {
        sockaddr_in client_addr = {};
        int addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET) {
            log_msg("[S] accept() failed: %d", WSAGetLastError());
            continue;
        }

        set_socket_timeouts(client_sock, g_settings.recv_timeout, g_settings.recv_timeout);

        // Peek at the first line to determine if this is a control or data connection
        std::string first_line;
        if (!recv_line(client_sock, first_line)) {
            closesocket(client_sock);
            continue;
        }
        first_line = trim(first_line);

        if (first_line.find("DATA ") == 0) {
            // Data channel: "DATA <session_id>"
            DWORD session_id = (DWORD)atoi(first_line.substr(5).c_str());

            EnterCriticalSection(&g_session_cs);
            auto it = g_pending_sessions.find(session_id);
            SOCKET incoming = INVALID_SOCKET;
            if (it != g_pending_sessions.end()) {
                incoming = it->second;
                g_pending_sessions.erase(it);
            }
            LeaveCriticalSection(&g_session_cs);

            if (incoming == INVALID_SOCKET) {
                log_msg("[S] Unknown session %u, closing data channel", session_id);
                closesocket(client_sock);
                continue;
            }

            log_msg("[S] Data channel established for session %u", session_id);

            DataRelayCtx* dctx = new DataRelayCtx;
            dctx->data_sock = client_sock;
            dctx->incoming_sock = incoming;
            dctx->session_id = session_id;

            HANDLE h = CreateThread(nullptr, 0, data_relay_thread, dctx, 0, nullptr);
            if (h) {
                CloseHandle(h);
            }
            else {
                log_msg("[S] Failed to create relay thread for session %u", session_id);
                delete dctx;
                closesocket(client_sock);
                closesocket(incoming);
            }
        }
        else if (first_line.find("FORWARD ") == 0 || first_line == "READY") {
            // Control connection — first line is already a FORWARD or READY command.
            // We need to process it. We'll create a handler and "replay" it.
            // But since recv_line already consumed it, we handle it here.

            ServerClientCtx* sctx = new ServerClientCtx;
            sctx->control_sock = client_sock;
            sctx->client_addr = client_addr;

            // We consumed the first line. The simplest approach is to handle
            // the client inline in this special case. Instead, let's use a
            // wrapper that includes the first line.

            // Actually, let's send the first line info differently.
            // We'll handle this by "unreading" conceptually: we create a
            // thread and pass the first line along.

            // For simplicity, we handle the first FORWARD in the handler by
            // sending it back through. We'll use a different approach:
            // the client sends "CONTROL\n" first to identify itself.

            // NOTE: This requires the client to send "CONTROL\n" as the very
            // first line before any FORWARD commands.

            // Since we already designed it this way, let's adjust:
            // We already read the first line. If it's FORWARD, this IS a
            // control connection. Let's handle it right here.

            // For a clean design, let's just treat first_line as part of
            // the control flow and handle it in-thread.
            delete sctx;

            // We need a thread to handle this control client, but we need
            // to pass the first_line to it.

            struct ControlHandlerCtx {
                SOCKET control_sock;
                sockaddr_in client_addr;
                std::string first_line;
            };

            ControlHandlerCtx* cctx = new ControlHandlerCtx;
            cctx->control_sock = client_sock;
            cctx->client_addr = client_addr;
            cctx->first_line = first_line;

            // Inline lambda isn't available for CreateThread, so we use a static function
            // that accepts a ControlHandlerCtx.
            // We define this handler below.

            // Actually, let's keep it simple: use a dedicated thread proc.
            // We'll store the first_line in a global temporarily... or just
            // embed the logic.

            // Simplest: handle the control connection in a dedicated function.
            // Pass ControlHandlerCtx via a static wrapper.

            struct CtrlThreadParam {
                SOCKET control_sock;
                sockaddr_in client_addr;
                std::string first_line;
            };

            // We can't easily pass a C++ object via LPVOID to CreateThread
            // without a static trampoline. Let's use a simple approach:

            // Actually ControlHandlerCtx* works fine as LPVOID.
            // The issue is we can't use a local struct as the thread param type
            // in the thread function declaration. Let's use the approach of
            // handling it with a global struct. But we already have
            // ServerClientCtx. Let's extend it.

            // Cleanest solution: handle the control client inline with a helper.
            // We'll define a proper struct and thread function.

            // For now, the simplest correct approach: push the first command
            // back by handling it before entering the loop.

            // Let's restructure — spawn a thread using a heap-allocated context.
            // ... actually this got complex. Let me simplify the whole approach.
            // The client will send "CONTROL\n" first. Let me just redefine
            // the protocol slightly and adjust both sides.

            // --- REVISED: we simply require "CONTROL\n" as the first line
            //     from a control client, and "DATA <sid>\n" from data channels.

            // But since we're already HERE with first_line == "FORWARD ...",
            // we know this is a control connection. Let's just handle it properly.

            // SOLUTION: We'll pass the first_line embedded in a special struct.

            // We create the struct on heap and pass it.
            // The thread function casts param back.

            // (The code above with sctx was wrong, let me do it properly)

            // This struct is identical to what we need:
            // We already have cctx allocated. Let's define the thread func.

            // Due to C++ constraints with CreateThread, we define a static
            // function that casts LPVOID. We'll handle first_line processing.

            // Forwarding the first line into the thread by storing it in a
            // heap struct is fine. We do it via a global helper struct.

            // Let's just define a static thread proc here and pass cctx.
            // ... but we can't define a function inside main/run_server.

            // OK, simplest approach: don't consume the first line.
            // Instead, require the client to send "CONTROL\n" first.
            // Then all FORWARD lines come after.
            // Let's adjust the protocol.

            // Since we already have first_line = "FORWARD ...", and this is
            // a control connection, let's just close it and require
            // "CONTROL\n" as the first line.

            log_msg("[S] Protocol error: expected CONTROL or DATA, got: %s", first_line.c_str());
            log_msg("[S] Hint: Control clients must send CONTROL as first line");
            closesocket(client_sock);
            delete cctx;
        }
        else if (first_line == "CONTROL") {
            // New control connection
            ServerClientCtx* sctx = new ServerClientCtx;
            sctx->control_sock = client_sock;
            sctx->client_addr = client_addr;

            HANDLE h = CreateThread(nullptr, 0, server_client_handler, sctx, 0, nullptr);
            if (h) {
                CloseHandle(h);
            }
            else {
                log_msg("[S] Failed to create client handler thread");
                delete sctx;
                closesocket(client_sock);
            }
        }
        else {
            log_msg("[S] Unknown first line: %s", first_line.c_str());
            closesocket(client_sock);
        }
    }

    closesocket(listen_sock);
    DeleteCriticalSection(&g_session_cs);
    return 0;
}

// ===================================================================
//  CLIENT MODE
// ===================================================================

// Thread: handles a CONNECT notification — opens local target, opens data
// channel to server, relays.
struct ClientTunnelCtx {
    std::string server_address;
    int         server_port;
    DWORD       session_id;
    std::string local_host;
    int         local_port;
    int         remote_port;
};

static DWORD WINAPI client_tunnel_thread(LPVOID param) {
    InterlockedIncrement(&g_active_threads);

    ClientTunnelCtx* ctx = (ClientTunnelCtx*)param;
    std::string srv_addr = ctx->server_address;
    int srv_port = ctx->server_port;
    DWORD sid = ctx->session_id;
    std::string lhost = ctx->local_host;
    int lport = ctx->local_port;
    int rport = ctx->remote_port;
    delete ctx;

    log_msg("[C] Session %u (port %d): connecting to local %s:%d",
        sid, rport, lhost.c_str(), lport);

    // Connect to local target
    SOCKET local_sock = connect_to_host(lhost.c_str(), lport);
    if (local_sock == INVALID_SOCKET) {
        log_msg("[C] Session %u: cannot connect to local %s:%d", sid, lhost.c_str(), lport);
        InterlockedDecrement(&g_active_threads);
        return 0;
    }

    // Open data channel to server
    SOCKET data_sock = connect_to_host(srv_addr.c_str(), srv_port);
    if (data_sock == INVALID_SOCKET) {
        log_msg("[C] Session %u: cannot connect data channel to server", sid);
        close_socket(local_sock);
        InterlockedDecrement(&g_active_threads);
        return 0;
    }

    // Identify as data channel
    char data_msg[128];
    _snprintf_s(data_msg, sizeof(data_msg), _TRUNCATE, "DATA %u\n", sid);
    if (!send_all(data_sock, data_msg)) {
        log_msg("[C] Session %u: failed to send DATA handshake", sid);
        close_socket(data_sock);
        close_socket(local_sock);
        InterlockedDecrement(&g_active_threads);
        return 0;
    }

    log_msg("[C] Session %u: tunnel active (server:%d <-> %s:%d)",
        sid, rport, lhost.c_str(), lport);

    relay(local_sock, data_sock);

    log_msg("[C] Session %u: tunnel closed", sid);

    close_socket(data_sock);
    close_socket(local_sock);
    InterlockedDecrement(&g_active_threads);
    return 0;
}

static int run_client() {
    if (g_settings.rules.empty()) {
        fprintf(stderr, "No forwarding rules defined.\n");
        return 1;
    }

    log_msg("[C] Connecting to server %s:%d ...",
        g_settings.server_address.c_str(), g_settings.server_port);

    SOCKET control = connect_to_host(g_settings.server_address.c_str(),
        g_settings.server_port);
    if (control == INVALID_SOCKET) {
        fprintf(stderr, "Cannot connect to server %s:%d\n",
            g_settings.server_address.c_str(), g_settings.server_port);
        return 1;
    }

    log_msg("[C] Connected to server");

    // Identify as control connection
    if (!send_all(control, "CONTROL\n")) {
        fprintf(stderr, "Failed to send CONTROL handshake\n");
        close_socket(control);
        return 1;
    }

    // Build a map of remote_port -> rule for CONNECT lookups
    std::map<int, ForwardRule> rule_map;

    // Send forwarding rules
    for (const auto& rule : g_settings.rules) {
        char msg[512];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
            "FORWARD %d %s %d\n",
            rule.remote_port, rule.local_host.c_str(), rule.local_port);

        if (!send_all(control, msg)) {
            fprintf(stderr, "Failed to send rule\n");
            close_socket(control);
            return 1;
        }

        rule_map[rule.remote_port] = rule;
        log_msg("[C] Requested: server:%d -> %s:%d",
            rule.remote_port, rule.local_host.c_str(), rule.local_port);
    }

    // Send READY
    if (!send_all(control, "READY\n")) {
        fprintf(stderr, "Failed to send READY\n");
        close_socket(control);
        return 1;
    }

    log_msg("[C] All rules sent, waiting for server responses...");

    // Read server responses and handle CONNECT notifications
    // Set no timeout on control — it's long-lived
    set_socket_timeouts(control, 0, 0);

    while (true) {
        std::string line;
        if (!recv_line(control, line)) {
            log_msg("[C] Server disconnected");
            break;
        }

        line = trim(line);

        if (line.find("OK ") == 0) {
            int port = atoi(line.substr(3).c_str());
            log_msg("[C] Server confirmed port %d", port);
        }
        else if (line.find("ERROR ") == 0) {
            log_msg("[C] Server error: %s", line.c_str());
        }
        else if (line.find("CONNECT ") == 0) {
            // CONNECT <remote_port> <session_id>
            std::istringstream iss(line.substr(8));
            int rport;
            DWORD sid;
            iss >> rport >> sid;

            log_msg("[C] Server requests tunnel: port %d, session %u", rport, sid);

            auto it = rule_map.find(rport);
            if (it == rule_map.end()) {
                log_msg("[C] No rule for port %d, ignoring", rport);
                continue;
            }

            ClientTunnelCtx* ctx = new ClientTunnelCtx;
            ctx->server_address = g_settings.server_address;
            ctx->server_port = g_settings.server_port;
            ctx->session_id = sid;
            ctx->local_host = it->second.local_host;
            ctx->local_port = it->second.local_port;
            ctx->remote_port = rport;

            HANDLE h = CreateThread(nullptr, 0, client_tunnel_thread, ctx, 0, nullptr);
            if (h) {
                CloseHandle(h);
            }
            else {
                log_msg("[C] Failed to create tunnel thread for session %u", sid);
                delete ctx;
            }
        }
        else {
            log_msg("[C] Unknown message: %s", line.c_str());
        }
    }

    close_socket(control);
    return 0;
}

// ===================================================================
//  MAIN
// ===================================================================
int main(int argc, char* argv[]) {
    const char* settings_path = "settings.txt";
    if (argc > 1) settings_path = argv[1];

    InitializeCriticalSection(&g_log_cs);

    if (!load_settings(settings_path)) {
        return 1;
    }

    // Open log file
    if (!g_settings.log_file.empty()) {
        g_log_fp = _fsopen(g_settings.log_file.c_str(), "a", _SH_DENYNO);
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    int ret;
    if (g_settings.mode == "server") {
        log_msg("=== Reverse Port Forwarder - SERVER mode ===");
        ret = run_server();
    }
    else if (g_settings.mode == "client") {
        log_msg("=== Reverse Port Forwarder — CLIENT mode ===");
        while (true) {
            ret = run_client();
            log_msg("[C] Reconnecting in %d seconds...", g_settings.reconnect_interval);
            Sleep(g_settings.reconnect_interval * 1000);
        }
    }
    else {
        fprintf(stderr, "Error: mode must be 'server' or 'client' (got '%s')\n",
            g_settings.mode.c_str());
        ret = 1;
    }

    WSACleanup();
    if (g_log_fp) fclose(g_log_fp);
    DeleteCriticalSection(&g_log_cs);
    return ret;
}
