#include "app.h"

#include <openssl/evp.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int websocket_accept_value(const char *key, char *out, size_t out_size) {
    char input[256];
#ifdef __APPLE__
    unsigned char sha1[CC_SHA1_DIGEST_LENGTH];
#else
    unsigned char sha1[SHA_DIGEST_LENGTH];
#endif
    int len = snprintf(input, sizeof(input), "%s%s", key, WS_GUID);
    if (len < 0 || len >= (int)sizeof(input)) return -1;

#ifdef __APPLE__
    CC_SHA1((const unsigned char *)input, (CC_LONG)strlen(input), sha1);
#else
    SHA1((const unsigned char *)input, strlen(input), sha1);
#endif

    if (out_size < 29) return -1;
    EVP_EncodeBlock((unsigned char *)out, sha1, sizeof(sha1));
    return 0;
}

int ws_is_upgrade_request(const char *request) {
    char upgrade[128], key[256];
    return get_header_value(request, "Upgrade", upgrade, sizeof(upgrade)) == 0 &&
           get_header_value(request, "Sec-WebSocket-Key", key, sizeof(key)) == 0 &&
           strcasecmp(upgrade, "websocket") == 0;
}

static int send_handshake(int fd, const char *request) {
    char key[256], accept[128], response[512];
    if (get_header_value(request, "Sec-WebSocket-Key", key, sizeof(key)) != 0) {
        send_error_response(fd, 400, "Bad Request", "Missing Sec-WebSocket-Key.");
        return -1;
    }
    if (websocket_accept_value(key, accept, sizeof(accept)) != 0) return -1;
    int len = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept
    );
    return len > 0 && len < (int)sizeof(response) ? send_all(fd, response, (size_t)len) : -1;
}

static void get_name(const char *path, char *name, size_t name_size) {
    snprintf(name, name_size, "anon");
    const char *start = strstr(path, "name=");
    if (!start) return;
    snprintf(name, name_size, "%.*s", (int)strcspn(start + 5, "&"), start + 5);
    if (name[0] == '\0') snprintf(name, name_size, "anon");
}

void ws_clients_init(WsClient *clients) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].name[0] = '\0';
    }
}

static int add_client(WsClient *clients, int fd, const char *name) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            snprintf(clients[i].name, sizeof(clients[i].name), "%s", name);
            return 0;
        }
    }
    return -1;
}

void ws_remove_client(WsClient *clients, int idx) {
    if (clients[idx].fd < 0) return;
    close(clients[idx].fd);
    clients[idx].fd = -1;
    clients[idx].name[0] = '\0';
}

int ws_send_text(int fd, const char *text) {
    size_t len = strlen(text);
    unsigned char header[4] = {0x81, 0, 0, 0};
    size_t header_len = 2;
    if (len <= 125) {
        header[1] = (unsigned char)len;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (unsigned char)(len >> 8);
        header[3] = (unsigned char)len;
        header_len = 4;
    } else {
        return -1;
    }
    return send_all(fd, header, header_len) == 0 && send_all(fd, text, len) == 0 ? 0 : -1;
}

static void broadcast(WsClient *clients, const char *text) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (clients[i].fd >= 0 && ws_send_text(clients[i].fd, text) < 0) {
            ws_remove_client(clients, i);
        }
    }
}

static int read_payload_len(int fd, unsigned char byte, unsigned long long *len) {
    *len = byte & 0x7F;
    if (*len == 126) {
        unsigned char x[2];
        if (recv(fd, x, 2, MSG_WAITALL) != 2) return -1;
        *len = ((unsigned long long)x[0] << 8) | x[1];
    } else if (*len == 127) return -1;
    return 0;
}

int ws_handle_frame(WsClient *clients, int idx) {
    unsigned char header[2], mask[4];
    unsigned long long len = 0;
    int fd = clients[idx].fd;
    if (recv(fd, header, 2, MSG_WAITALL) != 2) return -1;
    if ((header[1] & 0x80) == 0) return -1;
    if (read_payload_len(fd, header[1], &len) < 0 || len > MAX_WS_PAYLOAD) return -1;
    if (recv(fd, mask, 4, MSG_WAITALL) != 4) return -1;
    char *msg = malloc((size_t)len + 1);
    if (!msg) return -1;
    if (recv(fd, msg, (size_t)len, MSG_WAITALL) != (ssize_t)len) {
        free(msg);
        return -1;
    }
    for (unsigned long long i = 0; i < len; i++) msg[i] ^= mask[i % 4];
    msg[len] = '\0';
    unsigned char opcode = header[0] & 0x0F;
    if (opcode == 0x1) {
        char line[MAX_NAME_LEN + 2 + MAX_WS_PAYLOAD + 1];
        chat_db_insert_message(clients[idx].name, msg);
        snprintf(line, sizeof(line), "%s: %s", clients[idx].name, msg);
        broadcast(clients, line);
    }
    free(msg);
    return opcode == 0x8 ? -1 : 0;
}

int ws_is_path(const char *path) {
    return strncmp(path, "/ws", 3) == 0;
}

int ws_upgrade(int fd, const char *request, const char *path, WsClient *clients) {
    char name[MAX_NAME_LEN];
    get_name(path, name, sizeof(name));
    if (send_handshake(fd, request) < 0) return -1;
    if (add_client(clients, fd, name) < 0) return -1;
    return chat_db_send_recent_messages(fd, HISTORY_LIMIT);
}
