/*-
 *   BSD LICENSE
 *
 *   Copyright 2015 6WIND S.A.
 *   Copyright 2015 Mellanox.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of 6WIND S.A. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <net/if.h>

/* Verbs header. */
/* ISO C doesn't support unnamed structs/unions, disabling -pedantic. */
#ifdef PEDANTIC
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <infiniband/verbs.h>
#ifdef PEDANTIC
#pragma GCC diagnostic error "-Wpedantic"
#endif

#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_ethdev_pci.h>
#include <rte_pci.h>
#include <rte_common.h>
#include <rte_kvargs.h>

#include "mlx5.h"
#include "mlx5_utils.h"
#include "mlx5_rxtx.h"
#include "mlx5_autoconf.h"
#include "mlx5_defs.h"

/* Device parameter to enable RX completion queue compression. */
#define MLX5_RXQ_CQE_COMP_EN "rxq_cqe_comp_en"

/* Device parameter to configure inline send. */
#define MLX5_TXQ_INLINE "txq_inline"

/*
 * Device parameter to configure the number of TX queues threshold for
 * enabling inline send.
 */
#define MLX5_TXQS_MIN_INLINE "txqs_min_inline"

/* Device parameter to enable multi-packet send WQEs. */
#define MLX5_TXQ_MPW_EN "txq_mpw_en"

/* Device parameter to include 2 dsegs in the title WQEBB. */
#define MLX5_TXQ_MPW_HDR_DSEG_EN "txq_mpw_hdr_dseg_en"

/* Device parameter to limit the size of inlining packet. */
#define MLX5_TXQ_MAX_INLINE_LEN "txq_max_inline_len"

/* Device parameter to enable hardware TSO offload. */
#define MLX5_TSO "tso"

/* Device parameter to enable hardware Tx vector. */
#define MLX5_TX_VEC_EN "tx_vec_en"

/* Device parameter to enable hardware Rx vector. */
#define MLX5_RX_VEC_EN "rx_vec_en"

/* Default PMD specific parameter value. */
#define MLX5_ARG_UNSET (-1)

struct mlx5_args {
	int cqe_comp;
	int txq_inline;
	int txqs_inline;
	int mps;
	int mpw_hdr_dseg;
	int inline_max_packet_sz;
	int tso;
	int tx_vec_en;
	int rx_vec_en;
};
/**
 * Retrieve integer value from environment variable.
 *
 * @param[in] name
 *   Environment variable name.
 *
 * @return
 *   Integer value, 0 if the variable is not set.
 */
int
mlx5_getenv_int(const char *name)
{
	const char *val = getenv(name);

	if (val == NULL)
		return 0;
	return atoi(val);
}

/**
 * DPDK callback to close the device.
 *
 * Destroy all queues and objects, free memory.
 *
 * @param dev
 *   Pointer to Ethernet device structure.
 */
