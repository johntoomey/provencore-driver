// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, ProvenRun S.A.S
 */
/**
 * @file dev_mmc.c
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run and/or its affiliates.
 *   All rights reserved.
 */

#include <linux/blkdev.h>
#include <linux/err.h>

#include "internal.h"

#ifdef CONFIG_PROVENCORE_SHARED_MMC

#if !defined(CONFIG_PROVENCORE_MMC_DEVICE) && !defined(CONFIG_PROVENCORE_MMC_DEVID)
#error "Missing ProvenCore mmc device configuration"
#endif

#ifndef CONFIG_PROVENCORE_MMC_IOCTL_SUSPEND
#define CONFIG_PROVENCORE_MMC_IOCTL_SUSPEND 1
#endif

#ifndef CONFIG_PROVENCORE_MMC_IOCTL_RESUME
#define CONFIG_PROVENCORE_MMC_IOCTL_RESUME 2
#endif

#include <linux/pm_runtime.h>
#include <linux/mmc/host.h>

static int _mmc_block_minor = -1;

static struct block_device *_shdev_mmc_bdev = NULL;

extern char *_shdev_shm_addr;

#ifdef CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/amba/bus.h>
#endif

static struct device *mmcblk_get_device(void)
{
    if (_shdev_mmc_bdev == NULL) {
        return NULL;
    } else {
#ifdef CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE
        struct device_node *node;
        struct device *dev;

        node = of_find_compatible_node(NULL, NULL,
                    CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE);
        if (node == NULL) {
            pr_err("(%s) no compatible node for \""
                    CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE "\"\n",
                    __func__);
            return NULL;
        }
        dev = bus_find_device(&platform_bus_type, NULL, node, of_dev_node_match);
        if (dev) {
            return dev;
        }
        dev = bus_find_device(&amba_bustype, NULL, node, of_dev_node_match);
        if (dev) {
            return dev;
        }

        /* Other bus types maybe used: pci_bus_type, spi_bus_type,
         * pcmcia_bus_type */
        return NULL;
#else /* !CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE */
        struct device *dev = part_to_dev(_shdev_mmc_bdev->bd_part);
        /* mmcblkX->mmcX:0001->mmcX->controller.mmc */
        return dev->parent->parent->parent;
#endif /* CONFIG_PROVENCORE_MMC_COMPATIBLE_DEVICE */
    }
}

/**
 * @brief Lookup the mmc block device with device file path
 *   \ref CONFIG_PROVENCORE_MMC_DEVICE.
 * @return              - ERR_PTR if the path \ref CONFIG_PROVENCORE_MMC_DEVICE
 *                        is invalid
 *                      - pointer to the block_device struct otherwise
 */
static struct block_device *mmcblk_get(void)
{
    if (_shdev_mmc_bdev != NULL) {
        return _shdev_mmc_bdev;
    }
    if (_mmc_block_minor == -1) {
        /* By default the mmc driver uses the default value CONFIG_MMC_BLOCK_MINORS
         * for perdev_minors, but the perdev_minors is also a parameter of
         * mmc module and it is modifiable. If the following message is shown
         * during boot process, the CONFIG_MMC_BLOCK_MINORS should be updated.
         * pr_info("mmcblk: using %d minors per device\n", perdev_minors);
         */
#if defined(CONFIG_PROVENCORE_MMC_DEVID)
        _mmc_block_minor = CONFIG_PROVENCORE_MMC_DEVID * CONFIG_MMC_BLOCK_MINORS;
#else
        char devname[] = CONFIG_PROVENCORE_MMC_DEVICE;
        long devid = 0;
        char *p = devname + sizeof(CONFIG_PROVENCORE_MMC_DEVICE) - 2;
        while (p > devname && p[-1] >= '0' && p[-1] <= '9')
            p--;
        if (kstrtol(p, 10, &devid) != 0 || devid < 0 ||
            devid > INT_MAX / CONFIG_MMC_BLOCK_MINORS)
            return ERR_PTR(-EINVAL);
        _mmc_block_minor = (int) devid * CONFIG_MMC_BLOCK_MINORS;
#endif
    }
    _shdev_mmc_bdev = blkdev_get_by_dev(MKDEV(MMC_BLOCK_MAJOR,
        _mmc_block_minor), FMODE_PATH, NULL);
    if (IS_ERR(_shdev_mmc_bdev) || !_shdev_mmc_bdev->bd_disk) {
        pr_err("(%s) invalid path \"" CONFIG_PROVENCORE_MMC_DEVICE "\"\n",
            __func__);
        _shdev_mmc_bdev = NULL;
        return ERR_PTR(-EINVAL);
    }
    return _shdev_mmc_bdev;
}

