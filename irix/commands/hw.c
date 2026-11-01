/* hw.c - memory blocks, CPU statistics, disks and disk statistics.
 *
 * The Linux-shaped replies are filled with what IRIX can provide; fields it cannot provide are
 * omitted (the schema marks them optional) rather than invented. */
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/attributes.h>
#include <sys/elog.h>
#include <sys/iograph.h>
#include <sys/param.h>
#include <sys/sysinfo.h>
#include <sys/sysmp.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MEMORY_BLOCK_SIZE (128LL * 1024 * 1024)     /* nominal: memory never goes away */
#define RDISK_DIR "/hw/rdisk"
#define MS_PER_TICK (1000 / HZ)

static qga_i64 physmem_bytes(void)
{
    struct rminfo rm;

    if (sysmp(MP_SAGET, MPSA_RMINFO, &rm, sizeof rm) < 0)
        return 0;
    return (qga_i64)rm.physmem * getpagesize();
}

static int cmd_memory_block_info(const json *args, json **ret, char *err, size_t errlen)
{
    json *o = json_obj();

    (void)args; (void)err; (void)errlen;
    json_obj_set(o, "size", json_int(MEMORY_BLOCK_SIZE));
    *ret = o;
    return 0;
}

static int cmd_memory_blocks(const json *args, json **ret, char *err, size_t errlen)
{
    qga_i64 mem = physmem_bytes(), n, k;
    json *a = json_arr();

    (void)args; (void)err; (void)errlen;
    n = (mem + MEMORY_BLOCK_SIZE - 1) / MEMORY_BLOCK_SIZE;
    if (n < 1)
        n = 1;
    for (k = 0; k < n; k++) {
        json *b = json_obj();

        json_obj_set(b, "phys-index", json_int(k));
        json_obj_set(b, "online", json_bool(1));
        json_obj_set(b, "can-offline", json_bool(0));
        json_arr_add(a, b);
    }
    *ret = a;
    return 0;
}

static int cmd_cpustats(const json *args, json **ret, char *err, size_t errlen)
{
    int ncpu = (int)sysmp(MP_NPROCS), k;
    json *a = json_arr();

    (void)args;
    for (k = 0; k < ncpu; k++) {
        struct sysinfo si;
        json *c;

        if (sysmp(MP_SAGET1, MPSA_SINFO, &si, sizeof si, k) < 0) {
            json_free(a);
            return qga_seterr(err, errlen, "sysmp MPSA_SINFO failed: %s", strerror(errno));
        }
        c = json_obj();
        json_obj_set(c, "type", json_str("linux"));
        json_obj_set(c, "cpu", json_int(k));
        json_obj_set(c, "user", json_int((qga_i64)si.cpu[CPU_USER]));
        json_obj_set(c, "system", json_int((qga_i64)si.cpu[CPU_KERNEL]));
        json_obj_set(c, "idle", json_int((qga_i64)si.cpu[CPU_IDLE]));
        json_obj_set(c, "iowait", json_int((qga_i64)si.cpu[CPU_WAIT]));
        json_obj_set(c, "irq", json_int((qga_i64)si.cpu[CPU_INTR]));
        json_arr_add(a, c);
    }
    *ret = a;
    return 0;
}

/* The hwgraph's raw disks: dksXdYvol is a whole disk and dksXdYsZ a partition. root, swap and the
 * volume header dksXdYvh are other names for what is there already. Returns 1 for a name that
 * counts, with its parent's length before the "vol" or "sZ" in *stem. */
static int disk_node(const char *name, int *partition, size_t *stem)
{
    const char *p = name;

    if (strncmp(p, "dks", 3) != 0)
        return 0;
    p = name + 3;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p++ != 'd')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    /* Fibre channel and RAID names have an l<lun> here. */
    if (*p == 'l') {
        p++;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    *stem = (size_t)(p - name);
    if (strcmp(p, "vol") == 0) {
        *partition = 0;
        return 1;
    }
    if (*p++ != 's' || *p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    *partition = 1;
    return *p == '\0';
}

static int cmd_disks(const json *args, json **ret, char *err, size_t errlen)
{
    DIR *d = opendir(RDISK_DIR);
    struct dirent *e;
    json *a = json_arr();

    (void)args;
    if (!d) {
        json_free(a);
        return qga_seterr(err, errlen, "cannot read %s: %s", RDISK_DIR, strerror(errno));
    }
    while ((e = readdir(d)) != NULL) {
        int part;
        size_t stem;

        if (disk_node(e->d_name, &part, &stem)) {
            char path[300];
            json *o = json_obj(), *deps = json_arr();

            sprintf(path, "%s/%.250s", part ? "/dev/dsk" : "/dev/rdsk", e->d_name);
            json_obj_set(o, "name", json_str(path));
            json_obj_set(o, "partition", json_bool(part));
            if (part) {
                sprintf(path, "/dev/rdsk/%.*svol", (int)(stem < 250 ? stem : 250), e->d_name);
                json_arr_add(deps, json_str(path));
            }
            json_obj_set(o, "dependencies", deps);
            json_arr_add(a, o);
        }
    }
    closedir(d);
    *ret = a;
    return 0;
}

/* The dksc driver keeps a struct iotime on each whole disk's hwgraph vertex, which is what sar and
 * pcp read. It counts in 512-byte blocks and ticks, with reads as the total less the writes. */
static int cmd_diskstats(const json *args, json **ret, char *err, size_t errlen)
{
    DIR *d = opendir(RDISK_DIR);
    struct dirent *e;
    json *a = json_arr();

    (void)args;
    if (!d) {
        json_free(a);
        return qga_seterr(err, errlen, "cannot read %s: %s", RDISK_DIR, strerror(errno));
    }
    while ((e = readdir(d)) != NULL) {
        int part;
        size_t stem;
        char path[300];
        struct iotime io;
        int len = sizeof io;
        json *o, *m;

        if (!disk_node(e->d_name, &part, &stem) || part)
            continue;
        sprintf(path, "%s/%.250s", RDISK_DIR, e->d_name);
        if (attr_get(path, INFO_LBL_DKIOTIME, (char *)&io, &len, 0) != 0)
            continue;
        o = json_obj();
        m = json_obj();
        json_obj_set(m, "read-sectors", json_int((qga_i64)io.io_bcnt - (qga_i64)io.io_wbcnt));
        json_obj_set(m, "read-ios", json_int((qga_i64)io.io_cnt - (qga_i64)io.io_wops));
        json_obj_set(m, "write-sectors", json_int((qga_i64)io.io_wbcnt));
        json_obj_set(m, "write-ios", json_int((qga_i64)io.io_wops));
        json_obj_set(m, "weight-ticks", json_int((qga_i64)io.io_resp * MS_PER_TICK));
        json_obj_set(m, "ios-pgr", json_int((qga_i64)io.io_qc));
        json_obj_set(m, "total-ticks", json_int((qga_i64)io.io_act * MS_PER_TICK));
        sprintf(path, "/dev/rdsk/%.250s", e->d_name);
        json_obj_set(o, "name", json_str(path));
        json_obj_set(o, "stats", m);
        json_arr_add(a, o);
    }
    closedir(d);
    *ret = a;
    return 0;
}

void qga_hw_register(void)
{
    qga_register("guest-get-memory-block-info", cmd_memory_block_info);
    qga_register("guest-get-memory-blocks", cmd_memory_blocks);
    qga_register("guest-get-cpustats", cmd_cpustats);
    qga_register("guest-get-disks", cmd_disks);
    qga_register("guest-get-diskstats", cmd_diskstats);
}