static void
mlx5_dev_close(struct rte_eth_dev *dev)
{
	struct priv *priv = mlx5_get_priv(dev);
	unsigned int i;

	priv_lock(priv);
	DEBUG("%p: closing device \"%s\"",
	      (void *)dev,
	      ((priv->ctx != NULL) ? priv->ctx->device->name : ""));
	/* In case mlx5_dev_stop() has not been called. */
	priv_dev_interrupt_handler_uninstall(priv, dev);
	priv_special_flow_disable_all(priv);
	priv_mac_addrs_disable(priv);
	priv_destroy_hash_rxqs(priv);

	/* Remove flow director elements. */
	priv_fdir_disable(priv);
	priv_fdir_delete_filters_list(priv);

	/* Prevent crashes when queues are still in use. */
	dev->rx_pkt_burst = removed_rx_burst;
	dev->tx_pkt_burst = removed_tx_burst;
	if (priv->rxqs != NULL) {
		/* XXX race condition if mlx5_rx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->rxqs_n); ++i) {
			struct rxq *rxq = (*priv->rxqs)[i];
			struct rxq_ctrl *rxq_ctrl;

			if (rxq == NULL)
				continue;
			rxq_ctrl = container_of(rxq, struct rxq_ctrl, rxq);
			(*priv->rxqs)[i] = NULL;
			rxq_cleanup(rxq_ctrl);
			rte_free(rxq_ctrl);
		}
		priv->rxqs_n = 0;
		priv->rxqs = NULL;
	}
	if (priv->txqs != NULL) {
		/* XXX race condition if mlx5_tx_burst() is still running. */
		usleep(1000);
		for (i = 0; (i != priv->txqs_n); ++i) {
			struct txq *txq = (*priv->txqs)[i];
			struct txq_ctrl *txq_ctrl;

			if (txq == NULL)
				continue;
			txq_ctrl = container_of(txq, struct txq_ctrl, txq);
			(*priv->txqs)[i] = NULL;
			txq_cleanup(txq_ctrl);
			rte_free(txq_ctrl);
		}
		priv->txqs_n = 0;
		priv->txqs = NULL;
	}
	if (priv->pd != NULL) {
		assert(priv->ctx != NULL);
		claim_zero(ibv_dealloc_pd(priv->pd));
		claim_zero(ibv_close_device(priv->ctx));
	} else
		assert(priv->ctx == NULL);
	if (priv->rss_conf != NULL) {
		for (i = 0; (i != hash_rxq_init_n); ++i)
			rte_free((*priv->rss_conf)[i]);
		rte_free(priv->rss_conf);
	}
	if (priv->reta_idx != NULL)
		rte_free(priv->reta_idx);
	priv_unlock(priv);
	memset(priv, 0, sizeof(*priv));
}

static const struct eth_dev_ops mlx5_dev_ops = {
	.dev_configure = mlx5_dev_configure,
	.dev_start = mlx5_dev_start,
	.dev_stop = mlx5_dev_stop,
	.dev_set_link_down = mlx5_set_link_down,
	.dev_set_link_up = mlx5_set_link_up,
	.dev_close = mlx5_dev_close,
	.promiscuous_enable = mlx5_promiscuous_enable,
	.promiscuous_disable = mlx5_promiscuous_disable,
	.allmulticast_enable = mlx5_allmulticast_enable,
	.allmulticast_disable = mlx5_allmulticast_disable,
	.link_update = mlx5_link_update,
	.stats_get = mlx5_stats_get,
	.stats_reset = mlx5_stats_reset,
	.xstats_get = mlx5_xstats_get,
	.xstats_reset = mlx5_xstats_reset,
	.xstats_get_names = mlx5_xstats_get_names,
	.dev_infos_get = mlx5_dev_infos_get,
	.dev_supported_ptypes_get = mlx5_dev_supported_ptypes_get,
	.vlan_filter_set = mlx5_vlan_filter_set,
	.rx_queue_setup = mlx5_rx_queue_setup,
	.tx_queue_setup = mlx5_tx_queue_setup,
	.rx_queue_release = mlx5_rx_queue_release,
	.tx_queue_release = mlx5_tx_queue_release,
	.flow_ctrl_get = mlx5_dev_get_flow_ctrl,
	.flow_ctrl_set = mlx5_dev_set_flow_ctrl,
	.mac_addr_remove = mlx5_mac_addr_remove,
	.mac_addr_add = mlx5_mac_addr_add,
	.mac_addr_set = mlx5_mac_addr_set,
	.mtu_set = mlx5_dev_set_mtu,
	.vlan_strip_queue_set = mlx5_vlan_strip_queue_set,
	.vlan_offload_set = mlx5_vlan_offload_set,
	.reta_update = mlx5_dev_rss_reta_update,
	.reta_query = mlx5_dev_rss_reta_query,
	.rss_hash_update = mlx5_rss_hash_update,
	.rss_hash_conf_get = mlx5_rss_hash_conf_get,
	.filter_ctrl = mlx5_dev_filter_ctrl,
	.rx_descriptor_status = mlx5_rx_descriptor_status,
	.tx_descriptor_status = mlx5_tx_descriptor_status,
#ifdef HAVE_UPDATE_CQ_CI
	.rx_queue_intr_enable = mlx5_rx_intr_enable,
	.rx_queue_intr_disable = mlx5_rx_intr_disable,
#endif
};

