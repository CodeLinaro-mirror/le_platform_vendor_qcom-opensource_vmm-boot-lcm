/* Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

/*===========================================================================
@file
    vmm-boot-lcm.cpp

@brief
    vmm boot lifecycle manager

@details
    Simple code that controls the lifecycle of VMs that are started by VMM.
    If the GVM fails to start continuously, the recovery policy is executed.

@current
    Currently, only one GVM can be controlled, and the recovery cookie will 
    be set to enter the android recovery mode after the continuous boot fails.

==========================================================================*/
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include "libabctl.h"
#ifdef USE_GLIB
#include <glib.h>
#endif
#include "vmm_events.h"
#include "vmm_log.h"
#include "vmm_clib.h"
#include "vm_config.h"

#define GVM_MISC_PARTITION_PATH_PREFIX              "/dev/disk/by-partlabel/"

#define SYMMETRIC_SLOT_SWITCH       1
#define ASYMMETRIC_SLOT_SWITCH      2

typedef struct vmm_boot_lcm vmm_boot_lcm_t;

typedef enum lcm_request {
    STOP_GUEST = 0,
    START_GUEST = 1
} lcm_request_t;

typedef struct gvm_context {
    char vm_name[16];
    uint32_t vmid;
    int vmm_boot_lcm_enable;
    int lcm_retry_count;
    int slot_switch_config;
    pthread_mutex_t loop_mutex;
    pthread_cond_t wait_on_event;
    int32_t lcm_event;
    bool event_update;
    bool shutdown_requested;
    char misc_partition_path[PATH_MAX];
    int32_t retry_cnt;
    bool recovery_set;
    pthread_t loop_thread;
    vmm_boot_lcm_t *vmm_boot_lcm;
} gvm_context_t;

typedef struct vmm_boot_lcm {
    char host_boot_slot;
    void *vmm_handle;
    uint32_t num_gvms;
    gvm_context_t *gvm_ctxs;
} vmm_boot_lcm_t;

struct bootloader_message {
    char command[32];
    char status[32];
    char recovery[768];
    char stage[32];
    char reserved[1184];
};

struct boot_slot_info {
    int  recovery_flag;
    char current_slot;
    char target_slot;
    char bootable_status;
};

