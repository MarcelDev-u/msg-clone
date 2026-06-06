#include "app.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

static const char *route_file(const char *path) {
    if (strcmp(path, "/") == 0) return "/index.html";
    if (strcmp(path, "/chat") == 0) return "/chat.html";
    return NULL;
}

static int parse_request_line(const char *request, char *method, char *path) {
    char version[32];
    return sscanf(request, "%15s %511s %31s", method, path, version) == 3 ? 0 : -1;
}

static void strip_query_string(char *path) {
    char *q = strchr(path, '?');
    if (q) *q = '\0';
}

int get_header_value(const char *request, const char *name, char *out, size_t out_size) {
    size_t name_len = strlen(name);
    for (const char *line = request; *line; ) {
        const char *end = strstr(line, "\r\n");
        if (!end || end == line) return -1;
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (*value == ' ' || *value == '\t') value++;
            size_t len = (size_t)(end - value);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, value, len);
            out[len] = '\0';
            return 0;
        }
        line = end + 2;
    }
    return -1;
}

void send_error_response(int fd, int code, const char *reason, const char *message) {
    char body[256];
    int body_len = snprintf(body, sizeof(body), "%d %s\n%s\n", code, reason, message);
    if (body_len < 0 || body_len >= (int)sizeof(body)) return;

    dprintf(
        fd,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code,
        reason,
        body_len
    );
    send_all(fd, body, (size_t)body_len);
}

static int serve_file(int fd, const char *file_path) {
    char full_path[MAX_PATH_LEN * 2];
    snprintf(full_path, sizeof(full_path), "%s%s", PUBLIC_DIR, file_path);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        send_error_response(fd, 404, "Not Found", "HTML file not found.");
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char *body = malloc((size_t)size);
    if (!body || fread(body, 1, (size_t)size, f) != (size_t)size) {
        free(body);
        fclose(f);
        send_error_response(fd, 500, "Internal Server Error", "Could not read file.");
        return -1;
    }
    fclose(f);

    dprintf(
        fd,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        size
    );
    int ok = send_all(fd, body, (size_t)size);
    free(body);
    return ok;
}

static int listen_on_localhost(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 32) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_new_client(int fd, WsClient *clients) {
    char request[REQUEST_BUF_SIZE], method[16], path[MAX_PATH_LEN];
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        close(fd);
        return;
    }
    request[n] = '\0';
    if (parse_request_line(request, method, path) < 0) {
        send_error_response(fd, 400, "Bad Request", "Invalid request line.");
        close(fd);
        return;
    }
    if (strcmp(method, "GET") != 0) {
        send_error_response(fd, 405, "Method Not Allowed", "Only GET is supported.");
        close(fd);
        return;
    }
    if (ws_is_path(path) && ws_is_upgrade_request(request)) {
        if (set_nonblocking(fd) < 0 || ws_upgrade(fd, request, path, clients) < 0) close(fd);
        return;
    }
    strip_query_string(path);
    const char *file = route_file(path);
    if (file) serve_file(fd, file);
    else send_error_response(fd, 404, "Not Found", "No route matched this URL path.");
    close(fd);
}

static int fill_read_set(int server_fd, WsClient *clients, fd_set *set) {
    int maxfd = server_fd;
    FD_ZERO(set);
    FD_SET(server_fd, set);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (clients[i].fd >= 0) {
            FD_SET(clients[i].fd, set);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }
    }
    return maxfd;
}

static void run_server(int server_fd, WsClient *clients) {
    for (;;) {
        fd_set readfds;
        int maxfd = fill_read_set(server_fd, clients, &readfds);
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select");
            continue;
        }
        if (FD_ISSET(server_fd, &readfds)) {
            int fd = accept(server_fd, NULL, NULL);
            if (fd >= 0) handle_new_client(fd, clients);
        }
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (clients[i].fd >= 0 && FD_ISSET(clients[i].fd, &readfds)) {
                if (ws_handle_frame(clients, i) < 0) ws_remove_client(clients, i);
            }
        }
    }
}

int main(void) {
    WsClient clients[MAX_WS_CLIENTS];
    const char *db_path = getenv("CHAT_DB_PATH");
    if (!db_path) db_path = CHAT_DB_PATH;
    if (chat_db_init(db_path) < 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    ws_clients_init(clients);
    int server_fd = listen_on_localhost();
    if (server_fd < 0) return 1;
    printf("Server running at http://0.0.0.0:%d\n", PORT);
    printf("Chat DB: %s\n", db_path);
    printf("WebSocket chat endpoint: ws://127.0.0.1:%d/ws?name=YOUR_NAME\n", PORT);
    run_server(server_fd, clients);
    close(server_fd);
    chat_db_close();
    return 0;
}