static struct {
	struct rte_pci_addr pci_addr; /* associated PCI address */
	uint32_t ports; /* physical ports bitfield. */
} mlx5_dev[32];

/**
 * Get device index in mlx5_dev[] from PCI bus address.
 *
 * @param[in] pci_addr
 *   PCI bus address to look for.
 *
 * @return
 *   mlx5_dev[] index on success, -1 on failure.
 */
static int
mlx5_dev_idx(struct rte_pci_addr *pci_addr)
{
	unsigned int i;
	int ret = -1;

	assert(pci_addr != NULL);
	for (i = 0; (i != RTE_DIM(mlx5_dev)); ++i) {
		if ((mlx5_dev[i].pci_addr.domain == pci_addr->domain) &&
		    (mlx5_dev[i].pci_addr.bus == pci_addr->bus) &&
		    (mlx5_dev[i].pci_addr.devid == pci_addr->devid) &&
		    (mlx5_dev[i].pci_addr.function == pci_addr->function))
			return i;
		if ((mlx5_dev[i].ports == 0) && (ret == -1))
			ret = i;
	}
	return ret;
}

/**
 * Verify and store value for device argument.
 *
 * @param[in] key
 *   Key argument to verify.
 * @param[in] val
 *   Value associated with key.
 * @param opaque
 *   User data.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_args_check(const char *key, const char *val, void *opaque)
{
	struct mlx5_args *args = opaque;
	unsigned long tmp;

	errno = 0;
	tmp = strtoul(val, NULL, 0);
	if (errno) {
		WARN("%s: \"%s\" is not a valid integer", key, val);
		return errno;
	}
	if (strcmp(MLX5_RXQ_CQE_COMP_EN, key) == 0) {
		args->cqe_comp = !!tmp;
	} else if (strcmp(MLX5_TXQ_INLINE, key) == 0) {
		args->txq_inline = tmp;
	} else if (strcmp(MLX5_TXQS_MIN_INLINE, key) == 0) {
		args->txqs_inline = tmp;
	} else if (strcmp(MLX5_TXQ_MPW_EN, key) == 0) {
		args->mps = !!tmp;
	} else if (strcmp(MLX5_TXQ_MPW_HDR_DSEG_EN, key) == 0) {
		args->mpw_hdr_dseg = !!tmp;
	} else if (strcmp(MLX5_TXQ_MAX_INLINE_LEN, key) == 0) {
		args->inline_max_packet_sz = tmp;
	} else if (strcmp(MLX5_TSO, key) == 0) {
		args->tso = !!tmp;
	} else if (strcmp(MLX5_TX_VEC_EN, key) == 0) {
		args->tx_vec_en = !!tmp;
	} else if (strcmp(MLX5_RX_VEC_EN, key) == 0) {
		args->rx_vec_en = !!tmp;
	} else {
		WARN("%s: unknown parameter", key);
		return -EINVAL;
	}
	return 0;
}

/**
 * Parse device parameters.
 *
 * @param priv
 *   Pointer to private structure.
 * @param devargs
 *   Device arguments structure.
 *
 * @return
 *   0 on success, errno value on failure.
 */
