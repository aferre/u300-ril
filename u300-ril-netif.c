/* ST-Ericsson U300 RIL
**
** Copyright (C) ST-Ericsson AB 2008-2010
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**
**  Author: Sjur Brendeland <sjur.brandeland@stericsson.com>
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if_arp.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/caif/if_caif.h>
#include <assert.h>
#include <errno.h>

#include "u300-ril.h"
#include "u300-ril-netif.h"

#define LOG_TAG "RILV"
#include <utils/Log.h>

#define NLMSG_TAIL(nmsg) \
    ((struct rtattr *) (((uint8_t *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

#define MAX_PAD_SIZE 1024
#define MAX_BUF_SIZE 4096

struct iplink_req {
    struct nlmsghdr n;
    struct ifinfomsg i;
    char pad[MAX_PAD_SIZE];
    int ifindex;
    char ifname[MAX_IFNAME_LEN];
};

static __u32 ipconfig_seqnr = 1;

static bool get_ifname(struct ifinfomsg *msg, int bytes,
                      const char **ifname)
{
    struct rtattr *attr;

    if (ifname == NULL)
        return false;

    for (attr = IFLA_RTA(msg); RTA_OK(attr, bytes);
         attr = RTA_NEXT(attr, bytes)) {
        if (attr->rta_type == IFLA_IFNAME) {
            *ifname = RTA_DATA(attr);
            return true;
        }
    }

    return false;
}

static void handle_rtnl_response(struct iplink_req *req, unsigned short type,
                                 int index, unsigned flags, unsigned change,
                                 struct ifinfomsg *msg, int bytes)
{
    const char *ifname = NULL;

    get_ifname(msg, bytes, &ifname);
    req->ifindex = index;
    strncpy(req->ifname, ifname, sizeof(req->ifname));
    req->ifname[sizeof(req->ifname)-1] = '\0';
}

/**
 * Returns -1 and sets errno on errors.
 */
static int send_iplink_req(int sk, struct iplink_req *req)
{
    struct sockaddr_nl addr;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;

    return sendto(sk, req, req->n.nlmsg_len, 0,
                  (struct sockaddr *) &addr, sizeof(addr));
}

/**
 * Returns 0 on receiving an ACK message;
 * Negative on error;
 * Positive otherwise.
 */
static int parse_rtnl_message(uint8_t *buf, size_t len, struct iplink_req *req)
{
    struct ifinfomsg *msg;

    while (len > 0) {
        struct nlmsghdr *hdr = (struct nlmsghdr *)buf;
        struct nlmsgerr *err;

        if (!NLMSG_OK(hdr, len))
            return -EBADMSG;

        if (hdr->nlmsg_type == NLMSG_ERROR) {
            err = NLMSG_DATA(hdr);
            if (err->error)
                LOGE("%s(): RTNL failed: seq:%d, error %d(%s)\n", __func__,
                      hdr->nlmsg_seq, err->error, strerror(-err->error));

            return err->error;
        } else if (hdr->nlmsg_type == RTM_NEWLINK ||
                   hdr->nlmsg_type == RTM_DELLINK) {
            msg = (struct ifinfomsg *) NLMSG_DATA(hdr);
            handle_rtnl_response(req, msg->ifi_type,
                                 msg->ifi_index, msg->ifi_flags,
                                 msg->ifi_change, msg,
                                 IFA_PAYLOAD(hdr));
        }

        len -= hdr->nlmsg_len;
        buf += hdr->nlmsg_len;
    }

    return 1;
}

/**
 * Returns 0 on success; On failure, errno is set and a negative value returned.
 */
static int netlink_get_response(int sk, struct iplink_req *req)
{
    unsigned char *buf;
    int ret;

    buf = malloc(MAX_BUF_SIZE);
    assert(buf != NULL);

    /*
     * Loops until an ACK message is received (i.e. parse_rtnl_message
     * returns 0) or an error occurs.
     */
    do {
        ret = read(sk, buf, MAX_BUF_SIZE);
        if (ret < 0) {
            if (errno == EINTR) {
                ret = 1;
                continue;
            }
            else
                break;
        }

        /*
         * EOF is treated as error. This may happen when no process
         * has the pipe open for writing or the other end closed
         * the socket orderly.
         */
        if (ret == 0) {
            LOGW("EOF received.\n");
            errno = EIO;
            ret = -1;
            break;
        }

        ret = parse_rtnl_message(buf, ret, req);
        if (ret < 0)
            errno = -ret;
    } while (ret > 0);

    free(buf);
    return ret;
}

static void add_attribute(struct nlmsghdr *n, int maxlen, int type,
                         const void *data, int datalen)
{
    int len = RTA_LENGTH(datalen);
    struct rtattr *rta;

    if ((int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len)) > maxlen) {
        LOGE("%s(): attribute too large for message. nlmsg_len:%d, len:%d, \
             maxlen:%d\n", __func__, n->nlmsg_len, len, maxlen);
        assert(false && "attribute too large for message.");
    }

    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, datalen);

    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
    return;
}

