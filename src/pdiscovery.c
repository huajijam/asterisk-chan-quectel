/*
   Copyright (C) 2011 bg <bg_one@mail.ru>
*/
#include <dirent.h>    /* DIR */
#include <stdio.h>     /* NULL */
#include <string.h>    /* strlen() */
#include <sys/stat.h>  /* stat() */
#include <sys/types.h> /* u_int16_t u_int8_t */

#include "ast_config.h"

#include "pdiscovery.h" /* pdiscovery_lookup()  */

#include "at_queue.h"     /* write_all() */
#include "at_read.h"      /* at_wait() at_read() at_read_result_iov() at_read_result_classification() */
#include "chan_quectel.h" /* opentty() closetty() */
#include "mutils.h"       /* ITEMS_OF() */
#include "ringbuffer.h"   /* struct ringbuffer */

/*
static const char sys_bus_usb_drivers_usb[] = "/sys/bus/usb/drivers/usb";
*/
static const char sys_bus_usb_devices[] = "/sys/bus/usb/devices";


/* timeout for port readering milliseconds */
#define PDISCOVERY_TIMEOUT 500

struct pdiscovery_device {
    u_int16_t vendor_id;
    u_int16_t product_id;
    u_int8_t interfaces[INTERFACE_TYPE_NUMBERS];
};

struct pdiscovery_request {
    const char* name;
    const char* imei;
    const char* imsi;
};

struct pdiscovery_cache_item {
    AST_LIST_ENTRY(pdiscovery_cache_item) entry;
    struct timeval validtill;
    int status;
    struct pdiscovery_result res;
};

struct discovery_cache {
    AST_RWLIST_HEAD(, pdiscovery_cache_item) items;
};

#define BUILD_NAME(d1, d2, d1len, d2len, out) \
    d2len = strlen(d2);                       \
    out   = alloca(d1len + 1 + d2len + 1);    \
    memcpy(out, d1, d1len);                   \
    out[d1len] = '/';                         \
    memcpy(out + d1len + 1, d2, d2len);       \
    d2len      += d1len + 1;                  \
    out[d2len]  = 0;


static const struct pdiscovery_device device_ids[] = {
    {0x12d1, 0x1001, {2, 1, /* 0 */}}, /* E1550 and generic */
  //	{ 0x12d1, 0x1465, { 2, 1, /* 0 */ } },		/* K3520 */
    {0x12d1, 0x140c, {3, 2, /* 0 */}}, /* E17xx */
    {0x12d1, 0x14ac, {4, 3, /* 0 */}}, /* E153Du-1 : thanks mghadam */
    {0x12d1, 0x1436, {4, 3, /* 0 */}}, /* E1750 */
    {0x12d1, 0x1506, {3, 2, /* 0 */}}, /* E171 firmware 21.x : thanks Sergey Ivanov */
    {0x2c7c, 0x0125, {2, 1, /* 0 */}}, /* Quectel EC25 */
    {0x1e0e, 0x9001, {2, 4, /* 0 */}}, /* Simcom Sim7600 */
};

static struct discovery_cache cache;

#/* return non-0 if all ports matched */

static int ports_match(const struct pdiscovery_ports* p1, const struct pdiscovery_ports* p2)
{
    for (unsigned i = 0; i < ITEMS_OF(p1->ports); ++i) {
        if (!p1->ports[i] || !p2->ports[i] || strcmp(p1->ports[i], p2->ports[i])) {
            return 0;
        }
    }

    return 1;
}

#/* */

static int ports_copy(struct pdiscovery_ports* dst, const struct pdiscovery_ports* src)
{
    for (unsigned i = 0; i < ITEMS_OF(dst->ports); ++i) {
        if (!src->ports[i]) {
            continue;
        }

        dst->ports[i] = ast_strdup(src->ports[i]);
        if (!dst->ports[i]) {
            return 0;
        }
    }

    return 1;
}

#/* */

