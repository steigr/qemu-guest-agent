/* system.c - read-only system information plus time and shutdown. */
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/raw.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <sys/statvfs.h>
#include <sys/sysget.h>
#include <sys/sysmp.h>
#include <sys/syssgi.h>
#include <sys/utsname.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#define NS_PER_SEC 1000000000LL
#define IFCONFIG "/usr/etc/ifconfig"
#define SHUTDOWN "/etc/shutdown"
#define AVENRUN_SCALE 1024.0

static int cmd_host_name(const json *args, json **ret, char *err, size_t errlen)
{
    char name[256];
    json *o;

    (void)args;
    if (gethostname(name, sizeof name) < 0)
        return qga_seterr(err, errlen, "gethostname failed: %s", strerror(errno));
    name[sizeof name - 1] = '\0';
    o = json_obj();
    json_obj_set(o, "host-name", json_str(name));
    *ret = o;
    return 0;
}

static int cmd_get_time(const json *args, json **ret, char *err, size_t errlen)
{
    struct timeval tv;

    (void)args;
    if (gettimeofday(&tv, NULL) < 0)
        return qga_seterr(err, errlen, "gettimeofday failed: %s", strerror(errno));
    *ret = json_int((qga_i64)tv.tv_sec * NS_PER_SEC + (qga_i64)tv.tv_usec * 1000);
    return 0;
}

static int cmd_set_time(const json *args, json **ret, char *err, size_t errlen)
{
    qga_i64 ns;
    struct timeval tv;

    (void)ret;
    if (!args || !json_obj_get(args, "time"))
        return qga_seterr(err, errlen, "setting the time from the hardware clock is not supported");
    if (qga_need_int(args, "time", &ns, err, errlen))
        return 1;
    tv.tv_sec = (time_t)(ns / NS_PER_SEC);
    tv.tv_usec = (long)((ns % NS_PER_SEC) / 1000);
    if (settimeofday(&tv) < 0)
        return qga_seterr(err, errlen, "settimeofday failed: %s", strerror(errno));
    return 0;
}

static int cmd_get_timezone(const json *args, json **ret, char *err, size_t errlen)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    json *o;

    (void)args;
    if (!tm)
        return qga_seterr(err, errlen, "localtime failed");
    o = json_obj();
    json_obj_set(o, "zone", json_str(tzname[tm->tm_isdst > 0]));
    json_obj_set(o, "offset", json_int(-(qga_i64)(tm->tm_isdst > 0 ? altzone : timezone)));
    *ret = o;
    return 0;
}

static int cmd_get_load(const json *args, json **ret, char *err, size_t errlen)
{
    sgt_cookie_t ck;
    int av[3];
    json *o;

    (void)args;
    SGT_COOKIE_INIT(&ck);
    SGT_COOKIE_SET_KSYM(&ck, KSYM_AVENRUN);
    if (sysget(SGT_KSYM, (char *)av, sizeof av, SGT_READ, &ck) < 0)
        return qga_seterr(err, errlen, "sysget avenrun failed: %s", strerror(errno));
    o = json_obj();
    json_obj_set(o, "load1m", json_real((double)av[0] / AVENRUN_SCALE));
    json_obj_set(o, "load5m", json_real((double)av[1] / AVENRUN_SCALE));
    json_obj_set(o, "load15m", json_real((double)av[2] / AVENRUN_SCALE));
    *ret = o;
    return 0;
}

