/* netstat.c - guest-network-get-listeners, and a stand-in for `netstat -an -f inet|inet6`, which is
 * too expensive to poll under TCG.
 *
 * On the PowerPC releases netstat looks its symbols up in /mach one at a time, about 11,200 lseek
 * and 11,100 read calls for each run, and takes 2 to 3 s of system time for a table it then gets
 * from a sysctl anyway (live, 10.3.9, 2026-11-05). internal/controllers/guestports asks for both
 * families every 15 s, which held an idle Panther's vCPU at 38% of a core against 8% between polls.
 *
 * guest-exec (commands/exec.c) spots exactly these commands, on their own or inside a /bin/sh -c
 * script, and re-executes this binary as `--applet netstat ...`. The applet reads
 * net.inet.tcp.pcblist and net.inet.udp.pcblist and prints what netstat prints. It reads no raw
 * sockets, so the icm6 row -f inet6 shows is missing, and an IPv6 address is not cut to 16
 * characters, or a scoped one would lose the %lo0 the host looks for. Anything it cannot read as
 * this build's structures -- another release's layout, a sysctl that fails -- runs the real netstat,
 * and nothing is printed before that is settled.
 *
 * guest-network-get-listeners is the same table without the process: every socket that listens,
 * its address and family, beside the guest's interfaces, so that a host can tell a socket on one
 * NIC from a socket on another. A host that knows it no longer needs the stand-in; one that does
 * not, and every script that asks for the table, still gets the stand-in.
 *
 * PowerPC only: the Intel binary is one build for 10.4 to 10.15, whose structures differ, and the
 * cost has only been seen here. */
#include "../core/dispatch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NETSTAT_PATH "/usr/sbin/netstat"

#ifdef __ppc__
#define __APPLE_API_PRIVATE
#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <arpa/inet.h>
#endif

#if defined(__ppc__) && defined(INP_IPV4)
#define NETSTAT_NATIVE 1
#define TCP_PCBLIST "net.inet.tcp.pcblist"
#define UDP_PCBLIST "net.inet.udp.pcblist"

static const char *const tcp_states[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "CLOSE_WAIT",
    "FIN_WAIT_1", "CLOSING", "LAST_ACK", "FIN_WAIT_2", "TIME_WAIT"
};

/* inet_ntop's form of an IPv6 address: the longest run of two or more zero groups, the first of
 * equals, becomes "::". 10.0's and 10.1's libc have no inet_ntop: the agent built there failed to
 * link (bootstrap 2026-11-05). */
static void ntop6(char *out, const struct in6_addr *a)
{
    unsigned g[8];
    int i, best = -1, bestlen = 1, run = -1, runlen = 0;

    for (i = 0; i < 8; i++)
        g[i] = ((unsigned)a->s6_addr[2 * i] << 8) | a->s6_addr[2 * i + 1];
    for (i = 0; i < 8; i++) {
        if (g[i] == 0) {
            if (run < 0)
                run = i, runlen = 0;
            if (++runlen > bestlen)
                best = run, bestlen = runlen;
        } else
            run = -1;
    }
    *out = '\0';
    for (i = 0; i < 8; i++) {
        if (i == best) {
            strcat(out, "::");
            i += bestlen - 1;
            continue;
        }
        if (i && i != best + bestlen)
            strcat(out, ":");
        /* An IPv4-mapped or -compatible tail is dotted, as inet_ntop writes it. */
        if (i == 6 && best == 0 && (bestlen == 6 || (bestlen == 5 && g[5] == 0xffff))) {
            sprintf(out + strlen(out), "%u.%u.%u.%u", a->s6_addr[12], a->s6_addr[13],
                    a->s6_addr[14], a->s6_addr[15]);
            break;
        }
        sprintf(out + strlen(out), "%x", g[i]);
    }
}

/* The name of the interface with this index, from SIOCGIFCONF's link-layer entries: 10.0 and 10.1
 * have no if_indextoname. */
static int ifname_of(unsigned index, char *name)
{
    char buf[8192], *p;
    struct ifconf ifc;
    int s = socket(AF_INET, SOCK_DGRAM, 0), found = 0;

    if (s < 0)
        return 0;
    ifc.ifc_len = sizeof buf;
    ifc.ifc_buf = buf;
    if (ioctl(s, SIOCGIFCONF, &ifc) == 0) {
        for (p = buf; p < buf + ifc.ifc_len && !found;) {
            struct ifreq *r = (struct ifreq *)p;
            size_t len = r->ifr_addr.sa_len > sizeof r->ifr_addr ? r->ifr_addr.sa_len
                                                                  : sizeof r->ifr_addr;

            if (r->ifr_addr.sa_family == AF_LINK &&
                ((struct sockaddr_dl *)&r->ifr_addr)->sdl_index == index) {
                memcpy(name, r->ifr_name, IFNAMSIZ);
                name[IFNAMSIZ - 1] = '\0';
                found = 1;
            }
            p += sizeof r->ifr_name + len;
        }
    }
    close(s);
    return found;
}

