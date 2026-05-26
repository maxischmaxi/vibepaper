#ifndef BG_DAEMON_H
#define BG_DAEMON_H

// Run the wayland renderer + ipc listener until SIGTERM/SIGINT or STOP cmd.
// Returns 0 on clean shutdown, non-zero on error.
int bg_daemon_run(void);

// Fork+setsid an instance of ourselves in daemon mode. Returns 0 if the
// daemon is now reachable on the IPC socket, -1 on timeout/error.
int bg_daemon_spawn(const char *self_exe_path);

#endif
