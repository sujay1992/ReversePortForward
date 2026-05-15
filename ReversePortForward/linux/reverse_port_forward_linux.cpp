/*
 * Multithreaded Reverse Port Forwarder for Linux
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
 *           Auto-reconnects if the server goes down.
 *
 * Control Protocol (text-based, over the control TCP connection):
 *   Client -> Server:  CONTROL\n
 *   Client -> Server:  FORWARD <remote_port> <local_host> <local_port>\n
 *   Client -> Server:  READY\n
 *   Server -> Client:  OK <remote_port>\n
 *   Server -> Client:  ERROR <remote_port> <reason>\n
 *   Server -> Client:  CONNECT <remote_port> <session_id>\n
 *   Client -> Server:  (opens new TCP to server) DATA <session_id>\n
 *
 * Build: g++ -std=c++11 -O2 -pthread -o reverse_port_forward reverse_port_forward_linux.cpp
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <atomic>

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
    int         listen_port    = 9000;

    // Client mode
    std::string server_address;
    int         server_port    = 9000;

    // Common
    int         max_threads     = 200;
    int         buffer_size     = 65536;
    int         connect_timeout = 10;
    int         recv_timeout    = 60;
    int         reconnect_interval = 5;
    bool        logging         = true;
    std::string log_file;

    std::vector<ForwardRule> rules;
};

static Settings          g_settings;
static std::atomic<int>  g_active_threads(0);
static FILE*             g_log_fp = nullptr;
static pthread_mutex_t   g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Session management (server side)
// ---------------------------------------------------------------------------
static pthread_mutex_t          g_session_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::map<unsigned int, int> g_pending_sessions;  // session_id -> incoming fd
static std::atomic<unsigned int>   g_next_session_id(1);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    pthread_mutex_lock(&g_log_mutex);

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

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
    }

    va_end(ap);
    pthread_mutex_unlock(&g_log_mutex);
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

            if      (key == "mode")            g_settings.mode            = val;
            else if (key == "listen_address")  g_settings.listen_address  = val;
            else if (key == "listen_port")     g_settings.listen_port     = atoi(val.c_str());
            else if (key == "server_address")  g_settings.server_address  = val;
            else if (key == "server_port")     g_settings.server_port     = atoi(val.c_str());
            else if (key == "max_threads")     g_settings.max_threads     = atoi(val.c_str());
            else if (key == "buffer_size")     g_settings.buffer_size     = atoi(val.c_str());
            else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
            else if (key == "recv_timeout")    g_settings.recv_timeout    = atoi(val.c_str());
            else if (key == "reconnect_interval") g_settings.reconnect_interval = atoi(val.c_str());
            else if (key == "logging")         g_settings.logging         = (val == "1");
            else if (key == "log_file")        g_settings.log_file        = val;
        } else {
            // Rule: RemotePort:LocalAddress:LocalPort
            size_t c1 = line.find(':');
            if (c1 == std::string::npos) continue;
            size_t c2 = line.rfind(':');
            if (c2 == c1) continue;

            ForwardRule rule;
            rule.remote_port = atoi(line.substr(0, c1).c_str());
            rule.local_host  = trim(line.substr(c1 + 1, c2 - c1 - 1));
            rule.local_port  = atoi(line.substr(c2 + 1).c_str());

            if (rule.remote_port > 0 && rule.remote_port <= 65535 &&
                rule.local_port > 0 && rule.local_port <= 65535 &&
                !rule.local_host.empty()) {
                g_settings.rules.push_back(rule);
            } else {
                fprintf(stderr, "Warning: Invalid rule: %s\n", line.c_str());
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------
static void close_socket(int& s) {
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
        s = -1;
    }
}

static void set_socket_timeouts(int s, int recv_sec, int send_sec) {
    struct timeval recv_tv = { recv_sec, 0 };
    struct timeval send_tv = { send_sec, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));
}

static int connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0) {
        if (errno != EINPROGRESS) {
            close_socket(sock);
            return -1;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec  = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(sock + 1, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            close_socket(sock);
            return -1;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            close_socket(sock);
            return -1;
        }
    }

    fcntl(sock, F_SETFL, flags);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Read a line from socket (up to \n). Returns false on disconnect.
// ---------------------------------------------------------------------------
static bool recv_line(int s, std::string& line) {
    line.clear();
    char ch;
    while (true) {
        ssize_t n = recv(s, &ch, 1, 0);
        if (n <= 0) return false;
        if (ch == '\n') return true;
        if (ch != '\r') line += ch;
    }
}

// ---------------------------------------------------------------------------
// Send a string on a socket
// ---------------------------------------------------------------------------
static bool send_all(int s, const std::string& data) {
    size_t total = 0;
    size_t len = data.size();
    while (total < len) {
        ssize_t n = send(s, data.c_str() + total, len - total, 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Bidirectional relay
// ---------------------------------------------------------------------------
static void relay(int a, int b) {
    char* buf = new char[g_settings.buffer_size];
    int maxfd = (a > b ? a : b) + 1;
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(a, &rset);
        FD_SET(b, &rset);

        struct timeval tv;
        tv.tv_sec  = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(maxfd, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(a, &rset)) {
            ssize_t n = recv(a, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = send(b, buf + sent, n - sent, 0);
                if (s <= 0) goto done;
                sent += s;
            }
        }

        if (FD_ISSET(b, &rset)) {
            ssize_t n = recv(b, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t s = send(a, buf + sent, n - sent, 0);
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

struct ServerForwardCtx {
    int            remote_port;
    int            control_sock;
    pthread_mutex_t* control_mutex;
    int            listen_sock;   // set by the listener thread, closed by cleanup to unblock accept()
};

static void* server_forward_listener(void* param) {
    ServerForwardCtx* ctx = (ServerForwardCtx*)param;
    int remote_port         = ctx->remote_port;
    int control             = ctx->control_sock;
    pthread_mutex_t* cmutex = ctx->control_mutex;
    // Note: do NOT delete ctx here; the client handler owns it and uses ctx->listen_sock

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        log_msg("[S:%d] socket() failed: %s", remote_port, strerror(errno));
        ctx->listen_sock = -1;
        return (void*)1;
    }
    ctx->listen_sock = listen_sock;  // publish so cleanup can close it

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)remote_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log_msg("[S:%d] bind() failed: %s", remote_port, strerror(errno));
        pthread_mutex_lock(cmutex);
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR %d bind_failed\n", remote_port);
        send_all(control, msg);
        pthread_mutex_unlock(cmutex);
        close(listen_sock);
        return (void*)1;
    }

    if (listen(listen_sock, SOMAXCONN) < 0) {
        log_msg("[S:%d] listen() failed: %s", remote_port, strerror(errno));
        close(listen_sock);
        return (void*)1;
    }

    // Send OK to client
    pthread_mutex_lock(cmutex);
    char ok_msg[128];
    snprintf(ok_msg, sizeof(ok_msg), "OK %d\n", remote_port);
    send_all(control, ok_msg);
    pthread_mutex_unlock(cmutex);

    log_msg("[S:%d] Listening for incoming connections", remote_port);

    while (true) {
        sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int incoming = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (incoming < 0) {
            log_msg("[S:%d] accept() failed: %s", remote_port, strerror(errno));
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        int client_port = ntohs(client_addr.sin_port);

        unsigned int session_id = g_next_session_id++;

        log_msg("[S:%d] Incoming from %s:%d, session %u",
                remote_port, client_ip, client_port, session_id);

        // Store the incoming socket
        pthread_mutex_lock(&g_session_mutex);
        g_pending_sessions[session_id] = incoming;
        pthread_mutex_unlock(&g_session_mutex);

        // Tell client to open a data channel
        pthread_mutex_lock(cmutex);
        char connect_msg[128];
        snprintf(connect_msg, sizeof(connect_msg),
                 "CONNECT %d %u\n", remote_port, session_id);
        bool sent = send_all(control, connect_msg);
        pthread_mutex_unlock(cmutex);

        if (!sent) {
            log_msg("[S:%d] Failed to notify client, closing session %u", remote_port, session_id);
            pthread_mutex_lock(&g_session_mutex);
            g_pending_sessions.erase(session_id);
            pthread_mutex_unlock(&g_session_mutex);
            close(incoming);
            break;
        }
    }

    // Only close if not already closed by cleanup
    if (ctx->listen_sock >= 0) {
        close(ctx->listen_sock);
        ctx->listen_sock = -1;
    }
    log_msg("[S:%d] Forward listener stopped", remote_port);
    return nullptr;
}

// Thread: relays between data channel socket and the pending incoming socket
struct DataRelayCtx {
    int          data_sock;
    int          incoming_sock;
    unsigned int session_id;
};

static void* data_relay_thread(void* param) {
    g_active_threads++;
    DataRelayCtx* ctx = (DataRelayCtx*)param;
    int data     = ctx->data_sock;
    int incoming = ctx->incoming_sock;
    unsigned int sid = ctx->session_id;
    delete ctx;

    log_msg("[S] Session %u: relaying", sid);
    relay(incoming, data);
    log_msg("[S] Session %u: done", sid);

    close_socket(data);
    close_socket(incoming);
    g_active_threads--;
    return nullptr;
}

// Thread: handles one client control connection on the server
struct ServerClientCtx {
    int         control_sock;
    sockaddr_in client_addr;
};

static void* server_client_handler(void* param) {
    g_active_threads++;

    ServerClientCtx* sctx = (ServerClientCtx*)param;
    int control = sctx->control_sock;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    delete sctx;

    log_msg("[S] Client connected from %s", client_ip);

    set_socket_timeouts(control, g_settings.recv_timeout, g_settings.recv_timeout);

    pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;

    // Phase 1: Receive FORWARD rules, then READY
    std::vector<pthread_t> listener_tids;
    std::vector<ServerForwardCtx*> listener_ctxs;

    while (true) {
        std::string line;
        if (!recv_line(control, line)) {
            log_msg("[S] Client %s disconnected during setup", client_ip);
            goto cleanup;
        }

        line = trim(line);
        if (line == "READY") break;

        if (line.find("FORWARD ") == 0) {
            std::istringstream iss(line.substr(8));
            int rport;
            std::string lhost;
            int lport;
            iss >> rport >> lhost >> lport;

            if (rport <= 0 || rport > 65535) {
                pthread_mutex_lock(&control_mutex);
                char msg[256];
                snprintf(msg, sizeof(msg), "ERROR %d invalid_port\n", rport);
                send_all(control, msg);
                pthread_mutex_unlock(&control_mutex);
                continue;
            }

            log_msg("[S] Client %s requests forward on port %d -> %s:%d",
                    client_ip, rport, lhost.c_str(), lport);

            ServerForwardCtx* fctx = new ServerForwardCtx;
            fctx->remote_port   = rport;
            fctx->control_sock  = control;
            fctx->control_mutex = &control_mutex;
            fctx->listen_sock   = -1;

            pthread_t tid;
            if (pthread_create(&tid, nullptr, server_forward_listener, fctx) == 0) {
                listener_tids.push_back(tid);
                listener_ctxs.push_back(fctx);
            } else {
                log_msg("[S] Failed to create listener thread for port %d", rport);
                delete fctx;
                pthread_mutex_lock(&control_mutex);
                char msg[256];
                snprintf(msg, sizeof(msg), "ERROR %d thread_failed\n", rport);
                send_all(control, msg);
                pthread_mutex_unlock(&control_mutex);
            }
        }
    }

    log_msg("[S] Client %s: all rules registered, %d listeners active",
            client_ip, (int)listener_tids.size());

    // Phase 2: Keep control connection alive
    {
        set_socket_timeouts(control, 0, 0);

        char buf[1];
        while (true) {
            ssize_t n = recv(control, buf, 1, 0);
            if (n <= 0) break;
        }
    }

    log_msg("[S] Client %s disconnected", client_ip);

cleanup:
    close_socket(control);

    // Close all forwarded listening sockets to unblock accept() and free ports
    for (size_t i = 0; i < listener_ctxs.size(); i++) {
        if (listener_ctxs[i]->listen_sock >= 0) {
            log_msg("[S] Closing forwarded port %d", listener_ctxs[i]->remote_port);
            shutdown(listener_ctxs[i]->listen_sock, SHUT_RDWR);
            close(listener_ctxs[i]->listen_sock);
            listener_ctxs[i]->listen_sock = -1;
        }
    }

    // Wait for listener threads to exit
    for (size_t i = 0; i < listener_tids.size(); i++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 5;
        pthread_timedjoin_np(listener_tids[i], nullptr, &ts);
    }

    // Free listener contexts
    for (size_t i = 0; i < listener_ctxs.size(); i++) {
        delete listener_ctxs[i];
    }

    pthread_mutex_destroy(&control_mutex);
    g_active_threads--;
    return nullptr;
}

// Server main
static int run_server() {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        perror("socket() failed");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_settings.listen_port);
    inet_pton(AF_INET, g_settings.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) < 0) {
        perror("listen() failed");
        close(listen_sock);
        return 1;
    }

    log_msg("[S] Server listening on %s:%d",
            g_settings.listen_address.c_str(), g_settings.listen_port);

    while (true) {
        sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            log_msg("[S] accept() failed: %s", strerror(errno));
            continue;
        }

        set_socket_timeouts(client_sock, g_settings.recv_timeout, g_settings.recv_timeout);

        std::string first_line;
        if (!recv_line(client_sock, first_line)) {
            close(client_sock);
            continue;
        }
        first_line = trim(first_line);

        if (first_line.find("DATA ") == 0) {
            unsigned int session_id = (unsigned int)atoi(first_line.substr(5).c_str());

            pthread_mutex_lock(&g_session_mutex);
            auto it = g_pending_sessions.find(session_id);
            int incoming = -1;
            if (it != g_pending_sessions.end()) {
                incoming = it->second;
                g_pending_sessions.erase(it);
            }
            pthread_mutex_unlock(&g_session_mutex);

            if (incoming < 0) {
                log_msg("[S] Unknown session %u, closing data channel", session_id);
                close(client_sock);
                continue;
            }

            log_msg("[S] Data channel established for session %u", session_id);

            DataRelayCtx* dctx = new DataRelayCtx;
            dctx->data_sock     = client_sock;
            dctx->incoming_sock = incoming;
            dctx->session_id    = session_id;

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            if (pthread_create(&tid, &attr, data_relay_thread, dctx) != 0) {
                log_msg("[S] Failed to create relay thread for session %u", session_id);
                delete dctx;
                close(client_sock);
                close(incoming);
            }

            pthread_attr_destroy(&attr);
        }
        else if (first_line == "CONTROL") {
            ServerClientCtx* sctx = new ServerClientCtx;
            sctx->control_sock = client_sock;
            sctx->client_addr  = client_addr;

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            if (pthread_create(&tid, &attr, server_client_handler, sctx) != 0) {
                log_msg("[S] Failed to create client handler thread");
                delete sctx;
                close(client_sock);
            }

            pthread_attr_destroy(&attr);
        }
        else {
            log_msg("[S] Unknown first line: %s", first_line.c_str());
            close(client_sock);
        }
    }

    close(listen_sock);
    return 0;
}

// ===================================================================
//  CLIENT MODE
// ===================================================================

struct ClientTunnelCtx {
    std::string  server_address;
    int          server_port;
    unsigned int session_id;
    std::string  local_host;
    int          local_port;
    int          remote_port;
};

static void* client_tunnel_thread(void* param) {
    g_active_threads++;

    ClientTunnelCtx* ctx = (ClientTunnelCtx*)param;
    std::string srv_addr = ctx->server_address;
    int srv_port         = ctx->server_port;
    unsigned int sid     = ctx->session_id;
    std::string lhost    = ctx->local_host;
    int lport            = ctx->local_port;
    int rport            = ctx->remote_port;
    delete ctx;

    log_msg("[C] Session %u (port %d): connecting to local %s:%d",
            sid, rport, lhost.c_str(), lport);

    int local_sock = connect_to_host(lhost.c_str(), lport);
    if (local_sock < 0) {
        log_msg("[C] Session %u: cannot connect to local %s:%d", sid, lhost.c_str(), lport);
        g_active_threads--;
        return nullptr;
    }

    int data_sock = connect_to_host(srv_addr.c_str(), srv_port);
    if (data_sock < 0) {
        log_msg("[C] Session %u: cannot connect data channel to server", sid);
        close_socket(local_sock);
        g_active_threads--;
        return nullptr;
    }

    char data_msg[128];
    snprintf(data_msg, sizeof(data_msg), "DATA %u\n", sid);
    if (!send_all(data_sock, data_msg)) {
        log_msg("[C] Session %u: failed to send DATA handshake", sid);
        close_socket(data_sock);
        close_socket(local_sock);
        g_active_threads--;
        return nullptr;
    }

    log_msg("[C] Session %u: tunnel active (server:%d <-> %s:%d)",
            sid, rport, lhost.c_str(), lport);

    relay(local_sock, data_sock);

    log_msg("[C] Session %u: tunnel closed", sid);

    close_socket(data_sock);
    close_socket(local_sock);
    g_active_threads--;
    return nullptr;
}

static int run_client() {
    if (g_settings.rules.empty()) {
        fprintf(stderr, "No forwarding rules defined.\n");
        return 1;
    }

    log_msg("[C] Connecting to server %s:%d ...",
            g_settings.server_address.c_str(), g_settings.server_port);

    int control = connect_to_host(g_settings.server_address.c_str(),
                                  g_settings.server_port);
    if (control < 0) {
        fprintf(stderr, "Cannot connect to server %s:%d\n",
                g_settings.server_address.c_str(), g_settings.server_port);
        return 1;
    }

    log_msg("[C] Connected to server");

    if (!send_all(control, "CONTROL\n")) {
        fprintf(stderr, "Failed to send CONTROL handshake\n");
        close_socket(control);
        return 1;
    }

    std::map<int, ForwardRule> rule_map;

    for (const auto& rule : g_settings.rules) {
        char msg[512];
        snprintf(msg, sizeof(msg), "FORWARD %d %s %d\n",
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

    if (!send_all(control, "READY\n")) {
        fprintf(stderr, "Failed to send READY\n");
        close_socket(control);
        return 1;
    }

    log_msg("[C] All rules sent, waiting for server responses...");

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
            std::istringstream iss(line.substr(8));
            int rport;
            unsigned int sid;
            iss >> rport >> sid;

            log_msg("[C] Server requests tunnel: port %d, session %u", rport, sid);

            auto it = rule_map.find(rport);
            if (it == rule_map.end()) {
                log_msg("[C] No rule for port %d, ignoring", rport);
                continue;
            }

            ClientTunnelCtx* ctx = new ClientTunnelCtx;
            ctx->server_address = g_settings.server_address;
            ctx->server_port    = g_settings.server_port;
            ctx->session_id     = sid;
            ctx->local_host     = it->second.local_host;
            ctx->local_port     = it->second.local_port;
            ctx->remote_port    = rport;

            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            if (pthread_create(&tid, &attr, client_tunnel_thread, ctx) != 0) {
                log_msg("[C] Failed to create tunnel thread for session %u", sid);
                delete ctx;
            }

            pthread_attr_destroy(&attr);
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

    signal(SIGPIPE, SIG_IGN);

    if (!load_settings(settings_path)) {
        return 1;
    }

    if (!g_settings.log_file.empty()) {
        g_log_fp = fopen(g_settings.log_file.c_str(), "a");
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    int ret;
    if (g_settings.mode == "server") {
        log_msg("=== Reverse Port Forwarder — SERVER mode ===");
        ret = run_server();
    } else if (g_settings.mode == "client") {
        log_msg("=== Reverse Port Forwarder — CLIENT mode ===");
        while (true) {
            ret = run_client();
            log_msg("[C] Reconnecting in %d seconds...", g_settings.reconnect_interval);
            sleep(g_settings.reconnect_interval);
        }
    } else {
        fprintf(stderr, "Error: mode must be 'server' or 'client' (got '%s')\n",
                g_settings.mode.c_str());
        ret = 1;
    }

    if (g_log_fp) fclose(g_log_fp);
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_session_mutex);
    return ret;
}
