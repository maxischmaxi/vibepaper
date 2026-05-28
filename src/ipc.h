#ifndef BG_IPC_H
#define BG_IPC_H

#include <stddef.h>
#include <stdbool.h>

// Fills `out` with the daemon socket path. Returns 0 / -1.
// Uses $XDG_RUNTIME_DIR/vibepaper.sock when available, falls back to /tmp.
int bg_ipc_socket_path(char *out, size_t out_len);

// Daemon side: bind + listen on the socket. Returns listen fd, -1 on error.
int bg_ipc_server_listen(void);

// Client side: connect to an existing daemon socket. Returns fd, -1 on error.
// Sets errno=ENOENT if there's no daemon yet (caller can spawn one).
int bg_ipc_client_connect(void);

// Send a single JSON-line request and read the JSON-line response. Returns 0
// on success, non-zero on transport error. `response` is malloc'd and the
// caller must free; pass NULL to discard.
int bg_ipc_client_request(int fd, const char *json_request, char **response);

// Read one '\n'-terminated line from fd into a malloc'd buffer (returned via
// *out). Returns 0 on success, -1 on EOF/error.
int bg_ipc_read_line(int fd, char **out);

// Write a single response line ("…\n" is appended automatically). Returns 0/-1.
int bg_ipc_write_line(int fd, const char *s);

// Apply receive/send timeouts (milliseconds; <= 0 leaves the default) to a
// connected socket so a stalled peer can neither block the daemon's single-
// threaded event loop nor hang the CLI forever. Best-effort (errors ignored).
void bg_ipc_set_timeouts(int fd, int recv_ms, int send_ms);

#endif
