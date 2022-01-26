// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, ProvenRun S.A.S
 */
/**
 * @file    dev_enet.c
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run and/or its affiliates.
 *   All rights reserved.
 */

#include <linux/blkdev.h>
#include <linux/err.h>

#include "internal.h"

#ifdef CONFIG_PROVENCORE_SHARED_ENET

/* There's no default value for CONFIG_PROVENCORE_ENET_DEVICE */
#ifndef CONFIG_PROVENCORE_ENET_DEVICE
#error "ENET_DEVICE not in config: run menuconfig or update defconfig file"
#endif

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/netdevice.h>

static struct device *_shared_enet_device = NULL;

/**
 * @brief Lookup the ethernet device with node type
 *  \ref CONFIG_PROVENCORE_ENET_DEVICE.
 * @param info          Shared device handle
 * @return              - NULL if the device \ref CONFIG_PROVENCORE_ENET_DEVICE
 *                        is not found
 *                      - pointer to the device struct otherwise
 */
static struct device *enetdev_get(void)
{
    struct device_node *node, *from = NULL;
    struct platform_device *pdev;

    if (_shared_enet_device != NULL) {
        return _shared_enet_device;
    }

    node = of_find_compatible_node(from, NULL, CONFIG_PROVENCORE_ENET_DEVICE);
    if (node == NULL) {
        pr_err("(%s) no compatible node for \""
                CONFIG_PROVENCORE_ENET_DEVICE "\"\n", __func__);
        return NULL;
    }

    pdev = of_find_device_by_node(node);
    if (pdev == NULL) {
        pr_err("(%s) enet platform device not found\n", __func__);
        return NULL;
    }

    _shared_enet_device = &pdev->dev;
    return &pdev->dev;
}

static int enetdev_suspend(void)
{
    struct net_device *ndev;
    struct device *dev = enetdev_get();
    if (dev == NULL) {
        return -ENODEV;
    }
    ndev = dev_get_drvdata(dev);
    pr_debug("%s)\n", __func__);
    return ndev->netdev_ops->ndo_do_ioctl(ndev, NULL, NETDEV_LOCK);
}

static int enetdev_resume(void)
{
    struct net_device *ndev;
    struct device *dev = enetdev_get();
    if (dev == NULL) {
        return -ENODEV;
    }
    ndev = dev_get_drvdata(dev);
    pr_debug("(%s)\n", __func__);
    return ndev->netdev_ops->ndo_do_ioctl(ndev, NULL, NETDEV_UNLOCK);
}

static shdev_ops_t enetdev_ops = {
    .suspend = enetdev_suspend,
    .resume  = enetdev_resume
};

shdev_ops_t *enetdev_init(void)
{
    return &enetdev_ops;
}

#endif /* CONFIG_PROVENCORE_SHARED_ENET */
