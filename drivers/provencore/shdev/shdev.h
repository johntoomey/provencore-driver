// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, ProvenRun S.A.S
 */
/**
 * @file    shdev.h
 * @brief
 * @author Alexandre Berdery
 * @date December 4th, 2020 (creation)
 * @copyright (c) 2020-2021, Prove & Run S.A.S and/or its affiliates.
 *   All rights reserved.
 */

#ifndef _SHDEV_H_INCLUDED_
#define _SHDEV_H_INCLUDED_

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/types.h>
#ifndef UINT32_C
#define UINT32_C(c) c ## U
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#else
#include <stdint.h>
#include <string.h>
#endif

/* Shared device(s) monitor is responsible for handling of device(s) shared
 * between S and NS.
 *
 * List of supported shared devices:
 * - MMC
 * - ENET
 * - SPI
 *
 * S is not supposed to be blocked waiting for a shared device to be ready
 * before using it. Nevertheless, when monitoring is activated, it will
 * asynchronously send messages to NS in order to indicate a request for a
 * device's operation  that can be:
 *  - SUSPEND (S is going to use a shared device)
 *  - RESUME  (S ended up with shared device usage)
 *
 * New messages are indicated sending signals.
 *
 * 2 monitor's internal ring buffers are used (one for S-->NS, one for NS-->S)
 * to share messages..
 *
 * When receiving a signal, NS:
 *  - check for new message(s) into S-->NS ring buffer
 *  - perform requested operation(s)
 *  - report operation's status into NS-->S ring buffer
 *  - signal S for new available message(s).
 *
 * When receiving a signal, S:
 *  - check for new message(s) into NS-->S ring buffer
 *  - handle operation's status
 *
 * S may not wait for operation's status before using shared device or sending
 * new signal.
 *
 */

/** Num of PAGE_SIZE pages used to shared device monitor's infos & messages */
#define SHDEV_PAGES    1

/** Num of PAGE_SIZE pages used to shared device monitor's for remote MMC feature
 * handling (e.g used only if CONFIG_PROVENCORE_MMC_REMOTE_HOST defined)
 */
#define SHDEV_MMC_PAGES    1

/** Different type of signals that can be sent from S-->NS or NS-->S */
enum shdev_signals {
    /* Sent by NS to signal S monitor is ready: a way for S to automatically
     * get monitor's session's index. Before this signal, S don't know where to
     * send its signals...
     */
    SIGNAL_READY = 0,

    /* Indicate NS and S that new S, respectively NS, MMC message is available */
    SIGNAL_MMC_MESSAGE,

    /* Indicate NS and S that new S, respectively NS, ENET message is available */
    SIGNAL_ENET_MESSAGE,

    /* Indicate NS and S that new S, respectively NS, SPI message is available */
    SIGNAL_SPI_MESSAGE,

    /* No signal value upon this limit: signals for a session are using a 32-bit
     * integer register. hence this 32 limitation for the max num of different
     * type of signals S and NS can share.
     */
    SIGNAL_INVALID = 32
};

/** Macro to set specific signal into 32-bit register */
#define SHDEV_SIGNAL(b)    (UINT32_C(1) << b)

/** List of supported shared devices */
#define SHDEV_DEVICE_PREFIX   UINT32_C(0xabed0000)
enum shdev_devices {
    MMC_DEVICE = 0,
    ENET_DEVICE,
    SPI_DEVICE,

    /* No device upon this limit that is used to know the size of shdev_devices
     * table
     */
    NUM_DEVICES
};
#define SHDEV_DEVICE_TO_ID(b)    ((uint32_t)b | SHDEV_DEVICE_PREFIX)
#define SHDEV_ID_TO_DEVICE(b)    ((uint32_t)b & ~SHDEV_DEVICE_PREFIX)

/** List of supported operations on a shared device */
enum shdev_operations {
    SUSPEND_DEVICE = 0,
    RESUME_DEVICE,
    SELECT_DEVICE,
    READ_DEVICE,
    WRITE_DEVICE,
    RPMB_DEVICE,

    /* Limit value for an operation, given it is encoded into uint16_t... */
    INVALID_OPERATION=0x10000
};

/** At runtime, for some MMC operations, messages exchanged between S and NS
 * require specific entries to describe the operation. These entries will all
 * have the following structure.
 */
typedef struct mmc_infos {
    /* MMC device ID */
    uint32_t devid;
    /* MMC h/w partition number */
    uint32_t hwpart;
    /* MMC logical partition */
    uint32_t lpart;
    /* Offset in partition */
    uint64_t offset;
    /* Size of data */
    uint64_t length;
} shdev_mmc_entry_t;

/** Description of a message shared between S and NS at runtime */
typedef struct shdev_message {
    /* Operation requested on shared device (see \ref enum shdev_operations) */
    uint16_t operation;
    /* Index of device to run operation on (see \ref NUM_DEVICES) */
    uint16_t device;
    /* Set to 1 to indicate this message is a request's status, set to 0 to
     * indicate it is a request.
     */
    uint16_t status;
    /* Status value... */
    uint16_t value;
} shdev_message_t;

/** Shared device descriptor */
typedef struct shdev_desc {
    /* Device identifier */
    uint32_t id;
    /* Slot for NS->S operation message */
    shdev_message_t ns_to_s;
    /* Slot for S->NS status message */
    shdev_message_t s_to_ns;

    /* OPTIONAL descriptors below... */

    /* Offset of device description entry */
    uint32_t entry_offset;
    /* Size of device description entry */
    uint32_t entry_size;
    /* Offset of shared data buffer if any: will always be aligned on a
     * PAGE_SIZE boundary */
    uint32_t data_offset;
    /* Size of shared data buffer if any: will always be aligned on a PAGE_SIZE
     * boundary. */
    uint32_t data_size;
} shdev_desc_t;

/** At start-up NS shares some generic infos with S. These infos are shared
 * through SHM. Explicit magic is stored in SHM in order to confirm following
 * data can be handled as \ref shdev_infos_t
 */
#define SHDEV_MAGIC_INFOS   UINT32_C(0xabeef001)
typedef struct shdev_infos {
    /* Magic */
    uint32_t magic;
    /* Num of handled devices */
    uint32_t num_devices;
    /* Descriptors for each supported device. Optional. Device sharing can
     * work only with shdev_message_t informations.
     */
    shdev_desc_t descriptors[NUM_DEVICES];
} shdev_infos_t;

#endif /* _SHDEV_H_INCLUDED_ */