static void mmcblk_pm_runtime_disable(void)
{
    struct device *dev = mmcblk_get_device();
    struct mmc_host *mmc;
    if (dev == NULL) {
        return;
    }
    mmc = dev_get_drvdata(dev);
    /* The function mmc_rescan() periodically scan for plugged mmc and try
     * to claim it, disable the rescan_disable variable temporarily blocking
     * mmc_rescan to resume mmc while we are suspending it
     * */
    mmc->rescan_disable = 1;
    cancel_delayed_work_sync(&mmc->detect);
    mmc->pm_flags = 0;

    /* Ref https://www.kernel.org/doc/Documentation/power/runtime_pm.txt
     * 6. Runtime PM and System Sleep
     */
    /* increment the device's usage counter */
    pm_runtime_get_sync(dev);
    /* make sure that all of the pending runtime PM operations on the device
     * are either completed or canceled;
     * */
    pm_runtime_disable(dev);
    /* decrement the device's usage counter */
    pm_runtime_put_noidle(dev);
}

static void mmcblk_pm_runtime_enable(void)
{
    struct device *dev = mmcblk_get_device();
    struct mmc_host *mmc;
    if (dev == NULL) {
        return;
    }

    mmc = dev_get_drvdata(dev);
    mmc->rescan_disable = 0;

    /* set the device's runtime PM status to 'active' and update its
     * parent's counter of 'active' children as appropriate
     * */
    pm_runtime_set_active(dev);

    /* decrement the device's 'power.disable_depth' field; if it is equal
     * to zero, the runtime PM helper functions can execute subsystem-level
     * callbacks for the device
     */
    pm_runtime_enable(dev);
}

static int mmcblk_suspend(void)
{
    int result;
    struct block_device *bdev = mmcblk_get();
    if (IS_ERR(bdev)) {
        return -ENODEV;
    }
    pr_debug("(%s)\n", __func__);
    result = blkdev_ioctl(bdev, 0,
                _IO(MMC_BLOCK_MAJOR, CONFIG_PROVENCORE_MMC_IOCTL_SUSPEND), 0);
    mmcblk_pm_runtime_disable();
    return result;
}

static int mmcblk_resume(void)
{
    int result;
    struct block_device *bdev = mmcblk_get();
    if (IS_ERR(bdev)) {
        return -ENODEV;
    }
    pr_debug("(%s)\n", __func__);
    result = blkdev_ioctl(bdev, 0,
                _IO(MMC_BLOCK_MAJOR, CONFIG_PROVENCORE_MMC_IOCTL_RESUME), 0);
    mmcblk_pm_runtime_enable();
    return result;
}

#ifdef CONFIG_PROVENCORE_MMC_REMOTE_HOST
#include <linux/version.h>
#include <linux/mmc/host.h>
#include <linux/mmc/ioctl.h>
#include <linux/mmc/mmc.h>
#include <linux/syscalls.h>

/* Physical partitions */
#define MMC_PART_USER       0

#ifdef CONFIG_PROVENCORE_MMC_USE_RPMB

#ifndef CONFIG_PROVENCORE_MMC_RPMB_DEVICE
#warning "Using default name for RPMB device: /dev/mmcblk0rpmb"
#define CONFIG_PROVENCORE_MMC_RPMB_DEVICE "/dev/mmcblk0rpmb"
#endif

/* Handling RPMB requests --------------------------------------------------- */

