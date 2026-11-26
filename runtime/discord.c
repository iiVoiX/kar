// Discord status (RELEASE_PLAN 17.14): "Playing <the Discord application's name>" with the play time, through the
// Discord client's local IPC; no Discord SDK. The client listens on a named pipe \\.\pipe\discord-ipc-<n> (Windows)
// or a Unix socket <runtime dir>/discord-ipc-<n> (n 0..9). Each message is a frame: opcode (u32 LE), length (u32 LE),
// JSON. Opcode 0 is the handshake ({"v":1,"client_id":<app id>}), opcode 1 a command: SET_ACTIVITY sets the status.
//
// TOTK_DISCORD (default 1, the Game tab, live): 0 clears the status and disconnects. TOTK_DISCORD_APP_ID: the id of a
// Discord application (discord.com/developers) whose name Discord shows; its art asset named "icon" is the picture.
// Unset, nothing connects. The thread retries every 15 s, so a Discord client started later is picked up.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"

#ifdef _WIN32
#include <windows.h>
typedef HANDLE Conn;
#define CONN_NONE INVALID_HANDLE_VALUE
static Conn conn_open(int n) {
    char name[64];
    snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", n);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}
static int conn_write(Conn c, const void *p, uint32_t n) {
    DWORD w = 0;
    return WriteFile(c, p, n, &w, NULL) && w == n;
}
// Reads what the client sent so far (its replies are not needed) so the pipe never fills; 0 when the pipe broke.
static int conn_drain(Conn c) {
    char buf[1024];
    DWORD avail = 0, got = 0;
    while (PeekNamedPipe(c, NULL, 0, NULL, &avail, NULL)) {
        if (!avail) return 1;
        if (!ReadFile(c, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL)) return 0;
    }
    return 0;
}
static void conn_close(Conn c) { CloseHandle(c); }
static void nap(int ms) { Sleep((DWORD)ms); }
static int self_pid(void) { return (int)GetCurrentProcessId(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>
typedef int Conn;
#define CONN_NONE (-1)
static Conn conn_open(int n) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-%d", dir, n);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return CONN_NONE;
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return CONN_NONE; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}
static int conn_write(Conn c, const void *p, uint32_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = send(c, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EAGAIN) { usleep(1000); continue; }
        if (w <= 0) return 0;
        b += w, n -= (uint32_t)w;
    }
    return 1;
}
static int conn_drain(Conn c) {
    char buf[1024];
    for (;;) {
        ssize_t r = recv(c, buf, sizeof buf, 0);
        if (r > 0) continue;
        return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}
static void conn_close(Conn c) { close(c); }
static void nap(int ms) { usleep((useconds_t)ms * 1000); }
static int self_pid(void) { return (int)getpid(); }
#endif

static int send_frame(Conn c, uint32_t op, const char *json) {
    uint32_t len = (uint32_t)strlen(json);
    uint8_t h[8] = {(uint8_t)op, (uint8_t)(op >> 8), (uint8_t)(op >> 16), (uint8_t)(op >> 24),
                    (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    return conn_write(c, h, 8) && conn_write(c, json, len);
}

// An application id is a decimal snowflake; anything else is not sent (it goes into JSON unquoted-safe only then).
static int app_id_ok(const char *id) {
    if (!id || !*id || strlen(id) > 24) return 0;
    for (const char *p = id; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static Conn connect_client(const char *id) {
    for (int n = 0; n < 10; n++) {
        Conn c = conn_open(n);
        if (c == CONN_NONE) continue;
        char hs[96];
        snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", id);
        if (send_frame(c, 0, hs)) return c;
        conn_close(c);
    }
    return CONN_NONE;
}

static int set_activity(Conn c, long long start, int on) {
    static unsigned nonce;
    char js[512];
    if (on)
        snprintf(js, sizeof js,
                 "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%d,\"activity\":{"
                 "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"icon\",\"large_text\":\"%s\"}}}}",
                 ++nonce, self_pid(), start, rt_game_is_kar() ? "Kirby Air Riders" : "Donkey Kong Bananza");
    else
        snprintf(js, sizeof js, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%u\",\"args\":{\"pid\":%d}}", ++nonce, self_pid());
    return send_frame(c, 1, js);
}

static void *discord_main(void *arg) {
    (void)arg;
    const long long start = (long long)time(NULL);
    Conn c = CONN_NONE;
    int told = 0;
    for (;;) {
        const char *id = cfg_get("TOTK_DISCORD_APP_ID");
        int want = cfg_bool("TOTK_DISCORD") && app_id_ok(id);
        if (c != CONN_NONE && (!want || !conn_drain(c))) {   // switched off, or the client went away
            if (want == 0) set_activity(c, start, 0);
            conn_close(c), c = CONN_NONE;
        }
        if (want && c == CONN_NONE) {
            c = connect_client(id);
            if (c != CONN_NONE && !set_activity(c, start, 1)) conn_close(c), c = CONN_NONE;
            if (c != CONN_NONE) rt_log("discord: status set (application %s)", id);
            else if (!told++) rt_log("discord: no Discord client found; trying again every 15 s");
        }
        nap(c == CONN_NONE && want ? 15000 : 2000);
    }
    return NULL;
}

// Starts the status thread once (gfx_window_win.c, when the game window opens).
void discord_start(void) {
    static int started;
    if (started++) return;
    if (!app_id_ok(cfg_get("TOTK_DISCORD_APP_ID"))) return;   // nothing to show without an application
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)discord_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (!pthread_create(&t, NULL, discord_main, NULL)) pthread_detach(t);
#endif
}