static void ports_free(struct pdiscovery_ports* ports)
{
    for (unsigned i = 0; i < ITEMS_OF(ports->ports); ++i) {
        if (!ports->ports[i]) {
            continue;
        }
        ast_free(ports->ports[i]);
        ports->ports[i] = NULL;
    }
}

#/* */

static void info_free(struct pdiscovery_result* res)
{
    if (res->imsi) {
        ast_free(res->imsi);
        res->imsi = NULL;
    }

    if (res->imei) {
        ast_free(res->imei);
        res->imei = NULL;
    }
}

#/* */

static void info_copy(struct pdiscovery_result* dst, const struct pdiscovery_result* src)
{
    if (src->imei) {
        dst->imei = ast_strdup(src->imei);
    }
    if (src->imsi) {
        dst->imsi = ast_strdup(src->imsi);
    }
}

#/* */

static void result_free(struct pdiscovery_result* res)
{
    ports_free(&res->ports);
    info_free(res);
}

#/* */

static void cache_item_free(struct pdiscovery_cache_item* item)
{
    if (item) {
        result_free(&item->res);
        ast_free(item);
    }
}

#/* */

static void cache_item_update(struct pdiscovery_cache_item* item, const struct pdiscovery_result* res, int status)
{
    info_free(&item->res);
    info_copy(&item->res, res);

    item->status = status;

    item->validtill         = ast_tvnow();
    item->validtill.tv_sec += CONF_GLOBAL(discovery_interval);
}

#/* */

static struct pdiscovery_cache_item* cache_item_create(const struct pdiscovery_result* res, int status)
{
    struct pdiscovery_cache_item* item = ast_calloc(1, sizeof(*item));
    if (item) {
        if (ports_copy(&item->res.ports, &res->ports)) {
            cache_item_update(item, res, status);
        } else {
            cache_item_free(item);
            item = NULL;
        }
    }

    return item;
}

#/* */

static struct pdiscovery_cache_item* cache_search(struct discovery_cache* cache, const struct pdiscovery_result* res)
{
    struct pdiscovery_cache_item* found = NULL;
    struct pdiscovery_cache_item* item;
    struct timeval now = ast_tvnow();

    AST_RWLIST_WRLOCK(&cache->items);
    AST_LIST_TRAVERSE_SAFE_BEGIN(&cache->items, item, entry)
        if (ast_tvcmp(now, item->validtill) < 0) {
            if (ports_match(&item->res.ports, &res->ports)) {
                found = item;
                break;
            }
        } else {
            // remove expired item
            AST_LIST_REMOVE_CURRENT(entry);
            cache_item_free(item);
        }
    AST_LIST_TRAVERSE_SAFE_END;
    AST_RWLIST_UNLOCK(&cache->items);

    return found;
}

#/* */

static int cache_lookup(struct discovery_cache* cache, const struct pdiscovery_request* req, struct pdiscovery_result* res, int* failed)
{
    int found                          = 0;
    struct pdiscovery_cache_item* item = cache_search(cache, res);
    if (item) {
        res->imei = item->res.imei ? ast_strdup(item->res.imei) : NULL;
        res->imsi = item->res.imsi ? ast_strdup(item->res.imsi) : NULL;
        found     = item->status || ((req->imei || item->res.imei) && (req->imsi || item->res.imsi));
        if (found) {
            *failed = item->status;
        }
    }
    return found;
}

#/* */

static void cache_update(struct discovery_cache* cache, const struct pdiscovery_result* res, int status)
{
    struct pdiscovery_cache_item* item = cache_search(cache, res);
    if (item) {
        cache_item_update(item, res, status);
    } else {
        item = cache_item_create(res, status);
        AST_LIST_INSERT_TAIL(&cache->items, item, entry);
    }
}

#/* */

static void cache_init(struct discovery_cache* cache)
{
    /* TODO: place lock init when locking becomes required */

    AST_RWLIST_HEAD_INIT(&cache->items);
}

#/* */