#define MMC_PART_RPMB       3
#define RPMB_FRAME_SIZE     512
#define RPMB_REQ_COUNTER    0x0002
#define RPMB_REQ_WRITE      0x0003
#define RPMB_REQ_READ       0x0004
#define RPMB_REQ_STATUS     0x0005

#define RPMB_WRITE_FLAG_RELIABLE (1 << 31)

/** Definition of a rpmb data frame, 512 bytes in size. */
struct rpmb_frame {
    uint8_t hidden[510];
    uint16_t request;
};

#define RPMB_MULTI_CMD_MAX_CMDS    3

#define INIT_MMC_IOC_CMD(cmd, opc, wflag, ptr)    \
{                                \
    cmd.flags = MMC_RSP_R1;      \
    cmd.blksz = RPMB_FRAME_SIZE; \
    cmd.blocks = 1;              \
    cmd.opcode = opc;            \
    cmd.write_flag = wflag;      \
    cmd.data_ptr = ptr;          \
}

static int mmcblk_remote_host_rpmb(shdev_desc_t *desc_ptr)
{
    shdev_message_t *shdev_mptr = NULL;
    shdev_mmc_entry_t *shdev_mmc_mptr = NULL;
    int result = -EACCES;
    struct mmc_ioc_multi_cmd *pcmds = NULL;
    struct rpmb_frame *frame = NULL;

    shdev_mptr     = &desc_ptr->s_to_ns;
    shdev_mmc_mptr = (shdev_mmc_entry_t *)(_shdev_shm_addr + desc_ptr->entry_offset);
    frame          = (struct rpmb_frame *) (_shdev_shm_addr + desc_ptr->data_offset);

    struct file* filp = filp_open(CONFIG_PROVENCORE_MMC_RPMB_DEVICE, O_RDWR, 0);
    if (IS_ERR(filp))
        return result;

    if (!filp->f_op || !filp->f_op->unlocked_ioctl)
        goto end;

    if (shdev_mptr->operation == SELECT_DEVICE) {
        unsigned long blkcnt = 0;
        unsigned long blksize = 0;
#ifdef CONFIG_PROVENCORE_MMC_RPMB_BLKCNT
        blkcnt = CONFIG_PROVENCORE_MMC_RPMB_BLKCNT;
        result = 0;
#else
        /* BLKGETSIZE may not be implemented on RPMB driver */
        result = filp->f_op->unlocked_ioctl(filp, BLKGETSIZE, (unsigned long)&blkcnt);
        if (result < 0) {
            pr_err("%s: ioctl BLKGETSIZE failed with error %d\n", __func__, result);
            pr_err("%s: CONFIG_PROVENCORE_MMC_RPMB_USE_DEFAULT_BLKCNT "
                   "can be considered to set the default block count\n", __func__);
            goto end;
        }
#endif

#ifdef CONFIG_PROVENCORE_MMC_RPMB_BLKSIZE
        blksize = CONFIG_PROVENCORE_MMC_RPMB_BLKSIZE;
        result = 0;
#else
        /* BLKPBSZGET may not be implemented on RPMB driver */
        result = filp->f_op->unlocked_ioctl(filp, BLKPBSZGET, (unsigned long)&blksize);
        if (result < 0) {
            pr_err("%s: ioctl BLKPBSZGET failed with error %d\n", __func__, result);
            pr_err("%s: CONFIG_PROVENCORE_MMC_RPMB_USE_DEFAULT_BLKSIZE "
                   "can be considered to set the default block count\n", __func__);
            goto end;
        }
#endif

        shdev_mmc_mptr->offset = blksize;
        shdev_mmc_mptr->length = blksize * blkcnt;
        goto end; // ok case
    }

    pcmds = (struct mmc_ioc_multi_cmd *) kmalloc(sizeof(struct mmc_ioc_multi_cmd) +
                RPMB_MULTI_CMD_MAX_CMDS * sizeof(struct mmc_ioc_cmd), GFP_KERNEL);
    if (pcmds == NULL)
        goto end;

    /* Set: .is_acmd .arg .postsleep_min_us .postsleep_max_us
     *      .data_timeout_ns .cmd_timeout_ms to 0 */
    memset(pcmds, 0x0, sizeof(struct mmc_ioc_multi_cmd) +
            RPMB_MULTI_CMD_MAX_CMDS * sizeof(struct mmc_ioc_cmd));
    /* Init common request */
    INIT_MMC_IOC_CMD(pcmds->cmds[0], MMC_WRITE_MULTIPLE_BLOCK, 1,
                     (unsigned long) frame);

    pr_debug("%s rpmb request %u\n", __func__, be16_to_cpu(frame->request));
    switch (be16_to_cpu(frame->request)) {
        case RPMB_REQ_COUNTER:
        case RPMB_REQ_READ:
        {
            #define RPMB_REQ_READ_COUNTER_CMDS   2

            INIT_MMC_IOC_CMD(pcmds->cmds[1], MMC_READ_MULTIPLE_BLOCK, 0,
                             (unsigned long) frame);

            pcmds->num_of_cmds = RPMB_REQ_READ_COUNTER_CMDS;
            /* MMC should not be suspended during the execution of this command */
            result = filp->f_op->unlocked_ioctl(filp, MMC_IOC_MULTI_CMD, (unsigned long) pcmds);
            break; // switch () {}
        }   /* MMC can be suspended */
        case RPMB_REQ_WRITE:
        {
            #define RPMB_REQ_WRITE_CMDS   3
            struct rpmb_frame frame_status = { 0 };
            frame_status.request = cpu_to_be16(RPMB_REQ_STATUS);

            /* RPMB_REQ_WRITE need reliable flag */
            pcmds->cmds[0].write_flag = 1 | RPMB_WRITE_FLAG_RELIABLE;

            INIT_MMC_IOC_CMD(pcmds->cmds[1], MMC_WRITE_MULTIPLE_BLOCK, 1,
                             (unsigned long)&frame_status);

            INIT_MMC_IOC_CMD(pcmds->cmds[2], MMC_READ_MULTIPLE_BLOCK, 0,
                             (unsigned long) frame);

            pcmds->num_of_cmds = RPMB_REQ_WRITE_CMDS;
            /* MMC should not be suspended during the execution of this command */
            result = filp->f_op->unlocked_ioctl(filp, MMC_IOC_MULTI_CMD, (unsigned long) pcmds);
            break; // switch () {}
        }
        default:
            pr_err("%s rpmb request %u is invalid\n", __func__, be16_to_cpu(frame->request));
            break; // switch () {}
    } /* END switch (be16_to_cpu(frame->request)) { */

    kfree(pcmds);
end:
    filp_close(filp, 0);
    return result;
}

