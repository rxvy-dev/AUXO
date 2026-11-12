#ifndef AUXO_SESSION_H
#define AUXO_SESSION_H
/* The desktop session's environment and D-Bus bus (kern/session.c). Who the user is: cred.h. */
#include <stddef.h>

const char *session_runtime_dir(void);       /* /run/user/UID */
const char *session_bus_env(void);           /* "DBUS_SESSION_BUS_ADDRESS=…", starting the bus if need be */
int         session_env(char *buf, size_t n, char **env, int max, int as_root, const char *pwd);

#endif