static int
mlx5_args(struct mlx5_args *args, struct rte_devargs *devargs)
{
	const char **params = (const char *[]){
		MLX5_RXQ_CQE_COMP_EN,
		MLX5_TXQ_INLINE,
		MLX5_TXQS_MIN_INLINE,
		MLX5_TXQ_MPW_EN,
		MLX5_TXQ_MPW_HDR_DSEG_EN,
		MLX5_TXQ_MAX_INLINE_LEN,
		MLX5_TSO,
		MLX5_TX_VEC_EN,
		MLX5_RX_VEC_EN,
		NULL,
	};
	struct rte_kvargs *kvlist;
	int ret = 0;
	int i;

	if (devargs == NULL)
		return 0;
	/* Following UGLY cast is done to pass checkpatch. */
	kvlist = rte_kvargs_parse(devargs->args, params);
	if (kvlist == NULL)
		return 0;
	/* Process parameters. */
	for (i = 0; (params[i] != NULL); ++i) {
		if (rte_kvargs_count(kvlist, params[i])) {
			ret = rte_kvargs_process(kvlist, params[i],
						 mlx5_args_check, args);
			if (ret != 0) {
				rte_kvargs_free(kvlist);
				return ret;
			}
		}
	}
	rte_kvargs_free(kvlist);
	return 0;
}

static struct rte_pci_driver mlx5_driver;

/**
 * Assign parameters from args into priv, only non default
 * values are considered.
 *
 * @param[out] priv
 *   Pointer to private structure.
 * @param[in] args
 *   Pointer to args values.
 */
static void
mlx5_args_assign(struct priv *priv, struct mlx5_args *args)
{
	if (args->cqe_comp != MLX5_ARG_UNSET)
		priv->cqe_comp = args->cqe_comp;
	if (args->txq_inline != MLX5_ARG_UNSET)
		priv->txq_inline = args->txq_inline;
	if (args->txqs_inline != MLX5_ARG_UNSET)
		priv->txqs_inline = args->txqs_inline;
	if (args->mps != MLX5_ARG_UNSET)
		priv->mps = args->mps ? priv->mps : 0;
	if (args->mpw_hdr_dseg != MLX5_ARG_UNSET)
		priv->mpw_hdr_dseg = args->mpw_hdr_dseg;
	if (args->inline_max_packet_sz != MLX5_ARG_UNSET)
		priv->inline_max_packet_sz = args->inline_max_packet_sz;
	if (args->tso != MLX5_ARG_UNSET)
		priv->tso = args->tso;
	if (args->tx_vec_en != MLX5_ARG_UNSET)
		priv->tx_vec_en = args->tx_vec_en;
	if (args->rx_vec_en != MLX5_ARG_UNSET)
		priv->rx_vec_en = args->rx_vec_en;
}

/**
 * DPDK callback to register a PCI device.
 *
 * This function creates an Ethernet device for each port of a given
 * PCI device.
 *
 * @param[in] pci_drv
 *   PCI driver structure (mlx5_driver).
 * @param[in] pci_dev
 *   PCI device information.
 *
 * @return
 *   0 on success, negative errno value on failure.
 */
