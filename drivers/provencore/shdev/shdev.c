/*
 * Copyright (c) 2020-2021 Prove & Run S.A.S
 * All Rights Reserved.
 *
 * This software is the confidential and proprietary information of
 * Prove & Run S.A.S ("Confidential Information"). You shall not
 * disclose such Confidential Information and shall use it only in
 * accordance with the terms of the license agreement you entered
 * into with Prove & Run S.A.S
 *
 * PROVE & RUN S.A.S MAKES NO REPRESENTATIONS OR WARRANTIES ABOUT THE
 * SUITABILITY OF THE SOFTWARE, EITHER EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT. PROVE & RUN S.A.S SHALL
 * NOT BE LIABLE FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING,
 * MODIFYING OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.
 */
/**
 * @file    shdev.c
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run and/or its affiliates.
 *   All rights reserved.
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/delay.h>        // msleep()
#include <linux/kthread.h>

/* Shared device(s) monitor is a ree session user...*/
#include "misc/provencore/ree_session.h"

#include "internal.h"

/* Internal device descriptor */
typedef struct shdev {
    /* Scheduled work */
    struct work_struct work;
    /* Device identifier */
    uint32_t id;
    /* Message signal id */
    uint32_t signal_msg;
    /* Device operations */
    shdev_ops_t *ops;
} shdev_t;

/** Start addr for shdev session's SHM */
char *_shdev_shm_addr = NULL;

/** Dedicated session for shared devices monitor */
static pnc_session_t *_shdev_session = NULL;

/** shdev infos shared between S and NS at start-up and during operations */
static shdev_infos_t *_shdev_infos_ptr = NULL;

/** Table of internal device descriptors */
static shdev_t _shdev_devices[NUM_DEVICES];

static void device_work_func(struct work_struct *work)
{
    int ret = 0;
    uint32_t index;
    shdev_t *dev_ptr;
    shdev_desc_t desc;
    shdev_message_t *mptr;

    /* Get internal device descriptor */
    dev_ptr = container_of(work, shdev_t, work);
    index = SHDEV_ID_TO_DEVICE(dev_ptr->id);
    if (index >= NUM_DEVICES) {
        pr_err("invalid work !\n");
        return;
    }

    /* Copy shared device descriptor */
    memcpy(&desc, &_shdev_infos_ptr->descriptors[index], sizeof(shdev_desc_t));
    if (desc.id != dev_ptr->id) {
        pr_err("invalid device descriptor (%u/%u) !\n", desc.id, dev_ptr->id);
        return;
    }

    switch(desc.s_to_ns.operation) {
        case SUSPEND_DEVICE:
            if (dev_ptr->ops->suspend) {
                pr_debug("suspend %u\n", index);
                ret = dev_ptr->ops->suspend();
            }
            break;
        case RESUME_DEVICE:
            if (dev_ptr->ops->resume) {
                pr_debug("resume %u\n", index);
                ret = dev_ptr->ops->resume();
            }
            break;
        case SELECT_DEVICE:
            if (dev_ptr->ops->select) {
                pr_debug("select %u\n", index);
                ret = dev_ptr->ops->select(&desc);
            }
            break;
        case READ_DEVICE:
            if (dev_ptr->ops->read) {
                pr_debug("read %u\n", index);
                ret = dev_ptr->ops->read(&desc);
            }
            break;
        case WRITE_DEVICE:
            if (dev_ptr->ops->write) {
                pr_debug("write %u\n", index);
                ret = dev_ptr->ops->write(&desc);
            }
            break;
#ifdef CONFIG_PROVENCORE_MMC_USE_RPMB
        case RPMB_DEVICE:
            if (dev_ptr->ops->rpmb) {
                pr_debug("rpmb %u\n", index);
                ret = dev_ptr->ops->rpmb(&desc);
            }
            break;
#endif /* CONFIG_PROVENCORE_MMC_USE_RPMB */
        default:
            pr_err("%s: unhandled operation %u\n", __func__,
                desc.s_to_ns.operation);
            ret = -ENOSYS;
            break;
    }

    /* Signal S about new operation status: copy NS->S in S->NS msg slot and
     * update it with operation status. */
    mptr = &_shdev_infos_ptr->descriptors[index].ns_to_s;
    memcpy(mptr, &desc.s_to_ns, sizeof(shdev_message_t));
    mptr->status = 1;
    mptr->value = (ret < 0) ? -ret:ret;
    ret = pnc_session_send_signal(_shdev_session, dev_ptr->signal_msg);
    if (ret != 0) {
        /* Leaving thread... */
        pr_err("Shared devices monitor synchro failure (%d).\n", ret);
        // TODO: handle this use case ?
    }

    return;
}