static int set_gvm_recovery_cmd(const char *partition_path, const char *recovery)
{
    struct bootloader_message *msg;
    ssize_t nbytes;
    off_t seek;
    int misc_fd;
    int ret = -1;

    if (partition_path == NULL) {
        vmm_err("Invalid partition path\n");
        goto exit;
    }

    msg = (struct bootloader_message *)calloc(1, sizeof(struct bootloader_message));
    if (msg == NULL) {
        vmm_err("Failed to alloc memory for msg");
        goto exit;
    }

    misc_fd = open(partition_path, O_RDWR);
    if (misc_fd < 0) {
        vmm_err("Failed to open misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_open;
    }

    seek = lseek(misc_fd, 0, SEEK_SET);
    if (seek != 0) {
        if (seek > 0)
            vmm_err("Failed to lseek misc partition %s to seek 0", partition_path);
        else
            vmm_err("Failed to lseek misc partition %s to seek 0, err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_lseek_read;
    }

    nbytes = read(misc_fd, msg, sizeof(*msg));
    if (nbytes != sizeof(*msg)) {
        if (nbytes < 0)
            vmm_err("Failed to read misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        else
            vmm_err("Failed to read misc partition %s", partition_path);
        goto error_read;
    }

    seek = lseek(misc_fd, 0, SEEK_SET);
    if (seek != 0) {
        if (seek > 0)
            vmm_err("Failed to lseek misc partition %s to seek 0", partition_path);
        else
            vmm_err("Failed to lseek misc partition %s to seek 0, err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_lseek_write;
    }

    (void)strlcpy(msg->command, "boot-recovery", sizeof("boot-recovery"));
    (void)strlcpy(msg->recovery, recovery, strlen(recovery) + 1);

    nbytes = write(misc_fd, msg, sizeof(*msg));
    if (nbytes == sizeof(*msg)) {
        vmm_info("Recovery command of gvm is written to partition");
    } else {
        if (nbytes < 0)
            vmm_err("Failed to write to misc partition %s, err = %d(%s)", partition_path, errno, strerror(errno));
        else
            vmm_err("Failed to write to misc partition %s", partition_path);
        goto error_write;
    }

    ret = EOK;

error_write:
error_lseek_write:
error_read:
error_lseek_read:
    close(misc_fd);
error_open:
    free(msg);
exit:
    return ret;
}

static int set_gvm_taget_slot(const char *partition_path, char taget_slot)
{
    ssize_t nbytes;
    off_t seek;
    int misc_fd;
    int ret = -1;

    if (partition_path == NULL) {
        vmm_err("Invalid partition path\n");
        goto exit;
    }

    if (taget_slot != 'a' && taget_slot != 'b') {
        vmm_err("Invalid taget slot %c\n", taget_slot);
        goto exit;
    }

    misc_fd = open(partition_path, O_RDWR);
    if (misc_fd < 0) {
        vmm_err("Failed to open misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_open;
    }

    seek = lseek(misc_fd, offsetof(struct bootloader_message, reserved[1]), SEEK_SET);
    if (seek != offsetof(struct bootloader_message, reserved[1])) {
        if (seek > 0)
            vmm_err("Failed to lseek misc partition %s", partition_path);
        else
            vmm_err("Failed to lseek misc partition %s, err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_lseek;
    }

    nbytes = write(misc_fd, &taget_slot, sizeof(char));
    if (nbytes == sizeof(char)) {
        vmm_info("Taget slot %c of gvm is written to partition", taget_slot);
    } else {
        if (nbytes < 0)
            vmm_err("Failed to write to misc partition %s, err = %d(%s)", partition_path, errno, strerror(errno));
        else
            vmm_err("Failed to write to misc partition %s", partition_path);
        goto error_write;
    }

    ret = EOK;

error_write:
error_lseek:
    close(misc_fd);
error_open:
exit:
    return ret;
}


static int set_bootable_status(const char *partition_path, char bootable_status)
{
    ssize_t nbytes;
    off_t seek;
    int misc_fd;
    int ret = -1;

    if (partition_path == NULL) {
        vmm_err("Invalid partition path\n");
        goto exit;
    }

    if (bootable_status != 'n' && bootable_status != 'y') {
        vmm_err("Invalid boot status %c\n", bootable_status);
        goto exit;
    }

    misc_fd = open(partition_path, O_RDWR);
    if (misc_fd < 0) {
        vmm_err("Failed to open misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_open;
    }

    seek = lseek(misc_fd, offsetof(struct bootloader_message, reserved[2]), SEEK_SET);
    if (seek != offsetof(struct bootloader_message, reserved[2])) {
        if (seek > 0)
            vmm_err("Failed to lseek misc partition %s", partition_path);
        else
            vmm_err("Failed to lseek misc partition %s, err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_lseek;
    }

    nbytes = write(misc_fd, &bootable_status, sizeof(char));
    if (nbytes == sizeof(char)) {
        vmm_info("bootable_status %c of gvm is written to partition", bootable_status);
    } else {
        if (nbytes < 0)
            vmm_err("Failed to write to misc partition %s, err = %d(%s)", partition_path, errno, strerror(errno));
        else
            vmm_err("Failed to write to misc partition %s", partition_path);
        goto error_write;
    }

    ret = EOK;

error_write:
error_lseek:
    close(misc_fd);
error_open:
exit:
    return ret;
}


static int check_gvm_boot_slot_info(const char *partition_path, struct boot_slot_info *slot_info)
{
    struct bootloader_message *msg;
    ssize_t nbytes;
    off_t seek;
    int misc_fd;
    int ret = -1;

    if (partition_path == NULL || slot_info == NULL) {
        vmm_err("Invalid partition path or vmid or slot_info\n");
        goto exit;
    }

    msg = (struct bootloader_message *)calloc(1, sizeof(struct bootloader_message));
    if (msg == NULL) {
        vmm_err("Failed to alloc memory for msg");
        goto exit;
    }

    misc_fd = open(partition_path, O_RDWR);
    if (misc_fd < 0) {
        vmm_err("Failed to open misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_open;
    }

    seek = lseek(misc_fd, 0, SEEK_SET);
    if (seek != 0) {
        if (seek > 0)
            vmm_err("Failed to lseek misc partition %s to seek 0", partition_path);
        else
            vmm_err("Failed to lseek misc partition %s to seek 0, err = %d(%s)", partition_path, errno, strerror(errno));
        goto error_lseek;
    }

    nbytes = read(misc_fd, msg, sizeof(*msg));
    if (nbytes != sizeof(*msg)) {
        if (nbytes < 0)
            vmm_err("Failed to read misc partition %s , err = %d(%s)", partition_path, errno, strerror(errno));
        else
            vmm_err("Failed to read misc partition %s", partition_path);
        goto error_read;
    }

    slot_info->current_slot = msg->reserved[0];
    slot_info->bootable_status = msg->reserved[2];
    if ((msg->reserved[1] == 'a') || (msg->reserved[1] == 'b') ) {
        slot_info->target_slot = msg->reserved[1];
        vmm_info("target slot: %c , current slot: %c , bootable_status %c", slot_info->target_slot, slot_info->current_slot, slot_info->bootable_status);
    } else {
        slot_info->target_slot = '\0';
        vmm_info("target_slot info is not avaiable in la_misc");
    }

    if (!strncmp(msg->command, "boot-recovery", sizeof("boot-recovery")-1)) {
        if (!strncmp(msg->recovery, "recovery\n--wipe_data", sizeof("recovery\n--wipe_data")-1) || 
        !strncmp(msg->recovery, "recovery\n--fastboot", sizeof("recovery\n--fastboot")-1) || /* check 
        for adb reboot fastboot */
        (msg->recovery[0] == '\0')) {   /* check for adb reboot recovery */
            vmm_info("RECOVERY command=%s recovery=%s\n", msg->command, msg->recovery);
            slot_info->recovery_flag = 1;
        } else {
            vmm_info("No recovery command found");
            slot_info->recovery_flag = 0;
        }
    } else {
        slot_info->recovery_flag = 0;
        vmm_info("boot-recovrey command is not present in la_misc");
    }

    ret = EOK;

error_read:
error_lseek:
    close(misc_fd);
error_open:
    free(msg);
exit:
    return ret;
}

static gvm_context_t* find_gvm_context_by_vmid(vmm_boot_lcm_t *vmm_boot_lcm, uint32_t vmid)
{
    for(int i = 0; i < vmm_boot_lcm->num_gvms; i++) {
        if(vmm_boot_lcm->gvm_ctxs[i].vmid == vmid)
            return &vmm_boot_lcm->gvm_ctxs[i];
    }

    return NULL;
}

static int vmm_event_callback(uint32_t vmid, vmm_event_t event, void *priv_data)
{
    vmm_boot_lcm_t *vmm_boot_lcm = (vmm_boot_lcm_t*)priv_data;
    gvm_context_t *gvm_ctx = NULL;

    gvm_ctx = find_gvm_context_by_vmid(vmm_boot_lcm, vmid);
    if(gvm_ctx == NULL) {
        vmm_err("Unknow vmid %u event recieved", vmid);
        return -EINVAL;
    }

    switch (event) {
    case GVM_WDOG_BITE:
    case GVM_CONTAINER_CRASH:
    case GVM_HANDLED_CONTAINER_CRASH:
    case GVM_STOPPED:
    case GVM_BAD_STATE:
        vmm_info("Gvm stopped due to event = %d  vmid: %d",event, vmid);
        break;
    case GVM_SHUTDOWN:
        vmm_info("Gvm requested for shutdown event = %d  vmid: %d",event, vmid);
        gvm_ctx->shutdown_requested = true;
        break;
    case GVM_EVENT_DOWN:
        vmm_info("Received lcm DOWN event vmid:%d", vmid);
        if (gvm_ctx->shutdown_requested == false) {
            pthread_mutex_lock(&gvm_ctx->loop_mutex);
            gvm_ctx->lcm_event = GVM_EVENT_DOWN;
            gvm_ctx->event_update = true;
            pthread_cond_signal(&gvm_ctx->wait_on_event);
            pthread_mutex_unlock(&gvm_ctx->loop_mutex);
        }
        break;
    case GVM_EVENT_UP:
        vmm_info("Received lcm UP event vmid:%d", vmid);
        gvm_ctx->shutdown_requested = false;
        break;
    case GVM_EVENT_FATAL_ERROR:
        vmm_err("Fatal error occurred on the gvm");
        break;
    default:
        break;
    }

    return EOK;
}

static int do_failure_recovery(gvm_context_t *gvm_ctx)
{
    return set_gvm_recovery_cmd(gvm_ctx->misc_partition_path, "recovery");
}

static int control_vm(vmm_boot_lcm_t *vmm_boot_lcm, gvm_context_t *gvm_ctx, lcm_request_t request)
{
    struct boot_slot_info slot_info;
    int count = 2;
    int ret = -1;

    switch (request) {
    case START_GUEST:
        ret = check_gvm_boot_slot_info(gvm_ctx->misc_partition_path, &slot_info);
        if (ret != EOK) {
            vmm_err("Failed in gvm boot slot check");
            return ret;
        }
        switch (slot_info.bootable_status) {
        case 'y':
            //set_bootable_status(gvm_ctx->misc_partition_path, 'n');
            gvm_ctx->retry_cnt = gvm_ctx->lcm_retry_count;
            gvm_ctx->recovery_set = false;
            break;
        default:
            if(gvm_ctx->retry_cnt > 0)
                --gvm_ctx->retry_cnt;

            if (gvm_ctx->retry_cnt == 0 && gvm_ctx->recovery_set == false) {
                (void)do_failure_recovery(gvm_ctx);
                gvm_ctx->recovery_set = true;
            }
            break;
        }

        vmm_dbg("vm boot retry count %d", gvm_ctx->retry_cnt);

        do {
            ret = vmm_request_contrl_vm(gvm_ctx->vmid, VM_CONTROL_START, vmm_boot_lcm->vmm_handle);
            count--;
        } while (ret != EOK && count > 0);
        break;
    case STOP_GUEST:
        break;
    default:
        vmm_err("unexpected event recieved %d");
    }

    return ret;
}

static int parse_gvm_info_and_populate_ctx(vmm_boot_lcm_t *vmm_boot_lcm)
{
    int ret = -1;
    uint32_t vmid;
    const char *vm_str;
    gvm_context_t *gvm_ctx;

    ret = vm_config_init();
    if (ret != EOK) {
        vmm_err("Failed vm_config_init %d", ret);
        goto exit;
    }

    vmm_boot_lcm->num_gvms = vm_config_get_num_vm();

    vmm_boot_lcm->gvm_ctxs = (gvm_context_t*)calloc(1, sizeof(gvm_context_t) * vmm_boot_lcm->num_gvms);
    if (vmm_boot_lcm->gvm_ctxs == NULL) {
        vmm_err("Failed to acllocate memory for gvm_info");
        ret = -ENOMEM;
        goto exit;
    }

    for (int i=0; i < vmm_boot_lcm->num_gvms; i++) {
        gvm_ctx = &vmm_boot_lcm->gvm_ctxs[i];

        gvm_ctx->vmm_boot_lcm = vmm_boot_lcm;

        ret = vm_config_get_vmid(i, &vmid);
        if (ret != EOK) {
            vmm_err("Failed get vmid for vm idx %d with %d", i, ret);
            goto exit;
        }
        gvm_ctx->vmid = vmid;

        ret = vm_config_get_vmm_boot_lcm_enable(vmid);
        if (ret < 0) {
            vmm_err("Failed get vmm_boot_lcm_enable for vmid %d with %d", vmid, ret);
            goto exit;
        }
        gvm_ctx->vmm_boot_lcm_enable = !!ret;

        ret = vm_config_get_lcm_retry_count(vmid);
        if (ret < 0) {
            vmm_err("Failed get lcm_retry_count for vmid %d with %d", vmid, ret);
            goto exit;
        }
        gvm_ctx->lcm_retry_count = ret;

        ret = vm_config_get_slot_switch_config(vmid);
        if (ret < 0) {
            vmm_err("Failed get slot_switch_config for vmid %d with %d", vmid, ret);
            goto exit;
        }
        gvm_ctx->slot_switch_config = ret;

        vm_str = vm_config_get_misc_partition(vmid);
        if (vm_str == NULL) {
            vmm_err("Failed get misc_partition for vmid %u", vmid);
            goto exit;
        }
        (void)snprintf(gvm_ctx->misc_partition_path, sizeof(gvm_ctx->misc_partition_path), "%s%s", GVM_MISC_PARTITION_PATH_PREFIX, vm_str);

        vm_str = vm_config_get_vm_name(vmid);
        if (vm_str == NULL) {
            vmm_err("Failed get vm name for vmid %u", vmid);
            goto exit;
        }
        (void)strlcpy(gvm_ctx->vm_name, vm_str, sizeof(gvm_ctx->vm_name));

        gvm_ctx->retry_cnt = gvm_ctx->lcm_retry_count;

        ret = pthread_mutex_init(&gvm_ctx->loop_mutex, NULL);
        if (ret != EOK) {
            vmm_err("Failed to initialize loop_mutex \n");
            goto exit;
        }

        ret = pthread_cond_init(&gvm_ctx->wait_on_event, NULL);
        if (ret != EOK) {
            vmm_err("Failed to initialize wait_on_event\n");
            goto exit;
        }
    }

    ret = EOK;

exit:
    return ret;
}

void* vmm_lcm_event_loop(void *ctx)
{
    gvm_context_t *gvm_ctx = (gvm_context_t*)ctx;
    uint32_t lcm_event;
    int ret = -1;

    vmm_boot_lcm_t *vmm_boot_lcm = gvm_ctx->vmm_boot_lcm;
    ret = pthread_setname_np(pthread_self(), "event_loop");
    if (ret != EOK)
        vmm_err("Failed to set name vmm_lcm_event_loop thread");

    ret = control_vm(vmm_boot_lcm, gvm_ctx, START_GUEST);
    if (ret != EOK) {
        vmm_err("Default start gvm failed");
        goto exit;
    }

    while (true) {
        vmm_dbg("Wait for vmm lifecycle event");
        pthread_mutex_lock(&gvm_ctx->loop_mutex);
        while (gvm_ctx->event_update == false)
            pthread_cond_wait(&gvm_ctx->wait_on_event, &gvm_ctx->loop_mutex);
        lcm_event = gvm_ctx->lcm_event;
        gvm_ctx->event_update = false;
        pthread_mutex_unlock(&gvm_ctx->loop_mutex);
        vmm_dbg("vmm lifecycle event recieved");

        switch (lcm_event) {
        case GVM_EVENT_DOWN:
            ret = control_vm(vmm_boot_lcm, gvm_ctx, START_GUEST);
            if (ret != EOK) {
                vmm_err("Failed to start gvm");
            }
            break;
        case GVM_EVENT_UP:
            break;
        default:
            vmm_err("unexpected event recieved %d");
        }
    }

exit:
    return NULL;
}


static int vmm_lcm_event_loop_thread_create(vmm_boot_lcm_t *vmm_boot_lcm)
{
    int ret = -1;
    pthread_attr_t attr;
    gvm_context_t *gvm_ctx;

    ret = pthread_attr_init(&attr);
    if (ret != 0) {
        vmm_err("Failed to initialize pthread_attr: %d\n", ret);
        goto exit;
    }

    ret = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (ret != 0) {
        vmm_err("Failed to set detached attribute: %d\n", ret);
        goto exit;
    }

    for (int i = 0; i < vmm_boot_lcm->num_gvms; i++) {
        gvm_ctx = &vmm_boot_lcm->gvm_ctxs[i];
        if (gvm_ctx->vmm_boot_lcm_enable == false) continue;
        ret = pthread_create(&gvm_ctx->loop_thread, &attr, vmm_lcm_event_loop, (void*)gvm_ctx);
        if (ret != 0) {
            vmm_err("Failed to create vmm_lcm_event_loop thread  %d\n", ret);
            goto exit;
        }
    }

    ret = EOK;

exit:
    return ret;
}

static int vmm_boot_lcm_init(vmm_boot_lcm_t *vmm_boot_lcm)
{
    int ret = -1;
    uint32_t *vmids;
    uint32_t num_gvm_lcm_enable = 0;
    vmm_subscribe_attr_t  s_attr = {0};
    struct boot_slot_info slot_info;
    gvm_context_t *gvm_ctx;


    ret = libabctl_getBootSlot();
    if (ret != 0 && ret != 1) {
        vmm_err("Can not get host boot slot\n");
        ret = -1;
        goto exit;
    }
    vmm_boot_lcm->host_boot_slot = ret ? 'b' : 'a';
    vmm_info("host boot slot %c\n", vmm_boot_lcm->host_boot_slot);

    ret = parse_gvm_info_and_populate_ctx(vmm_boot_lcm);
    if (ret != EOK) {
        vmm_err("Failed to parse gvm info and populate ctx");
        goto exit;
    }

    vmids = (uint32_t*)calloc(vmm_boot_lcm->num_gvms, sizeof(uint32_t));
    if (vmids == NULL) {
        vmm_err("Failed to acllocate memory for vmids");
        goto exit;
    }

    for (int i = 0; i < vmm_boot_lcm->num_gvms; i++) {
        gvm_ctx = &vmm_boot_lcm->gvm_ctxs[i];
        if (gvm_ctx->vmm_boot_lcm_enable == true) {
            vmids[num_gvm_lcm_enable] = gvm_ctx->vmid;
            num_gvm_lcm_enable++;
        }
    }

    if (num_gvm_lcm_enable == 0) {
        vmm_info("There is no gvm to enable lcm");
        goto exit;
    }

    ret = vmm_client_connect(VMM_BOOT_LIFECYC_MANAGER_CLIENT_NAME, VMM_SERVICE_SERVER, &vmm_boot_lcm->vmm_handle);
    if (ret != EOK) {
        vmm_err("Failed to connect to vmm service, Error: %d \n", ret);
        goto exit;
    }

    s_attr.event_cb_func = vmm_event_callback;
    s_attr.event_mask = GVM_SHUTDOWN_LEVEL_0 | GVM_SHUTDOWN_LEVEL_1 | GVM_EVENT_UP | GVM_EVENT_DOWN | GVM_EVENT_FATAL_ERROR;
    s_attr.level = LEVEL_0;
    s_attr.priv_data = (void*)vmm_boot_lcm;

    ret = vmm_subscribe_event_notification(vmm_boot_lcm->vmm_handle, num_gvm_lcm_enable, vmids, &s_attr);
    if (ret != EOK) {
        vmm_err("Failed to subscribe event notification, Error: %d \n", ret);
        goto exit;
    }
    free(vmids);

    ret = EOK;

exit:
    return ret;
}


int main(int argc, char *argv[])
{
    int ret = -1;
    vmm_boot_lcm_t *vmm_boot_lcm;

    vmm_boot_lcm = (vmm_boot_lcm_t*)calloc(1, sizeof(vmm_boot_lcm_t));
    if (vmm_boot_lcm == NULL) {
        vmm_err("Failed to acllocate memory for vmm_boot_lcm");
        goto exit;
    }

    ret = vmm_boot_lcm_init(vmm_boot_lcm);
    if (ret != EOK) {
        vmm_err("Failed to vmm_boot_lcm_init with %d\n", ret);
        goto exit;
    }

    ret = vmm_lcm_event_loop_thread_create(vmm_boot_lcm);
    if (ret != EOK) {
        vmm_err("Failed to create vmm lifecycle event loop threads %d\n", ret);
        goto exit;
    }

    pause();

exit:
    return ret;
}
