/*
Copyright 2011 by Matthieu Boutier and Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <zebra.h>
#include "prefix.h"
#include "zclient.h"
#include "kernel.h"
#include "privs.h"
#include "command.h"
#include "vty.h"
#include "memory.h"
#include "thread.h"

#include "util.h"
#include "babel_interface.h"
#include "babel_zebra.h"


static int
kernel_route_add_v4(const unsigned char *pref, unsigned short plen,
                    const unsigned char *gate, int ifindex, unsigned int metric,
                    const unsigned char *newgate, int newifindex,
                    unsigned int newmetric);
static int
kernel_route_add_v6(const unsigned char *pref, unsigned short plen,
                    const unsigned char *gate, int ifindex, unsigned int metric,
                    const unsigned char *newgate, int newifindex,
                    unsigned int newmetric);
static int
kernel_route_delete_v4(const unsigned char *pref, unsigned short plen,
                       const unsigned char *gate, int ifindex,
                       unsigned int metric,
                       const unsigned char *newgate, int newifindex,
                       unsigned int newmetric);
static int
kernel_route_delete_v6(const unsigned char *pref, unsigned short plen,
                       const unsigned char *gate, int ifindex,
                       unsigned int metric,
                       const unsigned char *newgate, int newifindex,
                       unsigned int newmetric);

int
kernel_interface_operational(struct interface *interface)
{
    return if_is_operative(interface);
}

int
kernel_interface_mtu(struct interface *interface)
{
    return MIN(interface->mtu, interface->mtu6);
}

int
kernel_interface_wireless(struct interface *interface)
{
    return 0;
}

int
kernel_route(int operation, const unsigned char *pref, unsigned short plen,
             const unsigned char *gate, int ifindex, unsigned int metric,
             const unsigned char *newgate, int newifindex,
             unsigned int newmetric)
{
    int rc;
    int added;
    int ipv4;

    /* Check that the protocol family is consistent. */
    if(plen >= 96 && v4mapped(pref)) {
        if(!v4mapped(gate)) {
            errno = EINVAL;
            return -1;
        }
        ipv4 = 1;
    } else {
        if(v4mapped(gate)) {
            errno = EINVAL;
            return -1;
        }
        ipv4 = 0;
    }

    switch (operation) {
        case ROUTE_ADD:
            return ipv4 ?
                   kernel_route_add_v4(pref, plen, gate, ifindex, metric,
                                       newgate, newifindex, newmetric):
                   kernel_route_add_v6(pref, plen, gate, ifindex, metric,
                                       newgate, newifindex, newmetric);
            break;
        case ROUTE_FLUSH:
            return ipv4 ?
                   kernel_route_delete_v4(pref, plen, gate, ifindex, metric,
                                          newgate, newifindex, newmetric):
                   kernel_route_delete_v6(pref, plen, gate, ifindex, metric,
                                          newgate, newifindex, newmetric);
            break;
        case ROUTE_MODIFY:
            if(newmetric == metric && memcmp(newgate, gate, 16) == 0 &&
               newifindex == ifindex)
                return 0;
            debugf(BABEL_DEBUG_ROUTE, "Modify route: delete old; add new.");
            if (ipv4) {
                kernel_route_delete_v4(pref, plen,
                                       gate, ifindex, metric,
                                       NULL, 0, 0);
            } else {
                kernel_route_delete_v6(pref, plen,
                                       gate, ifindex, metric,
                                       NULL, 0, 0);
            }

            rc = ipv4 ?
            kernel_route_add_v4(pref, plen,
                                newgate, newifindex, newmetric,
                                NULL, 0, 0):
            kernel_route_add_v6(pref, plen,
                                newgate, newifindex, newmetric,
                                NULL, 0, 0);
            if(rc < 0) {
                if(errno == EEXIST)
                    rc = 1;
                /* In principle, we should try to re-install the flushed
                 route on failure to preserve.  However, this should
                 hopefully not matter much in practice. */
            }

            return rc;
            break;
        default:
            zlog_err("this should never appens (false value - kernel_route)");
            assert(0);
            exit(1);
            break;
    }
}