static void cache_fini(struct discovery_cache* cache)
{
    struct pdiscovery_cache_item* item;

    AST_RWLIST_WRLOCK(&cache->items);
    AST_LIST_TRAVERSE_SAFE_BEGIN(&cache->items, item, entry)
        AST_LIST_REMOVE_CURRENT(entry);
        cache_item_free(item);
    AST_LIST_TRAVERSE_SAFE_END;
    AST_RWLIST_UNLOCK(&cache->items);

    AST_RWLIST_HEAD_DESTROY(&cache->items);
}

#/* */

static const struct pdiscovery_cache_item* cache_first_readlock(struct discovery_cache* cache)
{
    AST_RWLIST_RDLOCK(&cache->items);
    return AST_RWLIST_FIRST(&cache->items);
}

#/* */

static void cache_unlock(struct discovery_cache* cache) { AST_RWLIST_UNLOCK(&cache->items); }

#/* */

static int pdiscovery_get_id(const char* name, int len, const char* filename, unsigned* integer)
{
    int len2;
    char* name2;
    int assign = 0;

    BUILD_NAME(name, filename, len, len2, name2);
    FILE* file = fopen(name2, "r");
    if (file) {
        assign = fscanf(file, "%x", integer);
        fclose(file);
    }

    return assign;
}

#/* */

static int pdiscovery_is_port(const char* name, int len)
{
    int len2;
    char* name2;
    struct stat statb;

    BUILD_NAME(name, "port_number", len, len2, name2);
    return !stat(name2, &statb) && S_ISREG(statb.st_mode);
}

#/* */

static char* pdiscovery_port(const char* name, int len, const char* subdir)
{
    int len2, len3;
    char *name2, *name3;
    struct stat statb;
    char* port = NULL;

    BUILD_NAME(name, subdir, len, len2, name2);

    if (!stat(name2, &statb) && S_ISDIR(statb.st_mode) && pdiscovery_is_port(name2, len2)) {
        //		ast_debug(4, "[%s discovery] found port %s\n", devname, dentry->d_name);
        BUILD_NAME("/dev", subdir, 4, len3, name3);
        port = ast_strdup(name3);
    }
    return port;
}

#/* */

static char* pdiscovery_port_name(const char* name, int len)
{
    char* port = NULL;
    struct dirent* dentry;
    DIR* dir = opendir(name);
    if (dir) {
        while ((dentry = readdir(dir)) != NULL) {
            if (strcmp(dentry->d_name, ".") && strcmp(dentry->d_name, "..")) {
                port = pdiscovery_port(name, len, dentry->d_name);
                if (port) {
                    break;
                }
            }
        }
        closedir(dir);
    }
    return port;
}

#/* */

static char* pdiscovery_interface(const char* name, int len, unsigned* interface)
{
    char* port = NULL;
    if (pdiscovery_get_id(name, len, "bInterfaceNumber", interface) == 1) {
        //		ast_debug(4, "[%s discovery] bInterfaceNumber %02x\n", devname, *interface);
        port = pdiscovery_port_name(name, len);
    }
    return port;
}

#/* */

static char* pdiscovery_find_port(const char* name, int len, const char* subdir, unsigned* interface)
{
    int len2;
    char* name2;
    struct stat statb;
    char* port = NULL;

    BUILD_NAME(name, subdir, len, len2, name2);

    if (!stat(name2, &statb) && S_ISDIR(statb.st_mode)) {
        port = pdiscovery_interface(name2, len2, interface);
    }
    return port;
}

#/* */

static int pdiscovery_interfaces(const char* devname, const char* name, int len, const struct pdiscovery_device* device, struct pdiscovery_ports* ports)
{
    unsigned interface;
    unsigned idx;
    int found = 0;
    struct dirent* dentry;
    char* port;

    DIR* dir = opendir(name);
    if (dir) {
        while ((dentry = readdir(dir)) != NULL) {
            if (strchr(dentry->d_name, ':')) {
                port = pdiscovery_find_port(name, len, dentry->d_name, &interface);
                if (port) {
                    ast_debug(4, "[%s discovery] found InterfaceNumber %02x port %s\n", devname, interface, port);
                    for (idx = 0; idx < (int)ITEMS_OF(device->interfaces); idx++) {
                        if (device->interfaces[idx] == interface) {
                            if (ports->ports[idx] == NULL) {
                                ports->ports[idx] = port;
                                if (++found == INTERFACE_TYPE_NUMBERS) {
                                    break;
                                }
                            } else {
                                ast_debug(4, "[%s discovery] port %s for bInterfaceNumber %02x already exists new is %s\n", devname, ports->ports[idx],
                                          interface, port);
                                // FIXME
                            }
                        }
                    }
                }
            }
        }
        closedir(dir);
    }
    return found;
}

