// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, ProvenRun S.A.S
 */
/**
 * @file    internal.h
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run S.A.S and/or its affiliates.
 *   All rights reserved.
 */

#ifndef _SHDEV_INTERNAL_H_INCLUDED_
#define _SHDEV_INTERNAL_H_INCLUDED_

/* Prefix all driver output with the same string */
#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "pncshdev: " fmt

#include "shdev.h"

/**
 * Operations that can be implemented for each shared device handler.
 */
typedef struct shdev_ops {
    int (*suspend)(void);

    int (*resume)(void);

    int (*select)(shdev_desc_t *desc_ptr);

    int (*read)(shdev_desc_t *desc_ptr);

    int (*write)(shdev_desc_t *desc_ptr);

#ifdef CONFIG_PROVENCORE_MMC_USE_RPMB
    int (*rpmb)(shdev_desc_t *desc_ptr);
#endif /* CONFIG_PROVENCORE_MMC_USE_RPMB */
} shdev_ops_t;

/* Shared MMC public functions */
shdev_ops_t *mmcblk_init(void);

/* Shared ENET public functions */
shdev_ops_t *enetdev_init(void);

/* Shared SPI public functions */
shdev_ops_t *spidev_init(void);

#endif /* _SHDEV_INTERNAL_H_INCLUDED_ */
