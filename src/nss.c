#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <netdb.h>
#include <sys/socket.h>
#include <nss.h>

#include "query.h"

#define MAX_ENTRIES 16

#ifdef NSS_IPV4_ONLY
#define _nss_mdns_gethostbyname2_r _nss_mdns4_gethostbyname2_r
#define _nss_mdns_gethostbyname_r _nss_mdns4_gethostbyname_r
#define _nss_mdns_gethostbyaddr_r _nss_mdns4_gethostbyaddr_r
#elif NSS_IPV6_ONLY
#define _nss_mdns_gethostbyname2_r _nss_mdns6_gethostbyname2_r
#define _nss_mdns_gethostbyname_r _nss_mdns6_gethostbyname_r
#define _nss_mdns_gethostbyaddr_r _nss_mdns6_gethostbyaddr_r
#endif

struct userdata {
    int count;
    int data_len; /* only valid when doing reverse lookup */
    union  {
        ipv4_address_t ipv4[MAX_ENTRIES];
        ipv6_address_t ipv6[MAX_ENTRIES];
        char *name[MAX_ENTRIES];
    } data;
};

static void ipv4_callback(const ipv4_address_t *ipv4, void *userdata) {
    struct userdata *u = userdata;
    assert(ipv4 && userdata);

    if (u->count >= MAX_ENTRIES)
        return;

    u->data.ipv4[u->count++] = *ipv4;
    u->data_len += sizeof(ipv4_address_t);
}

static void ipv6_callback(const ipv6_address_t *ipv6, void *userdata) {
    struct userdata *u = userdata;
    assert(ipv6 && userdata);

    if (u->count >= MAX_ENTRIES)
        return;

    u->data.ipv6[u->count++] = *ipv6;
    u->data_len += sizeof(ipv6_address_t);
}

static void name_callback(const char*name, void *userdata) {
    struct userdata *u = userdata;
    assert(name && userdata);

    if (u->count >= MAX_ENTRIES)
        return;

    u->data.name[u->count++] = strdup(name);
    u->data_len += strlen(name)+1;
}

enum nss_status _nss_mdns_gethostbyname2_r(
    const char *name,
    int af,
    struct hostent * result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop) {

    struct userdata u;
    enum nss_status status = NSS_STATUS_UNAVAIL;
    int fd = -1, r, i;
    size_t address_length, l, index, astart;

/*     DEBUG_TRAP; */

#ifdef NSS_IPV4_ONLY
    if (af != AF_INET) 
#elif NSS_IPV6_ONLY
    if (af != AF_INET6)
#else        
    if (af != AF_INET && af != AF_INET6)
#endif        
    {    
        *errnop = EINVAL;
        *h_errnop = NO_RECOVERY;
        goto finish;
    }

    address_length = af == AF_INET ? sizeof(ipv4_address_t) : sizeof(ipv6_address_t);
    if (buflen <
        sizeof(char*)+    /* alias names */
        strlen(name)+1)  {   /* official name */
        
        *errnop = ERANGE;
        *h_errnop = NO_RECOVERY;
        status = NSS_STATUS_TRYAGAIN;

        goto finish;
    }
    
    if ((fd = mdns_open_socket()) < 0) {

        *errnop = errno;
        *h_errnop = NO_RECOVERY;
        goto finish;
    }

    u.count = 0;
    u.data_len = 0;
    
    if ((r = mdns_query_name(fd, name, af == AF_INET ? ipv4_callback : NULL, af == AF_INET6 ? ipv6_callback : NULL, &u)) < 0) {
        *errnop = ETIMEDOUT;
        *h_errnop = HOST_NOT_FOUND;
        goto finish;
    }

    /* Alias names */
    *((char**) buffer) = NULL;
    result->h_aliases = (char**) buffer;
    index = sizeof(char*);

    /* Official name */
    strcpy(buffer+index, name); 
    result->h_name = buffer+index;
    index += strlen(name)+1;
    
    result->h_addrtype = af;
    result->h_length = address_length;

    /* Check if there's enough space for the addresses */
    if (buflen < index+u.data_len+sizeof(char*)*(u.count+1)) {
        *errnop = ERANGE;
        *h_errnop = NO_RECOVERY;
        status = NSS_STATUS_TRYAGAIN;
        goto finish;
    }

    /* Addresses */
    astart = index;
    l = u.count*address_length;
    memcpy(buffer+astart, &u.data, l);
    index += l;

    /* Address array */
    for (i = 0; i < u.count; i++)
        ((char**) (buffer+index))[i] = buffer+astart+address_length*i;
    ((char**) (buffer+index))[i] = NULL;

    result->h_addr_list = (char**) (buffer+index);

    status = NSS_STATUS_SUCCESS;
    
finish:
    if (fd >= 0)
        close(fd);
    
    return status;
}