/* The address alone, "*" for none. */
static void host_of(char *host, const struct inpcb *inp, int local)
{
    strcpy(host, "*");
    if (inp->inp_vflag & INP_IPV4) {
        struct in_addr a = local ? inp->inp_laddr : inp->inp_faddr;

        if (a.s_addr != INADDR_ANY)
            strcpy(host, inet_ntoa(a));
    } else {
        struct in6_addr a = local ? inp->in6p_laddr : inp->in6p_faddr;

        if (!IN6_IS_ADDR_UNSPECIFIED(&a)) {
            unsigned scope = 0;
            char ifname[IFNAMSIZ];

            /* The kernel keeps a link-local address's scope in its second 16 bits. */
            if (IN6_IS_ADDR_LINKLOCAL(&a)) {
                scope = ((unsigned)a.s6_addr[2] << 8) | a.s6_addr[3];
                a.s6_addr[2] = a.s6_addr[3] = 0;
            }
            ntop6(host, &a);
            if (scope && ifname_of(scope, ifname)) {
                strcat(host, "%");
                strcat(host, ifname);
            }
        }
    }
}

static void put_endpoint(char *out, const struct inpcb *inp, int local)
{
    char host[64 + IFNAMSIZ + 2];
    unsigned port = ntohs(local ? inp->inp_lport : inp->inp_fport);

    host_of(host, inp, local);
    if (port)
        sprintf(out, "%s.%u", host, port);
    else
        sprintf(out, "%s.*", host);
}

/* One socket of a table. state is the TCP state, -1 on UDP. */
typedef void (*socket_fn)(void *ctx, const struct inpcb *inp, const struct xsocket *so, int state);

/* The kernel's table: a struct xinpgen, one record per socket, and a closing xinpgen. A record of
 * any length but this build's is another release's layout, which is not guessed at. */
static int each_socket(const char *oid, socket_fn fn, void *ctx)
{
    int tcp = strcmp(oid, TCP_PCBLIST) == 0, ok = 0;
    size_t len = 0, off;
    char *buf;

    if (sysctlbyname(oid, NULL, &len, NULL, 0) != 0)
        return -1;
    len += len / 2 + 4096;      /* sockets may come and go between the two calls */
    buf = (char *)qga_xmalloc(len);
    if (sysctlbyname(oid, buf, &len, NULL, 0) != 0 || len < sizeof(struct xinpgen) ||
        ((struct xinpgen *)buf)->xig_len != sizeof(struct xinpgen)) {
        free(buf);
        return -1;
    }
    for (off = sizeof(struct xinpgen); off + sizeof(size_t) <= len; ) {
        size_t rec = ((struct xinpgen *)(buf + off))->xig_len;

        if (rec == sizeof(struct xinpgen)) {
            ok = 1;
            break;
        }
        if (off + rec > len || rec != (tcp ? sizeof(struct xtcpcb) : sizeof(struct xinpcb)))
            break;
        if (tcp) {
            const struct xtcpcb *x = (const struct xtcpcb *)(buf + off);

            fn(ctx, &x->xt_inp, &x->xt_socket, x->xt_tp.t_state);
        } else {
            const struct xinpcb *x = (const struct xinpcb *)(buf + off);

            fn(ctx, &x->xi_inp, &x->xi_socket, -1);
        }
        off += rec;
    }
    free(buf);
    return ok ? 0 : -1;
}

typedef struct {
    sbuf *b;
    int want;       /* INP_IPV4 or INP_IPV6 */
} table_ctx;

static void put_row(void *ctx, const struct inpcb *inp, const struct xsocket *so, int state)
{
    table_ctx *t = (table_ctx *)ctx;
    const char *family = (inp->inp_vflag & INP_IPV4)
        ? ((inp->inp_vflag & INP_IPV6) ? "46" : "4") : "6";
    char local[96], foreign[96], line[256], name[8];

    if (!(inp->inp_vflag & t->want))
        return;
    put_endpoint(local, inp, 1);
    put_endpoint(foreign, inp, 0);
    sprintf(name, "%s%s", state >= 0 ? "tcp" : "udp", family);
    sprintf(line, "%-5.5s %6lu %6lu  %-22s %-22s ", name,
            (unsigned long)so->so_rcv.sb_cc, (unsigned long)so->so_snd.sb_cc, local, foreign);
    sb_puts(t->b, line);
    if (state >= 0) {
        if (state < (int)(sizeof tcp_states / sizeof tcp_states[0]))
            sb_puts(t->b, tcp_states[state]);
        else {
            sprintf(line, "%d", state);
            sb_puts(t->b, line);
        }
    }
    sb_putc(t->b, '\n');
}

/* The whole answer into b, or -1 with nothing printed. */
static int native_table(sbuf *b, int inet6)
{
    table_ctx t;

    t.b = b;
    t.want = inet6 ? INP_IPV6 : INP_IPV4;
    sb_puts(b, "Active Internet connections (including servers)\n"
               "Proto Recv-Q Send-Q  Local Address          Foreign Address        (state)\n");
    if (each_socket(TCP_PCBLIST, put_row, &t) != 0 || each_socket(UDP_PCBLIST, put_row, &t) != 0)
        return -1;
    return 0;
}

