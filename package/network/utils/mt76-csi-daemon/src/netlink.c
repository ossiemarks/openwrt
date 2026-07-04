#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
	[MTK_VENDOR_ATTR_CSI_DATA_TX_ANT] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_MODE] = { .type = NLA_U8 },
	[MTK_VENDOR_ATTR_CSI_DATA_CHAIN_INFO] = { .type = NLA_U32 },
};

struct dump_ctx {
	csi_queue_t *q;
	int got;
};

static int csi_dump_cb(struct nl_msg *msg, void *arg)
{
	static struct unl dummy; /* unl_find_attr does not use the handle */
	struct dump_ctx *ctx = arg;
	struct nlattr *tb[NUM_MTK_VENDOR_ATTRS_CSI_CTRL];
	struct nlattr *td[NUM_MTK_VENDOR_ATTRS_CSI_DATA];
	struct nlattr *attr, *cur;
	csi_frame_t f;
	int rem, idx;

	attr = unl_find_attr(&dummy, msg, NL80211_ATTR_VENDOR_DATA);
	if (!attr)
		return NL_SKIP;

	if (nla_parse_nested(tb, MTK_VENDOR_ATTR_CSI_CTRL_MAX, attr,
			     csi_ctrl_policy))
		return NL_SKIP;

	if (!tb[MTK_VENDOR_ATTR_CSI_CTRL_DATA])
		return NL_SKIP;

	if (nla_parse_nested(td, MTK_VENDOR_ATTR_CSI_DATA_MAX,
			     tb[MTK_VENDOR_ATTR_CSI_CTRL_DATA], csi_data_policy))
		return NL_SKIP;

	if (!(td[MTK_VENDOR_ATTR_CSI_DATA_VER] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_TS] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_TA] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_NUM] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_I] &&
	      td[MTK_VENDOR_ATTR_CSI_DATA_Q]))
		return NL_SKIP;

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
		f.tx_idx = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_TX_ANT]);
	if (td[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT])
		f.rx_idx = nla_get_u8(td[MTK_VENDOR_ATTR_CSI_DATA_RX_ANT]);
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

static int csi_set_cfg(const char *iface, uint8_t mode, uint8_t type,
		       uint8_t v1, uint8_t v2, const uint8_t *mac,
		       unsigned sta_interval)
{
	struct unl unl;
	struct nl_msg *msg;
	void *data, *cfg;
	int ifidx, ret, i;

	ifidx = if_nametoindex(iface);
	if (!ifidx)
		return -1;

	if (unl_genl_init(&unl, "nl80211") < 0)
		return -1;

	msg = unl_genl_msg(&unl, NL80211_CMD_VENDOR, false);
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

	ret = unl_genl_request(&unl, msg, NULL, NULL);
	unl_free(&unl);
	return ret;
}

static int parse_mac(const char *s, uint8_t *mac)
{
	return sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		      &mac[0], &mac[1], &mac[2],
		      &mac[3], &mac[4], &mac[5]) == 6 ? 0 : -1;
}

int csi_nl_enable(const char *iface, const char *sta_mac, unsigned interval)
{
	int ret;

	if (sta_mac && *sta_mac) {
		uint8_t mac[6];

		if (parse_mac(sta_mac, mac))
			return -1;
		/* mode 2 / type 8 / v1 1 / v2 ADD(1): add station filter */
		ret = csi_set_cfg(iface, 2, 8, 1, 1, mac, interval);
		if (ret)
			fprintf(stderr, "csi: mac filter add failed (%d)\n", ret);
	}

	/* mode 1: start CSI capture */
	ret = csi_set_cfg(iface, 1, 0, 0, 0, NULL, 0);
	if (ret)
		fprintf(stderr, "csi: enable failed (%d)\n", ret);
	return ret;
}

int csi_nl_disable(const char *iface)
{
	return csi_set_cfg(iface, 0, 0, 0, 0, NULL, 0);
}

int csi_nl_reader_run(const char *iface, csi_queue_t *q, volatile bool *running)
{
	int ifidx = if_nametoindex(iface);

	if (!ifidx) {
		fprintf(stderr, "csi: no such interface: %s\n", iface);
		return -1;
	}

	while (*running) {
		struct unl unl;
		struct nl_msg *msg;
		struct dump_ctx ctx = { .q = q, .got = 0 };
		void *data;
		int ret;

		if (unl_genl_init(&unl, "nl80211") < 0) {
			usleep(200000);
			continue;
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
		unl_free(&unl);

		if (ret < 0 && ret != -2 /* -ENOENT: queue empty */)
			usleep(200000);
		else if (!ctx.got)
			usleep(20000); /* nothing queued; don't spin */
	}

	return 0;
}