static int cmd_get_osinfo(const json *args, json **ret, char *err, size_t errlen)
{
    char version[256], pretty[300];
    struct utsname un;
    json *o = json_obj();

    (void)args; (void)err; (void)errlen;
    /* uname(2) says 6.5 on every 6.5.x; this is what uname -R adds. */
    if (syssgi(SGI_RELEASE_NAME, sizeof version, version) != 0)
        version[0] = '\0';
    version[sizeof version - 1] = '\0';
    sprintf(pretty, "IRIX %s", version);
    json_obj_set(o, "name", json_str("IRIX"));
    json_obj_set(o, "id", json_str("irix"));
    json_obj_set(o, "pretty-name", json_str(pretty));
    json_obj_set(o, "version", json_str(version));
    json_obj_set(o, "version-id", json_str(version));
    if (uname(&un) >= 0) {
        json_obj_set(o, "kernel-release", json_str(un.release));
        json_obj_set(o, "kernel-version", json_str(un.version));
        json_obj_set(o, "machine", json_str(un.machine));
    }
    *ret = o;
    return 0;
}

static int cmd_get_vcpus(const json *args, json **ret, char *err, size_t errlen)
{
    int ncpu = (int)sysmp(MP_NPROCS), online = (int)sysmp(MP_NAPROCS), k;
    json *a = json_arr();

    (void)args; (void)err; (void)errlen;
    if (ncpu < 1)
        ncpu = 1;
    for (k = 0; k < ncpu; k++) {
        json *c = json_obj();

        json_obj_set(c, "logical-id", json_int(k));
        json_obj_set(c, "online", json_bool(online < 1 || k < online));
        json_obj_set(c, "can-offline", json_bool(0));
        json_arr_add(a, c);
    }
    *ret = a;
    return 0;
}

static int cmd_get_fsinfo(const json *args, json **ret, char *err, size_t errlen)
{
    FILE *fp = setmntent(MOUNTED, "r");
    struct mntent *m;
    json *a;

    (void)args;
    if (!fp)
        return qga_seterr(err, errlen, "cannot read %s: %s", MOUNTED, strerror(errno));
    a = json_arr();
    while ((m = getmntent(fp)) != NULL) {
        const char *dev = strrchr(m->mnt_fsname, '/');
        struct statvfs64 vfs;
        json *e;

        /* Synthetic filesystems carry no data worth reporting; /ns is nsd's. */
        if (strcmp(m->mnt_type, "hwgfs") == 0 || strcmp(m->mnt_type, "proc") == 0 ||
            strcmp(m->mnt_type, "fd") == 0 || strcmp(m->mnt_type, "autofs") == 0 ||
            strcmp(m->mnt_dir, "/ns") == 0)
            continue;
        e = json_obj();
        json_obj_set(e, "name", json_str(dev && dev[1] ? dev + 1 : m->mnt_fsname));
        json_obj_set(e, "mountpoint", json_str(m->mnt_dir));
        json_obj_set(e, "type", json_str(m->mnt_type));
        /* An NFS server that has gone would hang here, but so would everything else on it. */
        if (statvfs64(m->mnt_dir, &vfs) == 0) {
            qga_i64 bs = (qga_i64)vfs.f_frsize;

            json_obj_set(e, "used-bytes", json_int(((qga_i64)vfs.f_blocks - (qga_i64)vfs.f_bfree) * bs));
            json_obj_set(e, "total-bytes", json_int((qga_i64)vfs.f_blocks * bs));
        }
        json_obj_set(e, "disk", json_arr());
        json_arr_add(a, e);
    }
    endmntent(fp);
    *ret = a;
    return 0;
}

static json *find_iface(json *list, const char *name)
{
    size_t k;

    for (k = 0; k < list->n; k++) {
        json *nm = json_obj_get(list->items[k], "name");

        if (nm && strcmp(nm->s, name) == 0)
            return list->items[k];
    }
    return NULL;
}

static json *iface(json *list, const char *name)
{
    json *ent = find_iface(list, name);

    if (!ent) {
        ent = json_obj();
        json_obj_set(ent, "name", json_str(name));
        json_arr_add(list, ent);
    }
    return ent;
}

static json *iface_addrs(json *ent)
{
    json *addrs = json_obj_get(ent, "ip-addresses");

    if (!addrs) {
        addrs = json_arr();
        json_obj_set(ent, "ip-addresses", addrs);
    }
    return addrs;
}