#/* */

static const struct pdiscovery_device* pdiscovery_lookup_ids(const char* devname, const char* name, int len)
{
    unsigned vid;
    unsigned pid;
    unsigned idx;

    if (pdiscovery_get_id(name, len, "idVendor", &vid) == 1 && pdiscovery_get_id(name, len, "idProduct", &pid) == 1) {
        ast_debug(4, "[%s discovery] found %s is idVendor %04x idProduct %04x\n", devname, name, vid, pid);
        for (idx = 0; idx < ITEMS_OF(device_ids); idx++) {
            if (device_ids[idx].vendor_id == vid && device_ids[idx].product_id == pid) {
                return &device_ids[idx];
            }
        }
    }
    return NULL;
}

#/* 0D 0A IMEI: <15 digits> 0D 0A */

static char* pdiscovery_handle_ati(const char* str)
{
    char imei[16];
    imei[15] = '\000';

    if (sscanf(str, "AT+GSN %15c OK", imei) == 1) {
        return ast_strdup(imei);
    }

    if (sscanf(str, " %15c OK", imei) == 1) {
        return ast_strdup(imei);
    }

    return NULL;
}

#/* 0D 0A 15 digits 0D 0A */

static char* pdiscovery_handle_cimi(const char* str)
{
    char imsi[16];
    imsi[15] = '\000';

    if (sscanf(str, "AT+CIMI %15c OK", imsi) == 1) {
        return ast_strdup(imsi);
    }

    if (sscanf(str, " %15c OK", imsi) == 1) {
        return ast_strdup(imsi);
    }

    return NULL;
}

#/* return non-zero on done with command */

static int pdiscovery_handle_response(const struct pdiscovery_request* req, const struct iovec* iov, int iovcnt, struct pdiscovery_result* res)
{
    int done = 0;
    char* str;
    char sym;
    size_t len = iov[0].iov_len + iov[1].iov_len;
    if (len > 0) {
        len--;
        if (iovcnt == 2) {
            str = alloca(len + 1);
            memcpy(str, iov[0].iov_base, iov[0].iov_len);
            memcpy(str + iov[0].iov_len, iov[1].iov_base, iov[1].iov_len);
        } else {
            str = iov[0].iov_base;
        }
        sym      = str[len];
        str[len] = 0;

        ast_debug(4, "[%s discovery] < %s\n", req->name, str);
        done = strstr(str, "OK") != NULL || strstr(str, "ERROR") != NULL;
        if (done && req->imei && res->imei == NULL) {
            res->imei = pdiscovery_handle_ati(str);
        }
        if (done && req->imsi && res->imsi == NULL) {
            res->imsi = pdiscovery_handle_cimi(str);
        }
        /* restore tail of string for collect data in buffer */
        str[len] = sym;
    }
    return done;
}

#/* return zero on sucess */

