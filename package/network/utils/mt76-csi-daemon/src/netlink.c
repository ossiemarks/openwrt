#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>

#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <unl.h>
#include <linux/nl80211.h>

#include "mtk_vendor_nl.h"
#include "netlink.h"

#define CSI_DUMP_PER_REQ 3

static struct nla_policy csi_ctrl_policy[NUM_MTK_VENDOR_ATTRS_CSI_CTRL] = {
	[MTK_VENDOR_ATTR_CSI_CTRL_CFG] = { .type = NLA_NESTED },
	[MTK_VENDOR_ATTR_CSI_CTRL_DATA] = { .type = NLA_NESTED },
	[MTK_VENDOR_ATTR_CSI_CTRL_DUMP_NUM] = { .type = NLA_U16 },
};

static struct nla_policy csi_data_policy[NUM_MTK_VENDOR_ATTRS_CSI_DATA] = {
	[MTK_VENDOR_ATTR_CSI_DATA_VER] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_TS] = { .type = NLA_U32 },
	[MTK_VENDOR_ATTR_CSI_DATA_RSSI] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_SNR] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_BW] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_CH_IDX] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_TA] = { .type = NLA_NESTED },
	[MTK_VENDOR_ATTR_CSI_DATA_NUM] = { .type = NLA_U32 },
	[MTK_VENDOR_ATTR_CSI_DATA_I] = { .type = NLA_NESTED },
	[MTK_VENDOR_ATTR_CSI_DATA_Q] = { .type = NLA_NESTED },
	[MTK_VENDOR_ATTR_CSI_DATA_INFO] = { .type = NLA_U32 },
	[MTK_VENDOR_ATTR_CSI_DATA_TX_ANT] = { .type = NLA_U16 },
	[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT] = { .type = NLA_U16 },
	[MTK_VENDOR_ATTR_CSI_DATA_MODE] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_CHAIN_INFO] = { .type = NLA_U32 },
};

struct dump_ctx {
	struct unl *unl;
	csi_queue_t *q;
	int got;
};

/* Rate-limited note about why a record was rejected; a rejected record is
 * already gone from the kernel queue, so silence here hides real loss. */
static void reject(const char *why, int err)
{
	static unsigned long n;

	if (n++ % 1000 == 0)
		fprintf(stderr, "csi: dump record rejected: %s (%d), %lu so far\n",
			why, err, n);
}

