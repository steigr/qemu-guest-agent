#ifndef QGA_INSTALL_H
#define QGA_INSTALL_H

#define QGA_DEFAULT_LOG "/var/adm/qemu-guest-agent.log"
#define QGA_PIDFILE "/var/run/qemu-guest-agent.pid"

/* Detaches from the terminal (fork, setsid, chdir /, stdio to /dev/null) and writes pidfile. */
int qga_daemonize(const char *pidfile);

/* Installs the binary into /usr/local/libexec, an rc script started after vioser and stopped before
 * it, and a chkconfig flag. Returns 0 on success. Re-running upgrades in place. */
int qga_install(void);
int qga_uninstall(void);

/* The rc script --install writes, for a package to carry. */
void qga_print_rc_script(void);

#endif