static int pdiscovery_do_cmd(const struct pdiscovery_request* req, int fd, const char* name, const char* cmd, unsigned length, struct pdiscovery_result* res)
{
    static const size_t RINGBUFFER_SIZE = 1024 + 1;

    struct ringbuffer rb;

    ast_debug(4, "[%s discovery] use %s for IMEI/IMSI discovery\n", req->name, name);

    void* const buf = ast_malloc(RINGBUFFER_SIZE);
    rb_init(&rb, buf, RINGBUFFER_SIZE);
    clean_read_data(req->name, fd, &rb);

    const size_t wrote = write_all(fd, cmd, length);
    if (wrote == length) {
        int timeout = PDISCOVERY_TIMEOUT;
        while (timeout > 0 && at_wait(fd, &timeout)) {
            int iovcnt = at_read(fd, name, &rb);
            if (iovcnt > 0) {
                struct iovec iov[2];
                iovcnt = rb_read_all_iov(&rb, iov);
                if (pdiscovery_handle_response(req, iov, iovcnt, res)) {
                    ast_free(buf);
                    return 0;
                }
            } else {
                ast_log(LOG_ERROR, "[%s discovery] read from %s failed: %s\n", req->name, name, strerror(errno));
                ast_free(buf);
                return -1;
            }
        }
        ast_log(LOG_ERROR, "[%s discovery] failed to get valid response from %s in %d msec\n", req->name, name, PDISCOVERY_TIMEOUT);
    } else {
        ast_log(LOG_ERROR, "[%s discovery] write to %s failed: %s\n", req->name, name, strerror(errno));
    }

    ast_free(buf);
    return 1;
}

#/* return non-zero on fail */

static int pdiscovery_get_info(const char* port, const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    static const struct {
        const char* cmd;
        unsigned length;
    } cmds[] = {
        {"AT+CIMI\r",       8 }, /* IMSI */
        {"AT+GSN\r",        7 }, /* IMEI */
        {"AT+GSN; +CIMI\r", 14}, /* IMSI + IMEI */
    };

    static const int want_map[2][2] = {
        {2, 0}, // want_imei = 0
        {1, 2}  // want_imei = 1
    };

    int fail = 1;
#ifdef USE_SYSV_UUCP_LOCKS
    char* lock_file;

    const int fd = opentty(port, &lock_file, 0);
#else
    const int fd = opentty(port, 0);
#endif
    if (fd >= 0) {
        unsigned want_imei = req->imei && res->imei == NULL;  // 1 && 0
        unsigned want_imsi = req->imsi && res->imsi == NULL;  // 1 && 1
        unsigned cmd       = want_map[want_imei][want_imsi];

        /* clean queue first ? */
        fail = pdiscovery_do_cmd(req, fd, port, cmds[cmd].cmd, cmds[cmd].length, res);
#ifdef USE_SYSV_UUCP_LOCKS
        closetty(port, fd, &lock_file);
#else
        closetty(port, fd);
#endif
    }

    return fail;
}

#/* return non-zero on fail */

static int pdiscovery_get_info_cached(const char* port, const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    int fail = 1;
    /* may add info also if !found */
    int found = cache_lookup(&cache, req, res, &fail);
    if (!found) {
        fail = pdiscovery_get_info(port, req, res);
        cache_update(&cache, res, fail);
    } else {
        ast_debug(4, "[%s discovery] %s use cached IMEI %s IMSI %s failed %d\n", req->name, port, S_OR(res->imei, ""), S_OR(res->imsi, ""), fail);
    }

    return fail;
}

#/* return zero on success */

static int pdiscovery_read_info(const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    int fail          = 1;
    const char* dport = res->ports.ports[INTERFACE_TYPE_DATA];

#ifdef USE_SYSV_UUCP_LOCKS
    char* dlock;
    int pid = lock_try(dport, &dlock);
    if (!pid) {
        fail = pdiscovery_get_info_cached(dport, req, res);
        closetty(dport, -1, &dlock);
    } else {
        ast_debug(4, "[%s discovery] %s already used by process %d, skipped\n", req->name, dport, pid);
    }
#else
    fail = pdiscovery_get_info_cached(dport, req, res);
#endif
    return fail;
}

#/* */

static int pdiscovery_check_req(const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    int match = 0;
    if (!pdiscovery_read_info(req, res)) {
        match = ((req->imei == 0) || (res->imei && !strcmp(req->imei, res->imei))) && ((req->imsi == 0) || (res->imsi && !strcmp(req->imsi, res->imsi)));

        ast_debug(4, "[%s discovery] %smatched IMEI=%s/%s IMSI=%s/%s\n", req->name, match ? "" : "un", S_OR(req->imei, ""), S_OR(res->imei, ""),
                  S_OR(req->imsi, ""), S_OR(res->imsi, ""));
    }

    return match;
}