/* END Handling RPMB requests ----------------------------------------------- */

#endif /* CONFIG_PROVENCORE_MMC_USE_RPMB */

static int mmcblk_remote_host(shdev_desc_t *desc_ptr)
{
    int ret = -EACCES;
    shdev_message_t *shdev_mptr = NULL;
    shdev_mmc_entry_t *shdev_mmc_mptr = NULL;

    if (desc_ptr->id != SHDEV_DEVICE_TO_ID(MMC_DEVICE)) {
        pr_err("%s: invalid device (%u) !\n", __func__, desc_ptr->id);
        return -EINVAL;
    }
    shdev_mptr      = &desc_ptr->s_to_ns;
    shdev_mmc_mptr  = (shdev_mmc_entry_t *)(_shdev_shm_addr + desc_ptr->entry_offset);

#ifdef CONFIG_PROVENCORE_MMC_USE_RPMB
    if (shdev_mmc_mptr->hwpart == MMC_PART_RPMB) {
        return mmcblk_remote_host_rpmb(desc_ptr);
    }
#endif /* CONFIG_PROVENCORE_MMC_USE_RPMB */

    if (shdev_mmc_mptr->hwpart != MMC_PART_USER) {
        pr_err("Not supported hwpart %u\n", shdev_mmc_mptr->hwpart);
        return -EACCES;
    }

    switch (shdev_mptr->operation) {
        case SELECT_DEVICE:
            {
                unsigned long blkcnt = 0;
                unsigned long blksize= 0;

                struct block_device *bdev = mmcblk_get();
                if (IS_ERR(bdev)) {
                    break;
                }

                ret = blkdev_ioctl(bdev, 0, BLKGETSIZE, (unsigned long)&blkcnt);
                if (ret != 0)
                    break;

                ret = blkdev_ioctl(bdev, 0, BLKPBSZGET, (unsigned long)&blksize);
                if (ret != 0)
                    break;

                shdev_mmc_mptr->offset = blksize;
                shdev_mmc_mptr->length = blksize * (uint64_t)blkcnt;
            }
            break;
        case READ_DEVICE:
        case WRITE_DEVICE:
            {
                uint8_t *mmc_data_ptr = NULL;
                bool write = (shdev_mptr->operation == WRITE_DEVICE);
                struct file* filp;
                loff_t pos;
                size_t length;
                ssize_t transferred_bytes = 0;

                /* Check length of the transfer request */
                if ((uint32_t)shdev_mmc_mptr->length > desc_ptr->data_size) {
                    pr_err("%s: out of bound %s request: %llu/%lu\n", __func__,
                            (write) ? "write":"read", shdev_mmc_mptr->length,
                            (unsigned long)desc_ptr->data_size);
                    return -EINVAL;
                }
                pr_debug("%s: %s: offset=%lu length=%lu\n", __func__,
                        (write) ? "write":"read",
                        (unsigned long)shdev_mmc_mptr->offset, (unsigned long)shdev_mmc_mptr->length);
                filp   = filp_open(CONFIG_PROVENCORE_MMC_DEVICE, O_RDWR, 0);
                if (IS_ERR(filp)) {
                    return PTR_ERR(filp);
                }
                pos    = (loff_t) shdev_mmc_mptr->offset; // offset in bytes
                length = (size_t) shdev_mmc_mptr->length; // number of bytes
                transferred_bytes = 0;
                /* Get address of MMC SHM data buffer */
                mmc_data_ptr = (uint8_t *)(_shdev_shm_addr + desc_ptr->data_offset);
                if (write) {
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,13,0)
                    transferred_bytes = (ssize_t) kernel_write(filp, (void *)mmc_data_ptr, length, pos);
#else  /* KERNEL_VERSION > 4,13,0) */
                    transferred_bytes = kernel_write(filp, (void *)mmc_data_ptr, length, &pos);
#endif /* LINUX_VERSION_CODE <= KERNEL_VERSION(4,13,0) */
                } else {
#if LINUX_VERSION_CODE <= KERNEL_VERSION(4,13,0)
                    transferred_bytes = (ssize_t) kernel_read(filp, pos, (void *)mmc_data_ptr, length);
#else  /* KERNEL_VERSION > 4,13,0) */
                    transferred_bytes = kernel_read(filp, (void *)mmc_data_ptr, length, &pos);
#endif /* LINUX_VERSION_CODE <= KERNEL_VERSION(4,13,0) */
                }
                if ((transferred_bytes > 0) && ((size_t) transferred_bytes == length)) {
                    ret = 0;
                } else {
                    ret = transferred_bytes?(int)transferred_bytes:-EACCES;
                }
                filp_close(filp, 0);
            }
            break;
        default:
            ret = -EINVAL;
            break;
    }
    return ret;
}
#endif /* CONFIG_PROVENCORE_MMC_REMOTE_HOST */

static shdev_ops_t mmcblk_ops = {
    .suspend = mmcblk_suspend,
    .resume  = mmcblk_resume,
#ifdef CONFIG_PROVENCORE_MMC_REMOTE_HOST
    .select  = mmcblk_remote_host,
    .read    = mmcblk_remote_host,
    .write   = mmcblk_remote_host,
#ifdef CONFIG_PROVENCORE_MMC_USE_RPMB
    .rpmb    = mmcblk_remote_host,
#endif /* CONFIG_PROVENCORE_MMC_USE_RPMB */
#endif
};

shdev_ops_t *mmcblk_init(void)
{
    return &mmcblk_ops;
}

#endif /* CONFIG_PROVENCORE_SHARED_MMC */
