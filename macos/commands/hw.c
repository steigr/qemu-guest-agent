/* hw.c - memory blocks, CPU statistics, disks and disk statistics.
 *
 * The Linux-shaped replies are filled with what Mac OS X can provide; fields it cannot
 * provide are omitted (the schema marks them optional) rather than invented. */
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#define MEMORY_BLOCK_SIZE (128LL * 1024 * 1024)     /* nominal: memory never goes away */

static qga_i64 physmem_bytes(void)
{
    int mib[2];
    unsigned long mem = 0;
    unsigned int mem32 = 0;
    size_t len = sizeof mem32;

    mib[0] = CTL_HW;
    mib[1] = HW_PHYSMEM;
    if (sysctl(mib, 2, &mem32, &len, NULL, 0) == 0 && len == sizeof mem32)
        mem = mem32;
    return (qga_i64)mem;
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
    natural_t ncpu = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t count = 0;
    processor_cpu_load_info_data_t *load;
    json *a = json_arr();
    natural_t k;

    (void)args;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &count) != KERN_SUCCESS) {
        json_free(a);
        return qga_seterr(err, errlen, "host_processor_info failed");
    }
    load = (processor_cpu_load_info_data_t *)info;
    for (k = 0; k < ncpu; k++) {
        json *c = json_obj();

        json_obj_set(c, "type", json_str("linux"));
        json_obj_set(c, "cpu", json_int(k));
        json_obj_set(c, "user", json_int(load[k].cpu_ticks[CPU_STATE_USER]));
        json_obj_set(c, "nice", json_int(load[k].cpu_ticks[CPU_STATE_NICE]));
        json_obj_set(c, "system", json_int(load[k].cpu_ticks[CPU_STATE_SYSTEM]));
        json_obj_set(c, "idle", json_int(load[k].cpu_ticks[CPU_STATE_IDLE]));
        json_arr_add(a, c);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, count * sizeof(int));
    *ret = a;
    return 0;
}

/* diskN is a whole disk, diskNsM a partition. */
static int disk_node(const char *name, int *partition)
{
    const char *p = name;

    if (strncmp(p, "disk", 4) != 0)
        return 0;
    p += 4;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p == '\0') {
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
    DIR *d = opendir("/dev");
    struct dirent *e;
    json *a = json_arr();

    (void)args;
    if (!d) {
        json_free(a);
        return qga_seterr(err, errlen, "cannot read /dev: %s", strerror(errno));
    }
    while ((e = readdir(d)) != NULL) {
        int part;

        if (disk_node(e->d_name, &part)) {
            char path[300];
            json *o = json_obj();

            sprintf(path, "/dev/%.250s", e->d_name);
            json_obj_set(o, "name", json_str(path));
            json_obj_set(o, "partition", json_bool(part));
            json_obj_set(o, "slaves", json_arr());
            json_arr_add(a, o);
        }
    }
    closedir(d);
    *ret = a;
    return 0;
}

static qga_i64 cf_number(CFDictionaryRef d, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    long long n = 0;

    CFRelease(k);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &n);
    return (qga_i64)n;
}

static CFTypeRef cf_get(CFDictionaryRef d, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
    CFTypeRef v = CFDictionaryGetValue(d, k);

    CFRelease(k);
    return v;
}

/* BSD name of the whole-disk IOMedia below a block storage driver, or "" if none. */
static void media_name(io_registry_entry_t driver, char *out, size_t outlen)
{
    io_iterator_t it;
    io_registry_entry_t child;

    out[0] = '\0';
    if (IORegistryEntryGetChildIterator(driver, kIOServicePlane, &it) != KERN_SUCCESS)
        return;
    while (out[0] == '\0' && (child = IOIteratorNext(it)) != 0) {
        CFMutableDictionaryRef props = NULL;

        if (IORegistryEntryCreateCFProperties(child, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
            CFTypeRef name = cf_get(props, "BSD Name");

            if (name && CFGetTypeID(name) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)name, out, (CFIndex)outlen, kCFStringEncodingUTF8);
            CFRelease(props);
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(it);
}

static int cmd_diskstats(const json *args, json **ret, char *err, size_t errlen)
{
    mach_port_t master;
    io_iterator_t it;
    io_registry_entry_t svc;
    json *a = json_arr();

    (void)args;
    if (IOMasterPort(MACH_PORT_NULL, &master) != KERN_SUCCESS ||
        IOServiceGetMatchingServices(master, IOServiceMatching("IOBlockStorageDriver"), &it) != KERN_SUCCESS) {
        json_free(a);
        return qga_seterr(err, errlen, "IOKit block storage lookup failed");
    }
    while ((svc = IOIteratorNext(it)) != 0) {
        CFMutableDictionaryRef props = NULL;
        char bsd[64];

        media_name(svc, bsd, sizeof bsd);
        if (bsd[0] &&
            IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
            CFTypeRef st = cf_get(props, "Statistics");

            if (st && CFGetTypeID(st) == CFDictionaryGetTypeID()) {
                CFDictionaryRef s = (CFDictionaryRef)st;
                json *o = json_obj(), *m = json_obj();
                char path[100];

                /* Sectors are 512 bytes; times are nanoseconds in IOKit, milliseconds here. */
                json_obj_set(m, "read-sectors", json_int(cf_number(s, "Bytes (Read)") / 512));
                json_obj_set(m, "read-ios", json_int(cf_number(s, "Operations (Read)")));
                json_obj_set(m, "write-sectors", json_int(cf_number(s, "Bytes (Write)") / 512));
                json_obj_set(m, "write-ios", json_int(cf_number(s, "Operations (Write)")));
                json_obj_set(m, "read-ticks", json_int(cf_number(s, "Total Time (Read)") / 1000000));
                json_obj_set(m, "write-ticks", json_int(cf_number(s, "Total Time (Write)") / 1000000));
                sprintf(path, "/dev/%.90s", bsd);
                json_obj_set(o, "name", json_str(path));
                json_obj_set(o, "stats", m);
                json_arr_add(a, o);
            }
            CFRelease(props);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
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
