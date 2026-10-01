#ifndef QGA_INSTALL_H
#define QGA_INSTALL_H

#define QGA_DEFAULT_LOG "/var/log/qemu-guest-agent.log"
#define QGA_PIDFILE "/var/run/qemu-guest-agent.pid"

/* Detaches from the terminal (fork, setsid, chdir /, stdio to /dev/null) and writes pidfile. */
int qga_daemonize(const char *pidfile);

/* Installs the binary into /usr/local/libexec plus a StartupItem (10.0-10.3) or a
 * LaunchDaemon (10.4+). Returns 0 on success. Re-running upgrades in place. */
int qga_install(void);
int qga_uninstall(void);

#endif