static int csi_dump_cb(struct nl_msg *msg, void *arg)
{
	struct dump_ctx *ctx = arg;
	struct nlattr *tb[NUM_MTK_VENDOR_ATTRS_CSI_CTRL];
	struct nlattr *td[NUM_MTK_VENDOR_ATTRS_CSI_DATA];
	struct nlattr *attr, *cur;
	csi_frame_t f;
	int rem, idx, err;

	attr = unl_find_attr(ctx->unl, msg, NL80211_ATTR_VENDOR_DATA);
	if (!attr) {
		reject("no vendor data", 0);
		return NL_SKIP;
	}

	err = nla_parse_nested(tb, MTK_VENDOR_ATTR_CSI_CTRL_MAX, attr,
			       csi_ctrl_policy);
	if (err) {
		reject("ctrl parse", err);
		return NL_SKIP;
	}

	if (!tb[MTK_VENDOR_ATTR_CSI_CTRL_DATA]) {
		reject("no DATA attr", 0);
		return NL_SKIP;
	}

	err = nla_parse_nested(td, MTK_VENDOR_ATTR_CSI_DATA_MAX,
			       tb[MTK_VENDOR_ATTR_CSI_CTRL_DATA], csi_data_policy);
	if (err) {
		reject("data parse", err);
		return NL_SKIP;
	}

	if (!(td[MTK_VENDOR_ATTR_CSI_DATA_VER] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_TS] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_TA] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_NUM] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_I] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_Q])) {
		reject("missing attrs", 0);
		return NL_SKIP;
	}

	memset(&f, 0, sizeof(f));
	f.ts = nla_get_u32(td[MTK_VENDOR_ATTR_CSI_DATA_TS]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_RSSI])
		f.rssi = (int8_t)nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_RSSI]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_SNR])
		f.snr = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_SNR]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_BW])
		f.data_bw = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_BW]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_CH_IDX])
		f.pri_ch_idx = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_CH_IDX]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_MODE])
		f.rx_mode = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_MODE]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_TX_ANT])
		f.tx_idx = nla_get_u16(td[MTK_VENDOR_ATTR_CSI_DATA_TX_ANT]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT])
		f.rx_idx = nla_get_u16(td[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_INFO])
		f.ext_info = nla_get_u32(td[MTK_VENDOR_ATTR_CSI_DATA_INFO]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_CHAIN_INFO])
		f.chain_info = nla_get_u32(td[MTK_VENDOR_ATTR_CSI_DATA_CHAIN_INFO]);

	idx = 0;
	nla_for_each_nested(cur, td[MTK_VENDOR_ATTR_CSI_DATA_TA], rem) {
		if (idx < 6)
			f.ta[idx++] = nla_get_u8(cur);
	}

	f.data_num = nla_get_u32(td[MTK_VENDOR_ATTR_CSI_DATA_NUM]);
	if (f.data_num > CSI_MAX_SUBCARRIERS)
		f.data_num = CSI_MAX_SUBCARRIERS;

	idx = 0;
	nla_for_each_nested(cur, td[MTK_VENDOR_ATTR_CSI_DATA_I], rem) {
		if (idx < f.data_num)
			f.data_i[idx++] = (int16_t)nla_get_u16(cur);
	}
	idx = 0;
	nla_for_each_nested(cur, td[MTK_VENDOR_ATTR_CSI_DATA_Q], rem) {
		if (idx < f.data_num)
			f.data_q[idx++] = (int16_t)nla_get_u16(cur);
	}

	queue_push(ctx->q, &f);
	ctx->got++;

	return NL_SKIP;
}

/*
 * Persistent nl80211 handles. Re-creating a socket plus a genl family cache
 * per request leaked ~6 KB per CSI frame through libnl-tiny and OOM-killed
 * the daemon every half hour at 80 frames/s. ctl_unl is used from the main
 * thread only; the reader thread owns its own handle.
 */
static struct unl ctl_unl;
static bool ctl_ready;

static struct unl *ctl(void)
{
	if (!ctl_ready) {
		if (unl_genl_init(&ctl_unl, "nl80211") < 0)
			return NULL;
		ctl_ready = true;
	}
	return &ctl_unl;
}

static void ctl_reset(void)
{
	if (ctl_ready)
		unl_free(&ctl_unl);
	ctl_ready = false;
}