static int
kernel_route_add_v4(const unsigned char *pref, unsigned short plen,
                    const unsigned char *gate, int ifindex, unsigned int metric,
                    const unsigned char *newgate, int newifindex,
                    unsigned int newmetric)
{
    unsigned int tmp_ifindex = ifindex; /* (for typing) */
    struct zapi_ipv4 api;               /* quagga's communication system */
    struct prefix_ipv4 quagga_prefix;   /* quagga's prefix */
    struct in_addr babel_prefix_addr;   /* babeld's prefix addr */
    struct in_addr nexthop;             /* next router to go */
    struct in_addr *nexthop_pointer = &nexthop; /* it's an array! */

    /* convert to be comprehensive by quagga */
    /* convert given addresses */
    uchar_to_inaddr(&babel_prefix_addr, pref);
    uchar_to_inaddr(&nexthop, gate);

    /* make prefix structure */
    memset (&quagga_prefix, 0, sizeof(quagga_prefix));
    quagga_prefix.family = AF_INET;
    IPV4_ADDR_COPY (&quagga_prefix.prefix, &babel_prefix_addr);
    quagga_prefix.prefixlen = plen - 96; /* our plen is for v4mapped's addr */
    apply_mask_ipv4(&quagga_prefix);

    api.type  = ZEBRA_ROUTE_BABEL;
    api.flags = 0;
    api.message = 0;
    api.safi = SAFI_UNICAST;
    SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
    api.nexthop_num = 1;
    api.nexthop = &nexthop_pointer;
    SET_FLAG(api.message, ZAPI_MESSAGE_IFINDEX);
    api.ifindex_num = 1;
    api.ifindex = &tmp_ifindex;
    SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);
    api.metric = metric;

    debugf(BABEL_DEBUG_ROUTE, "adding route (ipv4) to zebra");
    return zapi_ipv4_route (ZEBRA_IPV4_ROUTE_ADD, zclient,
                            &quagga_prefix, &api);
}

static int
kernel_route_add_v6(const unsigned char *pref, unsigned short plen,
                    const unsigned char *gate, int ifindex, unsigned int metric,
                    const unsigned char *newgate, int newifindex,
                    unsigned int newmetric)
{
    unsigned int tmp_ifindex = ifindex; /* (for typing) */
    struct zapi_ipv6 api;               /* quagga's communication system */
    struct prefix_ipv6 quagga_prefix;   /* quagga's prefix */
    struct in6_addr babel_prefix_addr;  /* babeld's prefix addr */
    struct in6_addr nexthop;            /* next router to go */
    struct in6_addr *nexthop_pointer = &nexthop;

    /* convert to be comprehensive by quagga */
    /* convert given addresses */
    uchar_to_in6addr(&babel_prefix_addr, pref);
    uchar_to_in6addr(&nexthop, gate);

    /* make prefix structure */
    memset (&quagga_prefix, 0, sizeof(quagga_prefix));
    quagga_prefix.family = AF_INET6;
    IPV6_ADDR_COPY (&quagga_prefix.prefix, &babel_prefix_addr);
    quagga_prefix.prefixlen = plen;
    apply_mask_ipv6(&quagga_prefix);

    api.type  = ZEBRA_ROUTE_BABEL;
    api.flags = 0;
    api.message = 0;
    api.safi = SAFI_UNICAST;
    SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
    api.nexthop_num = 1;
    api.nexthop = &nexthop_pointer;
    SET_FLAG(api.message, ZAPI_MESSAGE_IFINDEX);
    api.ifindex_num = 1;
    api.ifindex = &tmp_ifindex;
    SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);
    api.metric = metric;

    debugf(BABEL_DEBUG_ROUTE, "adding route (ipv6) to zebra");
    return zapi_ipv6_route (ZEBRA_IPV6_ROUTE_ADD, zclient,
                            &quagga_prefix, &api);
}

