/* system.c - read-only system information plus time and shutdown (plan M2, M5 subset).
 *
 * Everything uses plain BSD interfaces (sysctl, ioctl, getfsstat, utmp) that exist on 10.0. */
#include "../core/dispatch.h"

/* Include order matters on 10.1: its net/if.h and arpa/inet.h are not self-contained. */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <utmp.h>

#define NS_PER_SEC 1000000000LL

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
    if (settimeofday(&tv, NULL) < 0)
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
    json_obj_set(o, "zone", json_str(tm->tm_zone ? tm->tm_zone : "UTC"));
    json_obj_set(o, "offset", json_int(tm->tm_gmtoff));
    *ret = o;
    return 0;
}

static int cmd_get_load(const json *args, json **ret, char *err, size_t errlen)
{
    int mib[2];
    struct loadavg la;
    size_t len = sizeof la;
    json *o;

    (void)args;
    mib[0] = CTL_VM;
    mib[1] = VM_LOADAVG;
    if (sysctl(mib, 2, &la, &len, NULL, 0) < 0 || la.fscale == 0)
        return qga_seterr(err, errlen, "sysctl vm.loadavg failed: %s", strerror(errno));
    o = json_obj();
    json_obj_set(o, "load1m", json_real((double)la.ldavg[0] / (double)la.fscale));
    json_obj_set(o, "load5m", json_real((double)la.ldavg[1] / (double)la.fscale));
    json_obj_set(o, "load15m", json_real((double)la.ldavg[2] / (double)la.fscale));
    *ret = o;
    return 0;
}

/* Finds <key>NAME</key><string>VALUE</string> in a small XML plist. */
static int plist_string(const char *xml, const char *name, char *out, size_t outlen)
{
    char key[96];
    const char *p, *s, *e;

    sprintf(key, "<key>%.60s</key>", name);
    p = strstr(xml, key);
    if (!p)
        return 0;
    s = strstr(p, "<string>");
    if (!s)
        return 0;
    s += 8;
    e = strstr(s, "</string>");
    if (!e || (size_t)(e - s) >= outlen)
        return 0;
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = '\0';
    return 1;
}

static int cmd_get_osinfo(const json *args, json **ret, char *err, size_t errlen)
{
    char *xml = NULL;
    char name[64], version[64], build[64], pretty[160];
    struct utsname un;
    json *o = json_obj();
    FILE *fp;
    size_t n;

    (void)args; (void)err; (void)errlen;
    strcpy(name, "Mac OS X");
    version[0] = build[0] = '\0';
    fp = fopen("/System/Library/CoreServices/SystemVersion.plist", "r");
    if (fp) {
        xml = (char *)qga_xmalloc(32769);
        n = fread(xml, 1, 32768, fp);
        xml[n] = '\0';
        fclose(fp);
        plist_string(xml, "ProductName", name, sizeof name);
        plist_string(xml, "ProductVersion", version, sizeof version);
        plist_string(xml, "ProductBuildVersion", build, sizeof build);
        free(xml);
    }
    sprintf(pretty, "%s %s", name, version);
    json_obj_set(o, "name", json_str(name));
    json_obj_set(o, "id", json_str("macosx"));
    json_obj_set(o, "pretty-name", json_str(pretty));
    json_obj_set(o, "version", json_str(version));
    json_obj_set(o, "version-id", json_str(version));
    if (build[0])
        json_obj_set(o, "variant-id", json_str(build));
    if (uname(&un) == 0) {
        json_obj_set(o, "kernel-release", json_str(un.release));
        json_obj_set(o, "kernel-version", json_str(un.version));
        json_obj_set(o, "machine", json_str(un.machine));
    }
    *ret = o;
    return 0;
}

static int cmd_get_vcpus(const json *args, json **ret, char *err, size_t errlen)
{
    int mib[2], ncpu = 1;
    size_t len = sizeof ncpu, k;
    json *a = json_arr();

    (void)args; (void)err; (void)errlen;
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) < 0 || ncpu < 1)
        ncpu = 1;
    for (k = 0; k < (size_t)ncpu; k++) {
        json *c = json_obj();

        json_obj_set(c, "logical-id", json_int((qga_i64)k));
        json_obj_set(c, "online", json_bool(1));
        json_obj_set(c, "can-offline", json_bool(0));
        json_arr_add(a, c);
    }
    *ret = a;
    return 0;
}

static int cmd_get_fsinfo(const json *args, json **ret, char *err, size_t errlen)
{
    struct statfs *fs;
    int n, k;
    json *a = json_arr();

    (void)args;
    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n < 0) {
        json_free(a);
        return qga_seterr(err, errlen, "getfsstat failed: %s", strerror(errno));
    }
    fs = (struct statfs *)qga_xmalloc((size_t)(n + 1) * sizeof *fs);
    n = getfsstat(fs, (long)((n + 1) * sizeof *fs), MNT_NOWAIT);
    for (k = 0; k < n; k++) {
        const char *dev = strrchr(fs[k].f_mntfromname, '/');
        json *e = json_obj();
        qga_i64 bs = (qga_i64)fs[k].f_bsize;

        json_obj_set(e, "name", json_str(dev ? dev + 1 : fs[k].f_mntfromname));
        json_obj_set(e, "mountpoint", json_str(fs[k].f_mntonname));
        json_obj_set(e, "type", json_str(fs[k].f_fstypename));
        json_obj_set(e, "used-bytes", json_int(((qga_i64)fs[k].f_blocks - (qga_i64)fs[k].f_bfree) * bs));
        json_obj_set(e, "total-bytes", json_int((qga_i64)fs[k].f_blocks * bs));
        json_obj_set(e, "disk", json_arr());
        json_arr_add(a, e);
    }
    free(fs);
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

