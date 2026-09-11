/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0 */
#ifndef RAIL_RDMA_LINK_RATE_H
#define RAIL_RDMA_LINK_RATE_H

/* Verbs speed and width are encodings, not multipliers. */
static inline unsigned int rail_rdma_rate_mbps(unsigned int speed, unsigned int width)
{
	unsigned int lane_mbps;
	unsigned int lanes;

	switch (speed) {
	case 1: /* SDR */
		lane_mbps = 2500;
		break;
	case 2: /* DDR */
		lane_mbps = 5000;
		break;
	case 4: /* QDR */
		lane_mbps = 10000;
		break;
	case 8: /* FDR10 */
		lane_mbps = 10000;
		break;
	case 16: /* FDR */
		lane_mbps = 14000;
		break;
	case 32: /* EDR */
		lane_mbps = 25000;
		break;
	case 64: /* HDR */
		lane_mbps = 50000;
		break;
	case 128: /* NDR */
		lane_mbps = 100000;
		break;
	case 256: /* XDR */
		lane_mbps = 200000;
		break;
	default:
		return 0;
	}

	switch (width) {
	case 1:
		lanes = 1;
		break;
	case 2:
		lanes = 4;
		break;
	case 4:
		lanes = 8;
		break;
	case 8:
		lanes = 12;
		break;
	case 16:
		lanes = 2;
		break;
	default:
		return 0;
	}

	return lane_mbps * lanes;
}

#endif