#/* */

static int pdiscovery_check_device(const char* name, int len, const char* subdir, const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    int len2;
    char* name2;
    const struct pdiscovery_device* device;
    int found = 0;

    BUILD_NAME(name, subdir, len, len2, name2);

    device = pdiscovery_lookup_ids(req->name, name2, len2);
    if (device) {
        //		ast_debug(4, "[%s discovery] should ports <-> interfaces map for %04x:%04x modem=%02x voice=%02x
        // data=%02x\n",
        ast_debug(4, "[%s discovery] should ports <-> interfaces map for %04x:%04x voice=%02x data=%02x\n", req->name, device->vendor_id, device->product_id,
                  //			device->interfaces[INTERFACE_TYPE_COM],
                  device->interfaces[INTERFACE_TYPE_VOICE], device->interfaces[INTERFACE_TYPE_DATA]);
        pdiscovery_interfaces(req->name, name2, len2, device, &res->ports);

        /* check mandatory ports */
        if (res->ports.ports[INTERFACE_TYPE_DATA] && res->ports.ports[INTERFACE_TYPE_VOICE]) {
            found = pdiscovery_check_req(req, res);
        }
    }

    if (!found) {
        result_free(res);
    }
    return found;
}

#/* */

static int pdiscovery_request_do(const char* name, int len, const struct pdiscovery_request* req, struct pdiscovery_result* res)
{
    int found = 0;
    struct dirent* dentry;
    DIR* dir = opendir(name);
    if (dir) {
        while ((dentry = readdir(dir)) != NULL) {
            if (strcmp(dentry->d_name, ".") && strcmp(dentry->d_name, "..") && strstr(dentry->d_name, "usb") != dentry->d_name) {
                ast_debug(4, "[%s discovery] checking %s/%s\n", req->name, name, dentry->d_name);
                found = pdiscovery_check_device(name, len, dentry->d_name, req, res);
                if (found) {
                    break;
                }
            }
        }
        closedir(dir);
    }
    return found;
}

#/* */

void pdiscovery_init() { cache_init(&cache); }

#/* */

void pdiscovery_fini() { cache_fini(&cache); }

#/* */

int pdiscovery_lookup(const char* devname, const char* imei, const char* imsi, char** dport, char** aport)
{
    int found;
    struct pdiscovery_result res;
    const struct pdiscovery_request req = {
        devname,
        ((imei && imei[0]) ? imei : NULL),
        ((imsi && imsi[0]) ? imsi : NULL),
    };

    memset(&res, 0, sizeof(res));
    found = pdiscovery_request_do(sys_bus_usb_devices, STRLEN(sys_bus_usb_devices), &req, &res);
    if (found) {
        *dport = ast_strdup(res.ports.ports[INTERFACE_TYPE_DATA]);
        *aport = ast_strdup(res.ports.ports[INTERFACE_TYPE_VOICE]);
    }
    result_free(&res);
    return found;
}

#/* */

const struct pdiscovery_result* pdiscovery_list_begin(const struct pdiscovery_cache_item** opaque)
{
    const struct pdiscovery_cache_item* item;
    struct pdiscovery_result res;
    const struct pdiscovery_request req = {
        "list",
        "ANY",
        "ANY",
    };

    memset(&res, 0, sizeof(res));
    pdiscovery_request_do(sys_bus_usb_devices, STRLEN(sys_bus_usb_devices), &req, &res);
    result_free(&res);

    *opaque = item = cache_first_readlock(&cache);
    return item != NULL ? &item->res : NULL;
}

#/* */

const struct pdiscovery_result* pdiscovery_list_next(const struct pdiscovery_cache_item** opaque)
{
    const struct pdiscovery_cache_item* item = AST_RWLIST_NEXT(*opaque, entry);
    *opaque                                  = item;
    return item != NULL ? &item->res : NULL;
}

#/* */

void pdiscovery_list_end() { cache_unlock(&cache); }