static int prefix_len(unsigned long mask)
{
    int n = 0;

    mask &= 0xFFFFFFFFUL;
    while (mask & 0x80000000UL) {
        n++;
        mask = (mask << 1) & 0xFFFFFFFFUL;
    }
    return n;
}

/* The link-layer address is on the snoop socket of the interface (snoop(7P)); there is no ioctl for
 * it on an inet socket. A loopback reads as all zeros, which is no address. */
static void add_mac(json *ent, const char *name)
{
    struct sockaddr_raw sr;
    struct ifreq req;
    int s = socket(PF_RAW, SOCK_RAW, RAWPROTO_SNOOP);

    if (s < 0)
        return;
    memset(&sr, 0, sizeof sr);
    sr.sr_family = AF_RAW;
    strncpy(sr.sr_ifname, name, sizeof sr.sr_ifname);
    memset(&req, 0, sizeof req);
    strncpy(req.ifr_name, name, sizeof req.ifr_name);
    if (bind(s, (struct sockaddr *)&sr, sizeof sr) == 0 && ioctl(s, SIOCGIFADDR, &req) == 0) {
        unsigned char *m = (unsigned char *)req.ifr_addr.sa_data;
        char mac[20];

        if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) {
            sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            json_obj_set(ent, "hardware-address", json_str(mac));
        }
    }
    close(s);
}

/* SIOCGIFCONF lists no IPv6, so those addresses come from ifconfig ("inet6 ADDR[%scope] prefixlen
 * N" under each "name: flags=" header). */
static void add_ipv6(json *list)
{
    FILE *fp = popen(IFCONFIG " -a 2>/dev/null", "r");
    char line[512], name[IFNAMSIZ + 1];

    if (!fp)
        return;
    name[0] = '\0';
    while (fgets(line, sizeof line, fp)) {
        if (line[0] != ' ' && line[0] != '\t') {
            char *c = strchr(line, ':');

            name[0] = '\0';
            if (c && (size_t)(c - line) <= IFNAMSIZ) {
                memcpy(name, line, (size_t)(c - line));
                name[c - line] = '\0';
            }
        } else if (name[0]) {
            char *p = strstr(line, "inet6 ");
            char addr[96];
            int prefix = 0, n = 0;
            json *a;

            if (!p)
                continue;
            p += 6;
            while (*p && *p != ' ' && *p != '%' && *p != '\n' && n < (int)sizeof addr - 1)
                addr[n++] = *p++;
            addr[n] = '\0';
            p = strstr(line, "prefixlen ");
            if (p)
                prefix = atoi(p + 10);
            a = json_obj();
            json_obj_set(a, "ip-address-type", json_str("ipv6"));
            json_obj_set(a, "ip-address", json_str(addr));
            json_obj_set(a, "prefix", json_int(prefix));
            json_arr_add(iface_addrs(iface(list, name)), a);
        }
    }
    pclose(fp);
}

