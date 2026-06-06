#include "app.h"

#include <errno.h>
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
#include <time.h>
#include <unistd.h>

static const char *WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const double CLIENT_MESSAGE_RATE = 2.0;
static const double CLIENT_MESSAGE_BURST = 5.0;
static const double GLOBAL_MESSAGE_RATE = 40.0;
static const double GLOBAL_MESSAGE_BURST = 80.0;
static double global_message_tokens = GLOBAL_MESSAGE_BURST;
static double global_last_token_refill = 0.0;

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

static double monotonic_seconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void refill_tokens(double *tokens, double *last_refill, double rate, double burst, double now) {
    if (*last_refill == 0.0) *last_refill = now;
    *tokens += (now - *last_refill) * rate;
    if (*tokens > burst) *tokens = burst;
    *last_refill = now;
}

static int message_allowed(WsClient *client) {
    double now = monotonic_seconds();
    refill_tokens(
        &client->message_tokens,
        &client->last_token_refill,
        CLIENT_MESSAGE_RATE,
        CLIENT_MESSAGE_BURST,
        now
    );
    refill_tokens(
        &global_message_tokens,
        &global_last_token_refill,
        GLOBAL_MESSAGE_RATE,
        GLOBAL_MESSAGE_BURST,
        now
    );
    if (client->message_tokens < 1.0 || global_message_tokens < 1.0) return 0;
    client->message_tokens -= 1.0;
    global_message_tokens -= 1.0;
    return 1;
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
        clients[i].input_len = 0;
        clients[i].message_tokens = CLIENT_MESSAGE_BURST;
        clients[i].last_token_refill = monotonic_seconds();
    }
}

static int add_client(WsClient *clients, int fd, const char *name) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            snprintf(clients[i].name, sizeof(clients[i].name), "%s", name);
            clients[i].input_len = 0;
            clients[i].message_tokens = CLIENT_MESSAGE_BURST;
            clients[i].last_token_refill = monotonic_seconds();
            return i;
        }
    }
    return -1;
}

void ws_remove_client(WsClient *clients, int idx) {
    if (clients[idx].fd < 0) return;
    close(clients[idx].fd);
    clients[idx].fd = -1;
    clients[idx].name[0] = '\0';
    clients[idx].input_len = 0;
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

static int process_frame(WsClient *clients, int idx, size_t *consumed) {
    WsClient *client = &clients[idx];
    unsigned char *frame = client->input;
    if (client->input_len < 2) return 0;
    if ((frame[0] & 0x80) == 0 || (frame[1] & 0x80) == 0) return -1;

    size_t header_len = 2;
    unsigned long long payload_len = frame[1] & 0x7F;
    if (payload_len == 126) {
        if (client->input_len < 4) return 0;
        payload_len = ((unsigned long long)frame[2] << 8) | frame[3];
        header_len = 4;
    } else if (payload_len == 127) {
        return -1;
    }
    if (payload_len > MAX_WS_PAYLOAD) return -1;

    size_t frame_len = header_len + 4 + (size_t)payload_len;
    if (client->input_len < frame_len) return 0;

    unsigned char opcode = frame[0] & 0x0F;
    unsigned char *mask = frame + header_len;
    unsigned char *payload = frame + header_len + 4;

    if (opcode == 0x1 && payload_len > 0 && message_allowed(client)) {
        char msg[MAX_WS_PAYLOAD + 1];
        char line[MAX_NAME_LEN + 2 + MAX_WS_PAYLOAD + 1];
        for (size_t i = 0; i < (size_t)payload_len; i++) {
            msg[i] = (char)(payload[i] ^ mask[i % 4]);
        }
        msg[payload_len] = '\0';
        if (chat_db_insert_message(client->name, msg) == 0) {
            snprintf(line, sizeof(line), "%s: %s", client->name, msg);
            broadcast(clients, line);
        }
    }
    *consumed = frame_len;
    return opcode == 0x8 ? -1 : 1;
}

int ws_handle_frame(WsClient *clients, int idx) {
    WsClient *client = &clients[idx];
    size_t space = sizeof(client->input) - client->input_len;
    if (space == 0) return -1;

    ssize_t n = recv(client->fd, client->input + client->input_len, space, 0);
    if (n == 0) return -1;
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    client->input_len += (size_t)n;

    for (int frames = 0; frames < 8; frames++) {
        size_t consumed = 0;
        int rc = process_frame(clients, idx, &consumed);
        if (rc < 0) return -1;
        if (rc == 0) break;
        client->input_len -= consumed;
        memmove(client->input, client->input + consumed, client->input_len);
    }
    return 0;
}

int ws_is_path(const char *path) {
    return strncmp(path, "/ws", 3) == 0;
}

int ws_upgrade(int fd, const char *request, const char *path, WsClient *clients) {
    char name[MAX_NAME_LEN];
    get_name(path, name, sizeof(name));
    if (send_handshake(fd, request) < 0) return -1;
    int idx = add_client(clients, fd, name);
    if (idx < 0) return -1;
    if (chat_db_send_recent_messages(fd, HISTORY_LIMIT) < 0) {
        clients[idx].fd = -1;
        clients[idx].name[0] = '\0';
        clients[idx].input_len = 0;
        return -1;
    }
    return 0;
}