static int csi_set_cfg(const char *iface, uint8_t mode, uint8_t type,
		       uint8_t v1, uint8_t v2, const uint8_t *mac,
		       unsigned sta_interval)
{
	struct unl *unl = ctl();
	struct nl_msg *msg;
	void *data, *cfg;
	int ifidx, ret, i;

	ifidx = if_nametoindex(iface);
	if (!ifidx || !unl)
		return -1;

	msg = unl_genl_msg(unl, NL80211_CMD_VENDOR, false);
	nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifidx);
	nla_put_u32(msg, NL80211_ATTR_VENDOR_ID, MTK_NL80211_VENDOR_ID);
	nla_put_u32(msg, NL80211_ATTR_VENDOR_SUBCMD,
		    MTK_NL80211_VENDOR_SUBCMD_CSI_CTRL);

	data = nla_nest_start(msg, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED);

	cfg = nla_nest_start(msg, MTK_VENDOR_ATTR_CSI_CTRL_CFG | NLA_F_NESTED);
	nla_put_u8(msg, MTK_VENDOR_ATTR_CSI_CTRL_CFG_MODE, mode);
	nla_put_u8(msg, MTK_VENDOR_ATTR_CSI_CTRL_CFG_TYPE, type);
	nla_put_u8(msg, MTK_VENDOR_ATTR_CSI_CTRL_CFG_VAL1, v1);
	nla_put_u8(msg, MTK_VENDOR_ATTR_CSI_CTRL_CFG_VAL2, v2);
	nla_nest_end(msg, cfg);

	if (mac) {
		void *m = nla_nest_start(msg,
			MTK_VENDOR_ATTR_CSI_CTRL_MAC_ADDR | NLA_F_NESTED);
		for (i = 0; i < 6; i++)
			nla_put_u8(msg, i, mac[i]);
		nla_nest_end(msg, m);
		if (sta_interval)
			nla_put_u32(msg, MTK_VENDOR_ATTR_CSI_CTRL_STA_INTERVAL,
				    sta_interval);
	}

	nla_nest_end(msg, data);

	ret = unl_genl_request(unl, msg, NULL, NULL);
	if (ret < 0 && ret != -ENOENT && ret != -EINVAL)
		ctl_reset(); /* socket-level trouble: rebuild next time */
	return ret;
}

/* mode 2 / type 8 / v1 1 / v2: ADD_CSI_MAC (1) or DEL_CSI_MAC (0) */
#define CSI_MAC_DEL 0
#define CSI_MAC_ADD 1

int csi_nl_sta_filter(const char *iface, const uint8_t *mac, bool add,
		      unsigned interval)
{
	int ret = csi_set_cfg(iface, 2, 8, 1, add ? CSI_MAC_ADD : CSI_MAC_DEL,
			      mac, add ? interval : 0);

	if (ret)
		fprintf(stderr, "csi: mac filter %s failed (%d)\n",
			add ? "add" : "del", ret);
	return ret;
}

int csi_nl_set_frame_type(const char *iface, uint8_t v1, uint8_t v2)
{
	int ret = csi_set_cfg(iface, 2, 3, v1, v2, NULL, 0);

	if (ret)
		fprintf(stderr, "csi: frame type filter failed (%d)\n", ret);
	return ret;
}

int csi_nl_enable(const char *iface)
{
	/* mode 1: start CSI capture */
	int ret = csi_set_cfg(iface, 1, 0, 0, 0, NULL, 0);

	if (ret)
		fprintf(stderr, "csi: enable failed (%d)\n", ret);
	return ret;
}

int csi_nl_disable(const char *iface)
{
	return csi_set_cfg(iface, 0, 0, 0, 0, NULL, 0);
}

static int nl80211_parse(struct nl_msg *msg, struct nlattr **tb)
{
	struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

	return nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
			 genlmsg_attrlen(gnlh, 0), NULL);
}

struct sta_ctx {
	uint8_t (*macs)[6];
	int max;
	int total;
};

static int sta_dump_cb(struct nl_msg *msg, void *arg)
{
	struct sta_ctx *ctx = arg;
	struct nlattr *tb[NL80211_ATTR_MAX + 1];

	if (nl80211_parse(msg, tb) || !tb[NL80211_ATTR_MAC])
		return NL_SKIP;

	if (ctx->macs && ctx->total < ctx->max)
		memcpy(ctx->macs[ctx->total], nla_data(tb[NL80211_ATTR_MAC]), 6);
	ctx->total++;

	return NL_SKIP;
}

int csi_nl_list_stations(const char *iface, uint8_t (*macs)[6], int max)
{
	struct sta_ctx ctx = { .macs = macs, .max = max, .total = 0 };
	struct unl *unl = ctl();
	struct nl_msg *msg;
	int ifidx, ret;

	ifidx = if_nametoindex(iface);
	if (!ifidx || !unl)
		return -1;

	msg = unl_genl_msg(unl, NL80211_CMD_GET_STATION, true);
	nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifidx);

	ret = unl_genl_request(unl, msg, sta_dump_cb, &ctx);
	if (ret < 0) {
		ctl_reset();
		return -1;
	}

	return ctx.total;
}