/**
 * Handle reception of S-->NS signal
 *
 * If signal doesn't indicate new SIGNAL_xxx_MESSAGE, it is ignored.
 * Upon new SIGNAL_xxx_MESSAGE reception, schedule corresponding device handler.
 */
static void handle_signal(void)
{
    int ret;
    uint32_t signals;

    ret = pnc_session_get_signal(_shdev_session, &signals);
    if (ret != 0) {
        pr_err("failed to get received signals (%d)\n", ret);
        return;
    }

#ifdef CONFIG_PROVENCORE_SHARED_MMC
    if (signals & SHDEV_SIGNAL(SIGNAL_MMC_MESSAGE)) {
        /* Schedule MMC work */
        if (!schedule_work(&_shdev_devices[MMC_DEVICE].work)) {
            pr_warn("MMC job is already running\n");
        }
    }
#endif

#ifdef CONFIG_PROVENCORE_SHARED_ENET
    if (signals & SHDEV_SIGNAL(SIGNAL_ENET_MESSAGE)) {
        /* Schedule ENET work */
        if (!schedule_work(&_shdev_devices[ENET_DEVICE].work)) {
            pr_warn("ENET job is already running\n");
        }
    }
#endif

#ifdef CONFIG_PROVENCORE_SHARED_SPI
    if (signals & SHDEV_SIGNAL(SIGNAL_SPI_MESSAGE)) {
        /* Schedule SPI work */
        if (!schedule_work(&_shdev_devices[SPI_DEVICE].work)) {
            pr_warn("SPI job is already running\n");
        }
    }
#endif

    return;
}

/**
 * Configure shared devices monitor:
 *  - open session
 *  - alloc SHM for this session
 *  - configure session for dedicated service usage
 *  - configure slot layout for each supported device
 *  - signal S for monitor readiness
 */