/**
 * Sets errno and returns -1 on error.
 */
static int create_caif_interface(int sk, struct iplink_req *req,
                                 int connection_type, char *ifname,
                                 int nsapi, char loop_enabled)
{
    const char *type = "caif";
    struct rtattr *linkinfo;
    struct rtattr *data;

    req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req->n.nlmsg_type = RTM_NEWLINK;
    req->n.nlmsg_seq = ipconfig_seqnr++;
    req->i.ifi_family = AF_UNSPEC;

    add_attribute(&req->n, sizeof(*req), IFLA_IFNAME,
                  ifname, strlen(ifname));
    linkinfo = NLMSG_TAIL(&req->n);
    add_attribute(&req->n, sizeof(*req), IFLA_LINKINFO,
                  NULL, 0);

    add_attribute(&req->n, sizeof(*req), IFLA_INFO_KIND,
                  type, strlen(type));


    data = NLMSG_TAIL(&req->n);
    add_attribute(&req->n, sizeof(*req), IFLA_INFO_DATA,
                  NULL, 0);

    if (connection_type == IFLA_CAIF_IPV4_CONNID ||
        connection_type == IFLA_CAIF_IPV6_CONNID) {
        add_attribute(&req->n, sizeof(*req),
                      connection_type, &nsapi, sizeof(nsapi));
    } else {
        LOGE("%s(): Unsupported linktype.\n", __func__);
        errno = EINVAL;
        return -1;
    }

    if (loop_enabled)
        add_attribute(&req->n, sizeof(*req),
                      IFLA_CAIF_LOOPBACK, &loop_enabled, sizeof(loop_enabled));


    data->rta_len = (uint8_t *)NLMSG_TAIL(&req->n) - (uint8_t *)data;

    linkinfo->rta_len = (uint8_t *)NLMSG_TAIL(&req->n) - (uint8_t *)linkinfo;

    return send_iplink_req(sk, req);
}

static int destroy_caif_interface(int sk, struct iplink_req *req,
                                  int ifindex, char *ifname)
{

    req->n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req->n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    req->n.nlmsg_type = RTM_DELLINK;
    req->n.nlmsg_seq = ipconfig_seqnr++;
    req->i.ifi_family = AF_UNSPEC;
    req->i.ifi_index = ifindex;

    if (ifname != NULL)
        add_attribute(&req->n, sizeof(*req), IFLA_IFNAME,
                      ifname, strlen(ifname));

    return send_iplink_req(sk, req);
}

/**
 * Returns netlink socket on success; On failure, errno is set and -1 returned.
 */
static int rtnl_init(void)
{
    struct sockaddr_nl addr;
    int sk, ret;

    sk = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);

    /* -1 is returned if failed to create socket. */
    if (sk < 0)
        goto error;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;

    ret = bind(sk, (struct sockaddr *) &addr, sizeof(addr));

    /* -1 is returned if failed to bind a name to a socket. */
    if (ret < 0) {
        /* errno may be clobbered by a successful close. */
        int old_errno = errno;
        close(sk);
        errno = old_errno;
        goto error;
    }

    goto exit;

error:
    sk = -1;

exit:
    return sk;
}

/* Note ifname is in/out and must be minimum size 16 */
int rtnl_create_caif_interface(int type, int conn_id,
                               char ifname[MAX_IFNAME_LEN],
                               int *ifindex, char loop)
{
    int sk, ret;
    struct iplink_req *req;

    req = malloc(sizeof(*req));
    assert(req != NULL);
    memset(req, 0, sizeof(*req));

    sk = rtnl_init();
    ret = sk;
    if (sk < 0)
        goto exit;

    ret = create_caif_interface(sk, req, type, ifname, conn_id, loop);
    if (ret < 0)
        goto exit;

    ret = netlink_get_response(sk, req);
    if (ret < 0)
        goto exit;

    strncpy(ifname, req->ifname, MAX_IFNAME_LEN);
    ifname[MAX_IFNAME_LEN - 1] = '\0';
    *ifindex = req->ifindex;

exit:
    close(sk);
    free(req);
    return ret;
}

int rtnl_delete_caif_interface(int ifid, char *name)
{
    struct iplink_req req;
    int sk, ret;

    memset(&req, 0, sizeof(req));

    sk = rtnl_init();
    ret = sk;

    if (sk < 0)
        return ret;

    ret = destroy_caif_interface(sk, &req, ifid, name);
    if (ret < 0)
        goto exit;

    ret = netlink_get_response(sk, &req);
    if (ret < 0)
        goto exit;

    ret = 0;

exit:
    close(sk);
    return ret;
}
