// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, ProvenRun S.A.S
 */
/**
 * @file    dev_spi.c
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run and/or its affiliates.
 *   All rights reserved.
 */

#include <linux/blkdev.h>
#include <linux/err.h>

#include "internal.h"

#ifdef CONFIG_PROVENCORE_SHARED_SPI

#ifndef CONFIG_PROVENCORE_SPI_DEVICE
#error "SPI_DEVICE not in config: run menuconfig or update defconfig file"
#endif

#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/spi/spi.h>

static struct device *_shared_spi_device = NULL;

/**
 * @brief Lookup the ethernet device with node type
 *  \ref CONFIG_PROVENCORE_SPI_DEVICE.
 * @param info          Shared device handle
 * @return              - NULL if the device \ref CONFIG_PROVENCORE_SPI_DEVICE
 *                        is not found
 *                      - pointer to the device struct otherwise
 */

static struct device *spidev_get(void)
{
    struct device_node *node, *from = NULL;
    struct device *dev;

    if (_shared_spi_device != NULL) {
        return _shared_spi_device;
    }

    node = of_find_compatible_node(from, NULL, CONFIG_PROVENCORE_SPI_DEVICE);
    if (node == NULL) {
        pr_err("(%s) no compatible node for \""
                CONFIG_PROVENCORE_SPI_DEVICE "\"\n", __func__);
        return NULL;
    }

    dev = bus_find_device(&spi_bus_type, NULL, node, of_dev_node_match);
    if (dev == NULL) {
        pr_err("(%s) spi platform device not found\n", __func__);
        return NULL;
    }

    _shared_spi_device = dev;
    return dev;
}

static int spidev_suspend(void)
{
    struct spi_device *ndev;
    struct device *dev = spidev_get();
    if (dev == NULL) {
        return -ENODEV;
    }
    ndev = to_spi_device(dev);
    pr_debug("(%s)\n", __func__);
    return spi_master_suspend(ndev->master);
}

static int spidev_resume(void)
{
    struct spi_device *ndev;
    struct device *dev = spidev_get();
    if (dev == NULL) {
        return -ENODEV;
    }
    ndev = to_spi_device(dev);
    pr_debug("(%s)\n", __func__);
    return spi_master_resume(ndev->master);;
}

static shdev_ops_t spidev_ops = {
    .suspend = spidev_suspend,
    .resume  = spidev_resume
};

shdev_ops_t *spidev_init(void)
{
    return &spidev_ops;
}

#endif /* CONFIG_PROVENCORE_SHARED_SPI */