static int
mlx5_pci_probe(struct rte_pci_driver *pci_drv, struct rte_pci_device *pci_dev)
{
	struct ibv_device **list;
	struct ibv_device *ibv_dev;
	int err = 0;
	struct ibv_context *attr_ctx = NULL;
	struct ibv_device_attr device_attr;
	unsigned int sriov;
	unsigned int mps;
	unsigned int tunnel_en = 0;
	int idx;
	int i;

	(void)pci_drv;
	assert(pci_drv == &mlx5_driver);
	/* Get mlx5_dev[] index. */
	idx = mlx5_dev_idx(&pci_dev->addr);
	if (idx == -1) {
		ERROR("this driver cannot support any more adapters");
		return -ENOMEM;
	}
	DEBUG("using driver device index %d", idx);

	/* Save PCI address. */
	mlx5_dev[idx].pci_addr = pci_dev->addr;
	list = ibv_get_device_list(&i);
	if (list == NULL) {
		assert(errno);
		if (errno == ENOSYS)
			ERROR("cannot list devices, is ib_uverbs loaded?");
		return -errno;
	}
	assert(i >= 0);
	/*
	 * For each listed device, check related sysfs entry against
	 * the provided PCI ID.
	 */
	while (i != 0) {
		struct rte_pci_addr pci_addr;

		--i;
		DEBUG("checking device \"%s\"", list[i]->name);
		if (mlx5_ibv_device_to_pci_addr(list[i], &pci_addr))
			continue;
		if ((pci_dev->addr.domain != pci_addr.domain) ||
		    (pci_dev->addr.bus != pci_addr.bus) ||
		    (pci_dev->addr.devid != pci_addr.devid) ||
		    (pci_dev->addr.function != pci_addr.function))
			continue;
		sriov = ((pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX4VF) ||
		      (pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX4LXVF) ||
		      (pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX5VF) ||
		      (pci_dev->id.device_id ==
		       PCI_DEVICE_ID_MELLANOX_CONNECTX5EXVF));
		/*
		 * Multi-packet send is supported by ConnectX-4 Lx PF as well
		 * as all ConnectX-5 devices.
		 */
		switch (pci_dev->id.device_id) {
		case PCI_DEVICE_ID_MELLANOX_CONNECTX4:
			tunnel_en = 1;
			mps = MLX5_MPW_DISABLED;
			break;
		case PCI_DEVICE_ID_MELLANOX_CONNECTX4LX:
			tunnel_en = 1;
			mps = MLX5_MPW;
			break;
		case PCI_DEVICE_ID_MELLANOX_CONNECTX5:
		case PCI_DEVICE_ID_MELLANOX_CONNECTX5VF:
		case PCI_DEVICE_ID_MELLANOX_CONNECTX5EX:
		case PCI_DEVICE_ID_MELLANOX_CONNECTX5EXVF:
			tunnel_en = 1;
			mps = MLX5_MPW_ENHANCED;
			break;
		default:
			mps = MLX5_MPW_DISABLED;
		}
		INFO("PCI information matches, using device \"%s\""
		     " (SR-IOV: %s, %sMPS: %s)",
		     list[i]->name,
		     sriov ? "true" : "false",
		     mps == MLX5_MPW_ENHANCED ? "Enhanced " : "",
		     mps != MLX5_MPW_DISABLED ? "true" : "false");
		attr_ctx = ibv_open_device(list[i]);
		err = errno;
		break;
	}
	if (attr_ctx == NULL) {
		ibv_free_device_list(list);
		switch (err) {
		case 0:
			ERROR("cannot access device, is mlx5_ib loaded?");
			return -ENODEV;
		case EINVAL:
			ERROR("cannot use device, are drivers up to date?");
			return -EINVAL;
		}
		assert(err > 0);
		return -err;
	}
	ibv_dev = list[i];

	DEBUG("device opened");
	if (ibv_query_device(attr_ctx, &device_attr))
		goto error;
	INFO("%u port(s) detected", device_attr.phys_port_cnt);

	for (i = 0; i < device_attr.phys_port_cnt; i++) {
		uint32_t port = i + 1; /* ports are indexed from one */
		uint32_t test = (1 << i);
		struct ibv_context *ctx = NULL;
		struct ibv_port_attr port_attr;
		struct ibv_pd *pd = NULL;
		struct priv *priv = NULL;
		struct rte_eth_dev *eth_dev;
		struct ibv_exp_device_attr exp_device_attr;
		struct ether_addr mac;
		uint16_t num_vfs = 0;
		struct mlx5_args args = {
			.cqe_comp = MLX5_ARG_UNSET,
			.txq_inline = MLX5_ARG_UNSET,
			.txqs_inline = MLX5_ARG_UNSET,
			.mps = MLX5_ARG_UNSET,
			.mpw_hdr_dseg = MLX5_ARG_UNSET,
			.inline_max_packet_sz = MLX5_ARG_UNSET,
			.tso = MLX5_ARG_UNSET,
			.tx_vec_en = MLX5_ARG_UNSET,
			.rx_vec_en = MLX5_ARG_UNSET,
		};

		exp_device_attr.comp_mask =
			IBV_EXP_DEVICE_ATTR_EXP_CAP_FLAGS |
			IBV_EXP_DEVICE_ATTR_RX_HASH |
			IBV_EXP_DEVICE_ATTR_VLAN_OFFLOADS |
			IBV_EXP_DEVICE_ATTR_RX_PAD_END_ALIGN |
			IBV_EXP_DEVICE_ATTR_TSO_CAPS |
			0;

		DEBUG("using port %u (%08" PRIx32 ")", port, test);

		ctx = ibv_open_device(ibv_dev);
		if (ctx == NULL) {
			err = ENODEV;
			goto port_error;
		}

		/* Check port status. */
		err = ibv_query_port(ctx, port, &port_attr);
		if (err) {
			ERROR("port query failed: %s", strerror(err));
			goto port_error;
		}

		if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
			ERROR("port %d is not configured in Ethernet mode",
			      port);
			err = EINVAL;
			goto port_error;
		}

		if (port_attr.state != IBV_PORT_ACTIVE)
			DEBUG("port %d is not active: \"%s\" (%d)",
			      port, ibv_port_state_str(port_attr.state),
			      port_attr.state);

		/* Allocate protection domain. */
		pd = ibv_alloc_pd(ctx);
		if (pd == NULL) {
			ERROR("PD allocation failure");
			err = ENOMEM;
			goto port_error;
		}

		mlx5_dev[idx].ports |= test;

		/* from rte_ethdev.c */
		priv = rte_zmalloc("ethdev private structure",
				   sizeof(*priv),
				   RTE_CACHE_LINE_SIZE);
		if (priv == NULL) {
			ERROR("priv allocation failure");
			err = ENOMEM;
			goto port_error;
		}

		priv->ctx = ctx;
		priv->device_attr = device_attr;
		priv->port = port;
		priv->pd = pd;
		priv->mtu = ETHER_MTU;
		priv->mps = mps; /* Enable MPW by default if supported. */
		priv->cqe_comp = 1; /* Enable compression by default. */
		priv->tunnel_en = tunnel_en;
		/* Enable vector by default if supported. */
		priv->tx_vec_en = 1;
		priv->rx_vec_en = 1;
		err = mlx5_args(&args, pci_dev->device.devargs);
		if (err) {
			ERROR("failed to process device arguments: %s",
			      strerror(err));
			goto port_error;
		}
		mlx5_args_assign(priv, &args);
		if (ibv_exp_query_device(ctx, &exp_device_attr)) {
			ERROR("ibv_exp_query_device() failed");
			err = ENODEV;
			goto port_error;
		}

		priv->hw_csum =
			((exp_device_attr.exp_device_cap_flags &
			  IBV_EXP_DEVICE_RX_CSUM_TCP_UDP_PKT) &&
			 (exp_device_attr.exp_device_cap_flags &
			  IBV_EXP_DEVICE_RX_CSUM_IP_PKT));
		DEBUG("checksum offloading is %ssupported",
		      (priv->hw_csum ? "" : "not "));

		priv->hw_csum_l2tun = !!(exp_device_attr.exp_device_cap_flags &
					 IBV_EXP_DEVICE_VXLAN_SUPPORT);
		DEBUG("L2 tunnel checksum offloads are %ssupported",
		      (priv->hw_csum_l2tun ? "" : "not "));

		priv->ind_table_max_size = exp_device_attr.rx_hash_caps.max_rwq_indirection_table_size;
		/* Remove this check once DPDK supports larger/variable
		 * indirection tables. */
		if (priv->ind_table_max_size >
				(unsigned int)ETH_RSS_RETA_SIZE_512)
			priv->ind_table_max_size = ETH_RSS_RETA_SIZE_512;
		DEBUG("maximum RX indirection table size is %u",
		      priv->ind_table_max_size);
		priv->hw_vlan_strip = !!(exp_device_attr.wq_vlan_offloads_cap &
					 IBV_EXP_RECEIVE_WQ_CVLAN_STRIP);
		DEBUG("VLAN stripping is %ssupported",
		      (priv->hw_vlan_strip ? "" : "not "));

		priv->hw_fcs_strip = !!(exp_device_attr.exp_device_cap_flags &
					IBV_EXP_DEVICE_SCATTER_FCS);
		DEBUG("FCS stripping configuration is %ssupported",
		      (priv->hw_fcs_strip ? "" : "not "));

		priv->hw_padding = !!exp_device_attr.rx_pad_end_addr_align;
		DEBUG("hardware RX end alignment padding is %ssupported",
		      (priv->hw_padding ? "" : "not "));

		priv_get_num_vfs(priv, &num_vfs);
		priv->sriov = (num_vfs || sriov);
		priv->tso = ((priv->tso) &&
			    (exp_device_attr.tso_caps.max_tso > 0) &&
			    (exp_device_attr.tso_caps.supported_qpts &
			    (1 << IBV_QPT_RAW_ETH)));
		if (priv->tso)
			priv->max_tso_payload_sz =
				exp_device_attr.tso_caps.max_tso;
		if (priv->mps && !mps) {
			ERROR("multi-packet send not supported on this device"
			      " (" MLX5_TXQ_MPW_EN ")");
			err = ENOTSUP;
			goto port_error;
		} else if (priv->mps && priv->tso) {
			WARN("multi-packet send not supported in conjunction "
			      "with TSO. MPS disabled");
			priv->mps = 0;
		}
		INFO("%sMPS is %s",
		     priv->mps == MLX5_MPW_ENHANCED ? "Enhanced " : "",
		     priv->mps != MLX5_MPW_DISABLED ? "enabled" : "disabled");
		/* Set default values for Enhanced MPW, a.k.a MPWv2. */
		if (priv->mps == MLX5_MPW_ENHANCED) {
			if (args.txqs_inline == MLX5_ARG_UNSET)
				priv->txqs_inline = MLX5_EMPW_MIN_TXQS;
			if (args.inline_max_packet_sz == MLX5_ARG_UNSET)
				priv->inline_max_packet_sz =
					MLX5_EMPW_MAX_INLINE_LEN;
			if (args.txq_inline == MLX5_ARG_UNSET)
				priv->txq_inline = MLX5_WQE_SIZE_MAX -
						   MLX5_WQE_SIZE;
		}
		/* Allocate and register default RSS hash keys. */
		priv->rss_conf = rte_calloc(__func__, hash_rxq_init_n,
					    sizeof((*priv->rss_conf)[0]), 0);
		if (priv->rss_conf == NULL) {
			err = ENOMEM;
			goto port_error;
		}
		err = rss_hash_rss_conf_new_key(priv,
						rss_hash_default_key,
						rss_hash_default_key_len,
						ETH_RSS_PROTO_MASK);
		if (err)
			goto port_error;
		/* Configure the first MAC address by default. */
		if (priv_get_mac(priv, &mac.addr_bytes)) {
			ERROR("cannot get MAC address, is mlx5_en loaded?"
			      " (errno: %s)", strerror(errno));
			err = ENODEV;
			goto port_error;
		}
		INFO("port %u MAC address is %02x:%02x:%02x:%02x:%02x:%02x",
		     priv->port,
		     mac.addr_bytes[0], mac.addr_bytes[1],
		     mac.addr_bytes[2], mac.addr_bytes[3],
		     mac.addr_bytes[4], mac.addr_bytes[5]);
		/* Register MAC address. */
		claim_zero(priv_mac_addr_add(priv, 0,
					     (const uint8_t (*)[ETHER_ADDR_LEN])
					     mac.addr_bytes));
		/* Initialize FD filters list. */
		err = fdir_init_filters_list(priv);
		if (err)
			goto port_error;
#ifndef NDEBUG
		{
			char ifname[IF_NAMESIZE];

			if (priv_get_ifname(priv, &ifname) == 0)
				DEBUG("port %u ifname is \"%s\"",
				      priv->port, ifname);
			else
				DEBUG("port %u ifname is unknown", priv->port);
		}
#endif
		/* Get actual MTU if possible. */
		priv_get_mtu(priv, &priv->mtu);
		DEBUG("port %u MTU is %u", priv->port, priv->mtu);

		/* from rte_ethdev.c */
		{
			char name[RTE_ETH_NAME_MAX_LEN];

			snprintf(name, sizeof(name), "%s port %u",
				 ibv_get_device_name(ibv_dev), port);
			eth_dev = rte_eth_dev_allocate(name);
		}
		if (eth_dev == NULL) {
			ERROR("can not allocate rte ethdev");
			err = ENOMEM;
			goto port_error;
		}
		eth_dev->data->dev_private = priv;
		eth_dev->data->mac_addrs = priv->mac;
		eth_dev->device = &pci_dev->device;
		rte_eth_copy_pci_info(eth_dev, pci_dev);
		eth_dev->data->dev_flags |= RTE_ETH_DEV_DETACHABLE;
		eth_dev->device->driver = &mlx5_driver.driver;
		priv->dev = eth_dev;
		eth_dev->dev_ops = &mlx5_dev_ops;
		TAILQ_INIT(&priv->flows);

		/* Bring Ethernet device up. */
		DEBUG("forcing Ethernet interface up");
		priv_set_flags(priv, ~IFF_UP, IFF_UP);
		mlx5_link_update(priv->dev, 1);
		continue;

port_error:
		if (priv) {
			rte_free(priv->rss_conf);
			rte_free(priv);
		}
		if (pd)
			claim_zero(ibv_dealloc_pd(pd));
		if (ctx)
			claim_zero(ibv_close_device(ctx));
		break;
	}

	/*
	 * XXX if something went wrong in the loop above, there is a resource
	 * leak (ctx, pd, priv, dpdk ethdev) but we can do nothing about it as
	 * long as the dpdk does not provide a way to deallocate a ethdev and a
	 * way to enumerate the registered ethdevs to free the previous ones.
	 */

	/* no port found, complain */
	if (!mlx5_dev[idx].ports) {
		err = ENODEV;
		goto error;
	}