static int prefix_len(unsigned long mask_be)
{
    unsigned long m = ntohl((unsigned long)mask_be) & 0xFFFFFFFFUL;
    int n = 0;

    while (m & 0x80000000UL) {
        n++;
        m = (m << 1) & 0xFFFFFFFFUL;
    }
    return n;
}

/* IPv4 and link-layer addresses via SIOCGIFCONF (getifaddrs may be missing on old releases);
 * IPv6 addresses are not reported yet. */
static int cmd_network_get_interfaces(const json *args, json **ret, char *err, size_t errlen)
{
    enum { BUFSZ = 16384 };
    struct ifconf ifc;
    char *buf = (char *)qga_xmalloc(BUFSZ);
    char *p;
    int s;
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

    for (p = buf; p < buf + ifc.ifc_len; ) {
        struct ifreq *ifr = (struct ifreq *)p;
        size_t len = sizeof ifr->ifr_name + ifr->ifr_addr.sa_len;
        char name[IFNAMSIZ + 1];
        json *ent;

        if (len < sizeof *ifr)
            len = sizeof *ifr;
        p += len;
        memcpy(name, ifr->ifr_name, IFNAMSIZ);
        name[IFNAMSIZ] = '\0';

        ent = find_iface(list, name);
        if (!ent) {
            ent = json_obj();
            json_obj_set(ent, "name", json_str(name));
            json_arr_add(list, ent);
        }
        if (ifr->ifr_addr.sa_family == AF_LINK) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr->ifr_addr;

            if (sdl->sdl_alen == 6) {
                unsigned char *m = (unsigned char *)LLADDR(sdl);
                char mac[20];

                sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
                json_obj_set(ent, "hardware-address", json_str(mac));
            }
        } else if (ifr->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
            struct ifreq req;
            json *addrs = json_obj_get(ent, "ip-addresses");
            json *a = json_obj();

            if (!addrs) {
                addrs = json_arr();
                json_obj_set(ent, "ip-addresses", addrs);
            }
            json_obj_set(a, "ip-address-type", json_str("ipv4"));
            json_obj_set(a, "ip-address", json_str(inet_ntoa(sin->sin_addr)));
            memset(&req, 0, sizeof req);
            memcpy(req.ifr_name, ifr->ifr_name, IFNAMSIZ);
            memcpy(&req.ifr_addr, &ifr->ifr_addr, sizeof(struct sockaddr_in));
            if (ioctl(s, SIOCGIFNETMASK, &req) == 0)
                json_obj_set(a, "prefix",
                             json_int(prefix_len(((struct sockaddr_in *)&req.ifr_addr)->sin_addr.s_addr)));
            json_arr_add(addrs, a);
        }
    }
    close(s);
    free(buf);
    *ret = list;
    return 0;
}

static int cmd_get_users(const json *args, json **ret, char *err, size_t errlen)
{
    struct utmp ut;
    json *a = json_arr();
    FILE *fp = fopen("/var/run/utmp", "r");

    (void)args; (void)err; (void)errlen;
    if (fp) {
        while (fread(&ut, sizeof ut, 1, fp) == 1) {
            char user[sizeof ut.ut_name + 1];
            json *e;

            if (ut.ut_name[0] == '\0' || ut.ut_line[0] == '\0')
                continue;
            memcpy(user, ut.ut_name, sizeof ut.ut_name);
            user[sizeof ut.ut_name] = '\0';
            e = json_obj();
            json_obj_set(e, "user", json_str(user));
            json_obj_set(e, "login-time", json_real((double)ut.ut_time));
            json_arr_add(a, e);
        }
        fclose(fp);
    }
    *ret = a;
    return 0;
}

static int cmd_shutdown(const json *args, json **ret, char *err, size_t errlen)
{
    const char *mode = "powerdown";
    const char *flag;
    pid_t pid;

    (void)ret;
    if (qga_opt_str(args, "mode", &mode, err, errlen))
        return 1;
    if (strcmp(mode, "powerdown") == 0 || strcmp(mode, "halt") == 0)
        flag = "-h";
    else if (strcmp(mode, "reboot") == 0)
        flag = "-r";
    else
        return qga_seterr(err, errlen, "mode is invalid (valid values are: halt|powerdown|reboot)");
    pid = fork();
    if (pid < 0)
        return qga_seterr(err, errlen, "fork failed: %s", strerror(errno));
    if (pid == 0) {
        execl("/sbin/shutdown", "shutdown", flag, "now", (char *)NULL);
        _exit(127);
    }
    return 0;           /* QGA_NOREPLY: the guest goes down without answering */
}

void qga_system_register(void)
{
    qga_register("guest-get-host-name", cmd_host_name);
    qga_register("guest-get-hostname", cmd_host_name);
    qga_register("guest-get-time", cmd_get_time);
    qga_register("guest-set-time", cmd_set_time);
    qga_register("guest-get-timezone", cmd_get_timezone);
    qga_register("guest-get-load", cmd_get_load);
    qga_register("guest-get-osinfo", cmd_get_osinfo);
    qga_register("guest-get-vcpus", cmd_get_vcpus);
    qga_register("guest-get-fsinfo", cmd_get_fsinfo);
    qga_register("guest-network-get-interfaces", cmd_network_get_interfaces);
    qga_register("guest-get-users", cmd_get_users);
    qga_register_ex("guest-shutdown", cmd_shutdown, QGA_NOREPLY);
}