static int configure(void)
{
    int ret;
    unsigned long shm_size;
    uint32_t session_pages=SHDEV_PAGES, data_offset=0, infos_offset=0, version;
    shdev_desc_t *desc_ptr;

    pr_debug("opening shared devices monitor session\n");
    ret = pnc_session_open(&_shdev_session);
    if (ret != 0) {
        pr_err("open failure for shared devices monitor (%d)\n", ret);
        return ret;
    }

#ifdef CONFIG_PROVENCORE_MMC_REMOTE_HOST
    session_pages += SHDEV_MMC_PAGES;
#endif

    pr_debug("allocating shm for shared devices monitor session\n");
    ret = pnc_session_alloc(_shdev_session, session_pages*PAGE_SIZE);
    if (ret != 0) {
        pr_err("alloc failure for shared devices monitor (%d)\n", ret);
        goto config_err;
    }

    ret = pnc_session_get_version(_shdev_session, &version);
    if (ret != 0) {
        pr_err("fail to get REE version (%d)", ret);
        goto config_err;
    }
    if (version < 0x303) {
        pr_err("REE version 0x%x not supported, must be a least 0x303", version);
        goto config_err;
    }

    pr_debug("configuring shared devices monitor session\n");
session_config:
    ret = pnc_session_config_by_name(_shdev_session, "dev_monitor");
    while((ret == -EAGAIN) || (ret == -ENOENT)) {
        msleep(100);
        goto session_config;
    }
    if (ret != 0) {
        if (ret == -ETIMEDOUT) {
            pr_err("config failure because no S monitor ready.\n");
        } else {
            pr_err("config failure for shared devices monitor(%d).\n", ret);
        }
        goto config_err;
    }

    pr_debug("getting shared devices monitor session's shm\n");
    ret = pnc_session_get_mem(_shdev_session, &_shdev_shm_addr, &shm_size);
    if (ret != 0) {
        /* This should never occur unless very bad shape of the REE driver... */
        pr_err("get mem failure for shared devices monitor (%d)\n", ret);
        goto config_err;
    }
    pr_debug("shared devices monitor shm: %lu @ %p\n", shm_size,
        _shdev_shm_addr);

    /* Setup generic shdev _shdev_infos */
    _shdev_infos_ptr = (shdev_infos_t *)((void *)_shdev_shm_addr);
    _shdev_infos_ptr->magic = SHDEV_MAGIC_INFOS;
    _shdev_infos_ptr->num_devices = NUM_DEVICES;
    data_offset  = SHDEV_PAGES*PAGE_SIZE;
    infos_offset = sizeof(shdev_infos_t);

#ifdef CONFIG_PROVENCORE_SHARED_MMC
    /* Configure MMC descriptor: a way to confirm S about MMC support... */
    desc_ptr = &_shdev_infos_ptr->descriptors[MMC_DEVICE];
    desc_ptr->id = SHDEV_DEVICE_TO_ID(MMC_DEVICE);
#ifdef CONFIG_PROVENCORE_MMC_REMOTE_HOST
    /* Setup MMC remote host layout:
     *  - entry buffer used for additional device description
     *  - data buffer used for read/write ops. Starts after \p SHDEV_PAGES
     */
    desc_ptr->entry_offset = infos_offset;
    desc_ptr->entry_size   = sizeof(shdev_mmc_entry_t);
    infos_offset += desc_ptr->entry_size;
    desc_ptr->data_offset = data_offset;
    desc_ptr->data_size   = SHDEV_MMC_PAGES*PAGE_SIZE;
    data_offset += desc_ptr->data_size;
#endif /* CONFIG_PROVENCORE_MMC_REMOTE_HOST */
#endif /* CONFIG_PROVENCORE_SHARED_MMC */

#ifdef CONFIG_PROVENCORE_SHARED_ENET
    /* Configure ENET descriptor: a way to confirm S about ENET support... */
    desc_ptr = &_shdev_infos_ptr->descriptors[ENET_DEVICE];
    desc_ptr->id = SHDEV_DEVICE_TO_ID(ENET_DEVICE);
#endif

#ifdef CONFIG_PROVENCORE_SHARED_SPI
    /* Configure SPI descriptor: a way to confirm S about SPI support... */
    desc_ptr = &_shdev_infos_ptr->descriptors[SPI_DEVICE];
    desc_ptr->id = SHDEV_DEVICE_TO_ID(SPI_DEVICE);
#endif

    /* Check shdev infos and data fit in reserved area */
    if (infos_offset > SHDEV_PAGES*PAGE_SIZE) {
        pr_err("Invalid shared devices infos layout [1]: %u/%lu\n", infos_offset,
            SHDEV_PAGES*PAGE_SIZE);
        ret = -EFAULT;
        goto config_err;
    }
    if (data_offset > shm_size) {
        pr_err("Invalid shared devices infos layout [2]: %u/%lu\n", data_offset,
            shm_size);
        ret = -EFAULT;
        goto config_err;
    }

    /* Signal S about monitor readiness */
    pr_info("Signalling shared devices monitor readiness.\n");
    ret = pnc_session_send_signal(_shdev_session, SHDEV_SIGNAL(SIGNAL_READY));
    if (ret != 0) {
        pr_err("Shared devices monitor synchro failure (%d).\n", ret);
        goto config_err;
    }

    return 0;

config_err:
    pnc_session_close(_shdev_session);
    return ret;
}

/**
 * Shared devices monitor thread
 *
 * Responsible for configuring session with SID_DEVMON at startup and then to
 * keep waiting for S requests to suspend or resume any supported device.
 */