static int cmd_network_get_interfaces(const json *args, json **ret, char *err, size_t errlen)
{
    enum { BUFSZ = 16384 };
    struct ifconf ifc;
    char *buf = (char *)qga_xmalloc(BUFSZ);
    char *p;
    int s;
    size_t k;
    json *list = json_arr();

    (void)args;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        free(buf);
        json_free(list);
        return qga_seterr(err, errlen, "socket failed: %s", strerror(errno));
    }
    ifc.ifc_len = BUFSZ;
    ifc.ifc_buf = buf;
    if (ioctl(s, SIOCGIFCONF, &ifc) < 0) {
        int e = errno;
        close(s);
        free(buf);
        json_free(list);
        return qga_seterr(err, errlen, "SIOCGIFCONF failed: %s", strerror(e));
    }

    for (p = buf; p < buf + ifc.ifc_len; p += sizeof(struct ifreq)) {
        struct ifreq *ifr = (struct ifreq *)p;
        char name[IFNAMSIZ + 1];
        json *ent;

        memcpy(name, ifr->ifr_name, IFNAMSIZ);
        name[IFNAMSIZ] = '\0';
        ent = iface(list, name);
        if (ifr->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
            struct ifreq req;
            json *a = json_obj();

            json_obj_set(a, "ip-address-type", json_str("ipv4"));
            json_obj_set(a, "ip-address", json_str(inet_ntoa(sin->sin_addr)));
            memset(&req, 0, sizeof req);
            memcpy(req.ifr_name, ifr->ifr_name, IFNAMSIZ);
            memcpy(&req.ifr_addr, &ifr->ifr_addr, sizeof(struct sockaddr_in));
            if (ioctl(s, SIOCGIFNETMASK, &req) == 0)
                json_obj_set(a, "prefix",
                             json_int(prefix_len(((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr)));
            json_arr_add(iface_addrs(ent), a);
        }
    }
    close(s);
    free(buf);
    add_ipv6(list);
    for (k = 0; k < list->n; k++)
        add_mac(list->items[k], json_obj_get(list->items[k], "name")->s);
    *ret = list;
    return 0;
}

static int cmd_get_users(const json *args, json **ret, char *err, size_t errlen)
{
    struct utmpx *ut;
    json *a = json_arr();

    (void)args; (void)err; (void)errlen;
    setutxent();
    while ((ut = getutxent()) != NULL) {
        char user[sizeof ut->ut_user + 1];
        json *e;

        if (ut->ut_type != USER_PROCESS || ut->ut_user[0] == '\0')
            continue;
        memcpy(user, ut->ut_user, sizeof ut->ut_user);
        user[sizeof ut->ut_user] = '\0';
        e = json_obj();
        json_obj_set(e, "user", json_str(user));
        json_obj_set(e, "login-time", json_real((double)ut->ut_tv.tv_sec));
        json_arr_add(a, e);
    }
    endutxent();
    *ret = a;
    return 0;
}

static int cmd_shutdown(const json *args, json **ret, char *err, size_t errlen)
{
    const char *mode = "powerdown";
    const char *state, *power = NULL;
    pid_t pid;

    (void)ret;
    if (qga_opt_str(args, "mode", &mode, err, errlen))
        return 1;
    if (strcmp(mode, "powerdown") == 0) {
        state = "-i0";
        power = "-p";
    } else if (strcmp(mode, "halt") == 0) {
        state = "-i0";
    } else if (strcmp(mode, "reboot") == 0) {
        state = "-i6";
    } else {
        return qga_seterr(err, errlen, "mode is invalid (valid values are: halt|powerdown|reboot)");
    }
    pid = fork();
    if (pid < 0)
        return qga_seterr(err, errlen, "fork failed: %s", strerror(errno));
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);

        setsid();
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execl(SHUTDOWN, "shutdown", "-y", "-g0", state, power, (char *)NULL);
        _exit(127);
    }
    return 0;           /* QGA_NOREPLY: the guest goes down without answering */
}

void qga_system_register(void)
{
    qga_register("guest-get-host-name", cmd_host_name);
    qga_register("guest-get-hostname", cmd_host_name);
    qga_register("guest-get-time", cmd_get_time);
    qga_register_ex("guest-set-time", cmd_set_time, QGA_BLOCK_FROZEN);
    qga_register("guest-get-timezone", cmd_get_timezone);
    qga_register("guest-get-load", cmd_get_load);
    qga_register("guest-get-osinfo", cmd_get_osinfo);
    qga_register("guest-get-vcpus", cmd_get_vcpus);
    qga_register("guest-get-fsinfo", cmd_get_fsinfo);
    qga_register("guest-network-get-interfaces", cmd_network_get_interfaces);
    qga_register("guest-get-users", cmd_get_users);
    qga_register_ex("guest-shutdown", cmd_shutdown, QGA_NOREPLY | QGA_BLOCK_FROZEN);
}