#define MAX_AP_IFACES 8

struct iface_ctx {
	char names[MAX_AP_IFACES][IF_NAMESIZE];
	int n;
};

static int iface_dump_cb(struct nl_msg *msg, void *arg)
{
	struct iface_ctx *ctx = arg;
	struct nlattr *tb[NL80211_ATTR_MAX + 1];

	if (nl80211_parse(msg, tb) ||
	    !tb[NL80211_ATTR_IFNAME] || !tb[NL80211_ATTR_IFTYPE])
		return NL_SKIP;

	if (nla_get_u32(tb[NL80211_ATTR_IFTYPE]) != NL80211_IFTYPE_AP)
		return NL_SKIP;

	if (ctx->n < MAX_AP_IFACES)
		snprintf(ctx->names[ctx->n++], IF_NAMESIZE, "%s",
			 nla_get_string(tb[NL80211_ATTR_IFNAME]));

	return NL_SKIP;
}

int csi_nl_find_ap_iface(char *buf, size_t len)
{
	struct iface_ctx ctx = { .n = 0 };
	struct unl *unl = ctl();
	struct nl_msg *msg;
	int i, best = -1, best_stas = -1;

	if (!unl)
		return -1;

	msg = unl_genl_msg(unl, NL80211_CMD_GET_INTERFACE, true);
	if (unl_genl_request(unl, msg, iface_dump_cb, &ctx) < 0) {
		ctl_reset();
		return -1;
	}

	for (i = 0; i < ctx.n; i++) {
		int stas = csi_nl_list_stations(ctx.names[i], NULL, 0);

		if (stas > best_stas) {
			best_stas = stas;
			best = i;
		}
	}

	if (best < 0)
		return -1;

	snprintf(buf, len, "%s", ctx.names[best]);
	return best_stas;
}

int csi_nl_reader_run(const char *iface, csi_queue_t *q, volatile bool *running)
{
	struct unl unl;
	bool ready = false;

	while (*running) {
		struct nl_msg *msg;
		struct dump_ctx ctx = { .unl = &unl, .q = q, .got = 0 };
		void *data;
		int ret;
		/* re-resolved every pass: the daemon may switch interface */
		int ifidx = if_nametoindex(iface);

		if (!ifidx) {
			usleep(500000);
			continue;
		}

		if (!ready) {
			if (unl_genl_init(&unl, "nl80211") < 0) {
				usleep(200000);
				continue;
			}
			ready = true;
		}

		msg = unl_genl_msg(&unl, NL80211_CMD_VENDOR, true);
		nla_put_u32(msg, NL80211_ATTR_IFINDEX, ifidx);
		nla_put_u32(msg, NL80211_ATTR_VENDOR_ID, MTK_NL80211_VENDOR_ID);
		nla_put_u32(msg, NL80211_ATTR_VENDOR_SUBCMD,
			    MTK_NL80211_VENDOR_SUBCMD_CSI_CTRL);

		data = nla_nest_start(msg, NL80211_ATTR_VENDOR_DATA | NLA_F_NESTED);
		nla_put_u16(msg, MTK_VENDOR_ATTR_CSI_CTRL_DUMP_NUM,
			    CSI_DUMP_PER_REQ);
		nla_nest_end(msg, data);

		ret = unl_genl_request(&unl, msg, csi_dump_cb, &ctx);

		if (ret < 0 && ret != -ENOENT /* queue empty */) {
			/* rebuild the socket on real errors */
			unl_free(&unl);
			ready = false;
			usleep(200000);
		} else if (!ctx.got) {
			usleep(20000); /* nothing queued; don't spin */
		}
	}

	if (ready)
		unl_free(&unl);

	return 0;
}
