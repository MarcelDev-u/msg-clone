#include <stddef.h>
#define PORT 8080
#define PUBLIC_DIR "./public"
#define CHAT_DB_PATH "./chat.db"
#define REQUEST_BUF_SIZE 8192
#define MAX_PATH_LEN 512
#define MAX_WS_CLIENTS 128
#define MAX_NAME_LEN 64
#define MAX_WS_PAYLOAD 4096
#define MAX_WS_FRAME_SIZE (MAX_WS_PAYLOAD + 8)
#define HISTORY_LIMIT 50
#define MAX_STORED_MESSAGES 10000
typedef struct {
    int fd;
    char name[MAX_NAME_LEN];
    unsigned char input[MAX_WS_FRAME_SIZE];
    size_t input_len;
    double message_tokens;
    double last_token_refill;
} WsClient;
int chat_db_init(const char *path);
void chat_db_close(void);
int chat_db_insert_message(const char *name, const char *message);
int chat_db_send_recent_messages(int fd, int limit);
int get_header_value(const char *request, const char *name, char *out, size_t out_size);
void send_error_response(int fd, int code, const char *reason, const char *message);
void ws_clients_init(WsClient *clients);
int ws_is_upgrade_request(const char *request);
int ws_is_path(const char *path);
int ws_upgrade(int fd, const char *request, const char *path, WsClient *clients);
int ws_handle_frame(WsClient *clients, int idx);
void ws_remove_client(WsClient *clients, int idx);
int ws_send_text(int fd, const char *text);