static int shdev_thread(void *arg)
{
    int ret;
    uint32_t events;
    (void)arg;

#if defined(CONFIG_PROVENCORE_MMC_DEVICE) || defined(CONFIG_PROVENCORE_ENET_DEVICE) || defined(CONFIG_PROVENCORE_SPI_DEVICE)
    pr_info("    monitored devices:\n");
#if defined(CONFIG_PROVENCORE_MMC_DEVICE)
    pr_info("      mmc: %s\n", CONFIG_PROVENCORE_MMC_DEVICE);
#endif
#if defined(CONFIG_PROVENCORE_ENET_DEVICE)
    pr_info("      enet: %s\n", CONFIG_PROVENCORE_ENET_DEVICE);
#endif
#if defined(CONFIG_PROVENCORE_SPI_DEVICE)
    pr_info("      spi: %s\n", CONFIG_PROVENCORE_SPI_DEVICE);
#endif
#else
    pr_info("    monitored devices: none !\n");
#endif

    /* Configure monitor */
monitor_restart:
    /* Check if thread should stop before starting creating session */
    if (kthread_should_stop())
        return 0;

    pr_info("Configuring shared devices monitor.\n");
    ret = configure();
    if (ret != 0) {
        /* Leaving thread... */
        pr_err("Shared devices monitor config failure (%d).\n", ret);
        return -1;
    }

    /* Monitoring loop... */
    pr_info("Starting shared devices monitoring.\n");
    do {
        /* Check if thread should stop before starting waiting for event */
        if (kthread_should_stop())
            break;

        /* Wait for S monitor event: only signal or request. Response shall be
         * retreived separately if sending a request...
         */
        ret = pnc_session_wait_event(_shdev_session, &events,
                EVENT_PENDING_SIGNAL|EVENT_PENDING_REQUEST, NO_TIMEOUT);
        if (ret != 0) {
            if (ret == -ENODEV) {
                /* Session not ready anymore... Terminate... */
                pr_err("%s: session not functional anymore...\n", __func__);
                pnc_session_close(_shdev_session);
                /* ...and get ready for new one... */
                goto monitor_restart;
            } else {
                if (ret == -EPIPE) {
                    pr_err("monitor session ended\n");
                    goto monitor_restart;
                }
                // TODO: even if this should never occur...
                pr_err("%s: error waiting event (%d)\n", __func__, ret);
                continue;
            }
        }

        /* Check received event(s) */
        if (events & EVENT_PENDING_REQUEST) {
            pr_err("%s: request reception not supported\n", __func__);
        }
        if (events & EVENT_PENDING_SIGNAL) {
            handle_signal();
        }
    } while (1);

    /* Should never get there */
    return 0;
}

static struct task_struct *shdev_task;

static int __init shdev_init(void)
{
    /* Init shared devices */
#ifdef CONFIG_PROVENCORE_SHARED_MMC
    _shdev_devices[MMC_DEVICE].id  = SHDEV_DEVICE_TO_ID(MMC_DEVICE);
    _shdev_devices[MMC_DEVICE].signal_msg = SHDEV_SIGNAL(SIGNAL_MMC_MESSAGE);
    _shdev_devices[MMC_DEVICE].ops = mmcblk_init();
    INIT_WORK(&_shdev_devices[MMC_DEVICE].work, device_work_func);
#endif
#ifdef CONFIG_PROVENCORE_SHARED_ENET
    _shdev_devices[ENET_DEVICE].id  = SHDEV_DEVICE_TO_ID(ENET_DEVICE);
    _shdev_devices[ENET_DEVICE].signal_msg = SHDEV_SIGNAL(SIGNAL_ENET_MESSAGE);
    _shdev_devices[ENET_DEVICE].ops = enetdev_init();
    INIT_WORK(&_shdev_devices[ENET_DEVICE].work, device_work_func);
#endif
#ifdef CONFIG_PROVENCORE_SHARED_SPI
    _shdev_devices[SPI_DEVICE].id  = SHDEV_DEVICE_TO_ID(SPI_DEVICE);
    _shdev_devices[SPI_DEVICE].signal_msg = SHDEV_SIGNAL(SIGNAL_SPI_MESSAGE);
    _shdev_devices[SPI_DEVICE].ops = spidev_init();
    INIT_WORK(&_shdev_devices[SPI_DEVICE].work, device_work_func);
#endif

    /* Create and start shared devices monitor kthread */
    shdev_task = kthread_create(&shdev_thread, NULL, "pnc_shdev");
    if (IS_ERR(shdev_task)) {
        return PTR_ERR(shdev_task);
    }
    kthread_bind(shdev_task, 0); /* bind to cpu#0 */
    wake_up_process(shdev_task);
    return 0;
}

static void __exit shdev_exit(void)
{
    /* Close session with secure shared devices monitor */
    if (_shdev_session) {
        pr_debug("Closing monitor session\n");
        pnc_session_close(_shdev_session);
    }

    /* Stop main thread */
    pr_debug("Stopping monitor process\n");
    kthread_stop(shdev_task);

    /* Wait end of any shared device pending work */
#ifdef CONFIG_PROVENCORE_SHARED_MMC
    flush_work(&_shdev_devices[MMC_DEVICE].work);
#endif
#ifdef CONFIG_PROVENCORE_SHARED_ENET
    flush_work(&_shdev_devices[ENET_DEVICE].work);
#endif
#ifdef CONFIG_PROVENCORE_SHARED_SPI
    flush_work(&_shdev_devices[SPI_DEVICE].work);
#endif
    pr_info("module exit.\n");
}

module_init(shdev_init);
module_exit(shdev_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Provencore shared devices driver");
MODULE_AUTHOR("Provenrun");