enum nss_status _nss_mdns_gethostbyname_r (
    const char *name,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop) {

    return _nss_mdns_gethostbyname2_r(
        name,
#ifdef NSS_IPV6_ONLY
        AF_INET6,
#else
        AF_INET,
#endif        
        result,
        buffer,
        buflen,
        errnop,
        h_errnop);
}

enum nss_status _nss_mdns_gethostbyaddr_r(
    const void* addr,
    int len,
    int af,
    struct hostent *result,
    char *buffer,
    size_t buflen,
    int *errnop,
    int *h_errnop) {
    
    *errnop = EINVAL;
    *h_errnop = NO_RECOVERY;

    struct userdata u;
    enum nss_status status = NSS_STATUS_UNAVAIL;
    int fd = -1, r;
    size_t address_length, index, astart;

    u.count = 0;
    u.data_len = 0;
    
    address_length = af == AF_INET ? sizeof(ipv4_address_t) : sizeof(ipv6_address_t);

    if (len != (int) address_length ||
#ifdef NSS_IPV4_ONLY
        af != AF_INET
#elif NSS_IPV6_ONLY
        af != AF_INET6
#else        
        (af != AF_INET && af != AF_INET6)
#endif
        ) {
        *errnop = EINVAL;
        *h_errnop = NO_RECOVERY;
        goto finish;
    }

    if (buflen <
        sizeof(char*)+      /* alias names */
        address_length) {   /* address */
        
        *errnop = ERANGE;
        *h_errnop = NO_RECOVERY;
        status = NSS_STATUS_TRYAGAIN;

        goto finish;
    }
    
    if ((fd = mdns_open_socket()) < 0) {

        *errnop = errno;
        *h_errnop = NO_RECOVERY;
        goto finish;
    }

    if (af == AF_INET)
        r = mdns_query_ipv4(fd, (ipv4_address_t*) addr, name_callback, &u);
    else
        r = mdns_query_ipv6(fd, (ipv6_address_t*) addr, name_callback, &u);
    
    if (r < 0) {
        *errnop = ETIMEDOUT;
        *h_errnop = HOST_NOT_FOUND;
        goto finish;
    }

    /* Alias names */
    *((char**) buffer) = NULL;
    result->h_aliases = (char**) buffer;
    index = sizeof(char*);

    assert(u.count > 0 && u.data.name[0]);
    if (buflen <
        strlen(u.data.name[0])+1+ /* official names */
        sizeof(char*)+ /* alias names */
        address_length+  /* address */
        sizeof(void*)*2) {  /* address list */

        *errnop = ERANGE;
        *h_errnop = NO_RECOVERY;
        status = NSS_STATUS_TRYAGAIN;
        goto finish;
    }
    
    /* Official name */
    strcpy(buffer+index, u.data.name[0]); 
    result->h_name = buffer+index;
    index += strlen(u.data.name[0])+1;
    
    result->h_addrtype = af;
    result->h_length = address_length;

    /* Address */
    astart = index;
    memcpy(buffer+astart, addr, address_length);
    index += address_length;

    /* Address array */
    ((char**) (buffer+index))[0] = buffer+astart;
    ((char**) (buffer+index))[1] = NULL;
    result->h_addr_list = (char**) (buffer+index);

    status = NSS_STATUS_SUCCESS;
    
finish:
    if (fd >= 0)
        close(fd);
    
    return status;
}