/* A socket that can be reached from outside it: TCP in LISTEN, and UDP with no peer, UDP having no
 * listening state. Whether its address makes that so is the host's to say, which knows which of
 * the guest's interfaces it is asking about. */
static void add_listener(void *ctx, const struct inpcb *inp, const struct xsocket *so, int state)
{
    json *list = (json *)ctx, *o;
    char host[64 + IFNAMSIZ + 2];

    (void)so;
    if (state >= 0 ? state != TCPS_LISTEN : inp->inp_fport != 0)
        return;
    host_of(host, inp, 1);
    o = json_obj();
    json_obj_set(o, "protocol", json_str(state >= 0 ? "tcp" : "udp"));
    json_obj_set(o, "family", json_str((inp->inp_vflag & INP_IPV4)
        ? ((inp->inp_vflag & INP_IPV6) ? "dual" : "ipv4") : "ipv6"));
    json_obj_set(o, "address", json_str(host));
    json_obj_set(o, "port", json_int(ntohs(inp->inp_lport)));
    json_arr_add(list, o);
}

static int cmd_get_listeners(const json *args, json **ret, char *err, size_t errlen)
{
    json *list = json_arr(), *ifaces = NULL, *o;

    (void)args;
    if (each_socket(TCP_PCBLIST, add_listener, list) != 0 ||
        each_socket(UDP_PCBLIST, add_listener, list) != 0) {
        json_free(list);
        return qga_seterr(err, errlen, "the kernel's socket tables are not this build's");
    }
    if (qga_network_interfaces(&ifaces, err, errlen) != 0) {
        json_free(list);
        return 1;
    }
    o = json_obj();
    json_obj_set(o, "listeners", list);
    json_obj_set(o, "interfaces", ifaces);
    *ret = o;
    return 0;
}
#endif /* NETSTAT_NATIVE */

void qga_netstat_register(void)
{
#ifdef NETSTAT_NATIVE
    qga_register("guest-network-get-listeners", cmd_get_listeners);
#endif
}

int qga_netstat_native(void)
{
#ifdef NETSTAT_NATIVE
    return 1;
#else
    return 0;
#endif
}

/* -1 unless args are exactly `-an -f inet` (0) or `-an -f inet6` (1). */
static int family_of_args(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[0], "-an") != 0 || strcmp(argv[1], "-f") != 0)
        return -1;
    if (strcmp(argv[2], "inet") == 0)
        return 0;
    return strcmp(argv[2], "inet6") == 0 ? 1 : -1;
}

int qga_netstat_args_match(int argc, char **argv)
{
    return family_of_args(argc, argv) >= 0;
}

int qga_applet_netstat(int argc, char **argv)
{
    char **real;
    int k;
#ifdef NETSTAT_NATIVE
    int inet6 = family_of_args(argc, argv);

    if (inet6 >= 0) {
        sbuf b;

        sb_init(&b);
        if (native_table(&b, inet6) == 0) {
            fwrite(b.p, 1, b.len, stdout);
            sb_free(&b);
            return fflush(stdout) == 0 ? 0 : 1;
        }
        sb_free(&b);
    }
#endif
    real = (char **)qga_xmalloc((argc + 2) * sizeof *real);
    real[0] = NETSTAT_PATH;
    for (k = 0; k < argc; k++)
        real[k + 1] = argv[k];
    real[argc + 1] = NULL;
    execv(NETSTAT_PATH, real);
    fprintf(stderr, "netstat: %s: %s\n", NETSTAT_PATH, strerror(errno));
    return 1;
}

static int shell_safe(const char *s)
{
    return s[strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/._-")] == '\0';
}

/* Where a command ends: what follows is no further argument of it. A redirection is the shell's. */
static int command_ends(const char *p)
{
    int spaces = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
        spaces = 1;
    }
    if (*p == '\0' || strchr(";&|)\n", *p))
        return 1;
    return spaces && (*p == '>' || (*p == '2' && p[1] == '>'));
}

char *qga_netstat_rewrite(const char *script, const char *self)
{
    static const char tool[] = NETSTAT_PATH;
    static const char args[] = " -an -f inet";
    const char *p = script, *hit;
    sbuf out;
    int found = 0;

    if (!self[0] || !shell_safe(self))
        return NULL;
    sb_init(&out);
    while ((hit = strstr(p, tool)) != NULL) {
        const char *after = hit + sizeof tool - 1;
        const char *end = after + sizeof args - 1;
        int starts = hit == script || strchr(" \t\n;&|(", hit[-1]) != NULL;

        sb_append(&out, p, (size_t)(hit - p));
        if (starts && strncmp(after, args, sizeof args - 1) == 0) {
            if (*end == '6')
                end++;
            if (command_ends(end)) {
                sb_puts(&out, self);
                sb_puts(&out, " --applet netstat");
                found = 1;
                p = after;
                continue;
            }
        }
        sb_puts(&out, tool);
        p = after;
    }
    if (found) {
        char *r;

        sb_puts(&out, p);
        r = qga_xstrdup(sb_cstr(&out));
        sb_free(&out);
        return r;
    }
    sb_free(&out);
    return NULL;
}