static int
kernel_route_delete_v4(const unsigned char *pref, unsigned short plen,
                       const unsigned char *gate, int ifindex,
                       unsigned int metric,
                       const unsigned char *newgate, int newifindex,
                       unsigned int newmetric)
{
    unsigned int tmp_ifindex = ifindex; /* (for typing) */
    struct zapi_ipv4 api;               /* quagga's communication system */
    struct prefix_ipv4 quagga_prefix;   /* quagga's prefix */
    struct in_addr babel_prefix_addr;   /* babeld's prefix addr */
    struct in_addr nexthop;             /* next router to go */
    struct in_addr *nexthop_pointer = &nexthop; /* it's an array! */

    /* convert to be comprehensive by quagga */
    /* convert given addresses */
    uchar_to_inaddr(&babel_prefix_addr, pref);
    uchar_to_inaddr(&nexthop, gate);

    /* make prefix structure */
    memset (&quagga_prefix, 0, sizeof(quagga_prefix));
    quagga_prefix.family = AF_INET;
    IPV4_ADDR_COPY (&quagga_prefix.prefix, &babel_prefix_addr);
    quagga_prefix.prefixlen = plen - 96;
    apply_mask_ipv4(&quagga_prefix);

    api.type  = ZEBRA_ROUTE_BABEL;
    api.flags = 0;
    api.message = 0;
    api.safi = SAFI_UNICAST;
    SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
    api.nexthop_num = 1;
    api.nexthop = &nexthop_pointer;
    SET_FLAG(api.message, ZAPI_MESSAGE_IFINDEX);
    api.ifindex_num = 1;
    api.ifindex = &tmp_ifindex;
    SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);
    api.metric = metric;

    debugf(BABEL_DEBUG_ROUTE, "removing route (ipv4) to zebra");
    return zapi_ipv4_route (ZEBRA_IPV4_ROUTE_DELETE, zclient,
                            &quagga_prefix, &api);
}

static int
kernel_route_delete_v6(const unsigned char *pref, unsigned short plen,
                       const unsigned char *gate, int ifindex,
                       unsigned int metric,
                       const unsigned char *newgate, int newifindex,
                       unsigned int newmetric)
{
    unsigned int tmp_ifindex = ifindex; /* (for typing) */
    struct zapi_ipv6 api;               /* quagga's communication system */
    struct prefix_ipv6 quagga_prefix;   /* quagga's prefix */
    struct in6_addr babel_prefix_addr;  /* babeld's prefix addr */
    struct in6_addr nexthop;            /* next router to go */
    struct in6_addr *nexthop_pointer = &nexthop;

    /* convert to be comprehensive by quagga */
    /* convert given addresses */
    uchar_to_in6addr(&babel_prefix_addr, pref);
    uchar_to_in6addr(&nexthop, gate);

    /* make prefix structure */
    memset (&quagga_prefix, 0, sizeof(quagga_prefix));
    quagga_prefix.family = AF_INET6;
    IPV6_ADDR_COPY (&quagga_prefix.prefix, &babel_prefix_addr);
    quagga_prefix.prefixlen = plen;
    apply_mask_ipv6(&quagga_prefix);

    api.type  = ZEBRA_ROUTE_BABEL;
    api.flags = 0;
    api.message = 0;
    api.safi = SAFI_UNICAST;
    SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
    api.nexthop_num = 1;
    api.nexthop = &nexthop_pointer;
    SET_FLAG(api.message, ZAPI_MESSAGE_IFINDEX);
    api.ifindex_num = 1;
    api.ifindex = &tmp_ifindex;

    debugf(BABEL_DEBUG_ROUTE, "removing route (ipv6) to zebra");
    return zapi_ipv6_route (ZEBRA_IPV6_ROUTE_DELETE, zclient,
                            &quagga_prefix, &api);
}

int
if_eui64(char *ifname, int ifindex, unsigned char *eui)
{
    struct interface *ifp = if_lookup_by_index(ifindex);
    if (ifp == NULL) {
        return -1;
    }
#ifdef HAVE_STRUCT_SOCKADDR_DL
    u_char len = ifp->sdl.sdl_alen;
    char *tmp = ifp->sdl.sdl_data + ifp->sdl.sdl_nlen;
#else
    u_char len = (u_char) ifp->hw_addr_len;
    char *tmp = (void*) ifp->hw_addr;
#endif
    if (len == 8) {
        memcpy(eui, tmp, 8);
        eui[0] ^= 2;
    } else if (len == 6) {
        memcpy(eui,   tmp,   3);
        eui[3] = 0xFF;
        eui[4] = 0xFE;
        memcpy(eui+5, tmp+3, 3);
    } else if (len > 8) {
        memcpy(eui, tmp, 8);
    } else if (len > 0){
        memset(eui, 0, 8 - len);
        memcpy(eui + 8 - len, tmp, len);
    } else {
        return -1;
    }
    return 0;
}