error:
	if (attr_ctx)
		claim_zero(ibv_close_device(attr_ctx));
	if (list)
		ibv_free_device_list(list);
	assert(err >= 0);
	return -err;
}

static const struct rte_pci_id mlx5_pci_id_map[] = {
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX4)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX4VF)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX4LX)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX4LXVF)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX5)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX5VF)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX5EX)
	},
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MELLANOX,
			       PCI_DEVICE_ID_MELLANOX_CONNECTX5EXVF)
	},
	{
		.vendor_id = 0
	}
};

static struct rte_pci_driver mlx5_driver = {
	.driver = {
		.name = MLX5_DRIVER_NAME
	},
	.id_table = mlx5_pci_id_map,
	.probe = mlx5_pci_probe,
	.drv_flags = RTE_PCI_DRV_INTR_LSC,
};

/**
 * Driver initialization routine.
 */
RTE_INIT(rte_mlx5_pmd_init);
static void
rte_mlx5_pmd_init(void)
{
	/* Build the static table for ptype conversion. */
	mlx5_set_ptype_table();
	/*
	 * RDMAV_HUGEPAGES_SAFE tells ibv_fork_init() we intend to use
	 * huge pages. Calling ibv_fork_init() during init allows
	 * applications to use fork() safely for purposes other than
	 * using this PMD, which is not supported in forked processes.
	 */
	setenv("RDMAV_HUGEPAGES_SAFE", "1", 1);
	/* Don't map UAR to WC if BlueFlame is not used.*/
	setenv("MLX5_SHUT_UP_BF", "1", 1);
	ibv_fork_init();
	rte_pci_register(&mlx5_driver);
}

RTE_PMD_EXPORT_NAME(net_mlx5, __COUNTER__);
RTE_PMD_REGISTER_PCI_TABLE(net_mlx5, mlx5_pci_id_map);
RTE_PMD_REGISTER_KMOD_DEP(net_mlx5, "* ib_uverbs & mlx5_core & mlx5_ib");
