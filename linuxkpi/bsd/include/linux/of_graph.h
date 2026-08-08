/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _LINUXKPI_LINUX_OF_GRAPH_H_
#define _LINUXKPI_LINUX_OF_GRAPH_H_

#include <linux/of.h>

static inline int of_graph_get_port_count(const struct device_node *np)
{
	/* TODO: implement proper port counting from device tree */
	return 1;
}

#endif
