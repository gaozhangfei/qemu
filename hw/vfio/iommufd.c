/*
 * iommufd container backend
 *
 * Copyright (C) 2022 Intel Corporation.
 * Copyright Red Hat, Inc. 2022
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <linux/iommufd.h>

#include "hw/vfio/vfio-common.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "qapi/error.h"
#include "sysemu/iommufd.h"
#include "hw/qdev-core.h"
#include "sysemu/reset.h"
#include "qemu/cutils.h"
#include "qemu/char_dev.h"
#include "exec/address-spaces.h"

static bool iommufd_check_extension(VFIOContainer *bcontainer,
                                    VFIOContainerFeature feat)
{
    switch (feat) {
    case VFIO_FEAT_DMA_COPY:
        return true;
    default:
        return false;
    };
}

static int iommufd_map(VFIOContainer *bcontainer, hwaddr iova,
                       ram_addr_t size, void *vaddr, bool readonly)
{
    VFIOIOMMUFDContainer *container =
        container_of(bcontainer, VFIOIOMMUFDContainer, bcontainer);

    return iommufd_backend_map_dma(container->be,
                                   container->ioas_id,
                                   iova, size, vaddr, readonly);
}

static int iommufd_copy(VFIOContainer *src, VFIOContainer *dst,
                        hwaddr iova, ram_addr_t size, bool readonly)
{
    VFIOIOMMUFDContainer *container_src = container_of(src,
                                             VFIOIOMMUFDContainer, bcontainer);
    VFIOIOMMUFDContainer *container_dst = container_of(dst,
                                             VFIOIOMMUFDContainer, bcontainer);

    assert(container_src->be->fd == container_dst->be->fd);

    return iommufd_backend_copy_dma(container_src->be, container_src->ioas_id,
                                    container_dst->ioas_id, iova,
                                    size, readonly);
}

static int iommufd_unmap(VFIOContainer *bcontainer,
                         hwaddr iova, ram_addr_t size,
                         IOMMUTLBEntry *iotlb)
{
    VFIOIOMMUFDContainer *container =
        container_of(bcontainer, VFIOIOMMUFDContainer, bcontainer);

    /* TODO: Handle dma_unmap_bitmap with iotlb args (migration) */
    return iommufd_backend_unmap_dma(container->be,
                                     container->ioas_id, iova, size);
}

static int vfio_get_devicefd(const char *sysfs_path, Error **errp)
{
    long int ret = -ENOTTY;
    char *path, *vfio_dev_path = NULL, *vfio_path = NULL;
    DIR *dir;
    struct dirent *dent;
    gchar *contents;
    struct stat st;
    gsize length;
    int major, minor;
    dev_t vfio_devt;

    path = g_strdup_printf("%s/vfio-dev", sysfs_path);
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "no such host device");
        goto out_free_path;
    }

    dir = opendir(path);
    if (!dir) {
        error_setg_errno(errp, errno, "couldn't open dirrectory %s", path);
        goto out_free_path;
    }

    while ((dent = readdir(dir))) {
        if (!strncmp(dent->d_name, "vfio", 4)) {
            vfio_dev_path = g_strdup_printf("%s/%s/dev", path, dent->d_name);
            break;
        }
    }

    if (!vfio_dev_path) {
        error_setg(errp, "failed to find vfio-dev/vfioX/dev");
        goto out_free_path;
    }

    if (!g_file_get_contents(vfio_dev_path, &contents, &length, NULL)) {
        error_setg(errp, "failed to load \"%s\"", vfio_dev_path);
        goto out_free_dev_path;
    }

    if (sscanf(contents, "%d:%d", &major, &minor) != 2) {
        error_setg(errp, "failed to get major:mino for \"%s\"", vfio_dev_path);
        goto out_free_dev_path;
    }
    g_free(contents);
    vfio_devt = makedev(major, minor);

    vfio_path = g_strdup_printf("/dev/vfio/devices/%s", dent->d_name);
    ret = open_cdev(vfio_path, vfio_devt);
    if (ret < 0) {
        error_setg(errp, "Failed to open %s", vfio_path);
    }

    trace_vfio_iommufd_get_devicefd(vfio_path, ret);
    g_free(vfio_path);

out_free_dev_path:
    g_free(vfio_dev_path);
out_free_path:
    if (*errp) {
        error_prepend(errp, VFIO_MSG_PREFIX, path);
    }
    g_free(path);

    return ret;
}

static VFIOIOASHwpt *vfio_container_get_hwpt(VFIOIOMMUFDContainer *container,
                                             uint32_t hwpt_id)
{
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (hwpt->hwpt_id == hwpt_id) {
            return hwpt;
        }
    }

    hwpt = g_malloc0(sizeof(*hwpt));

    hwpt->hwpt_id = hwpt_id;
    QLIST_INIT(&hwpt->device_list);
    QLIST_INSERT_HEAD(&container->hwpt_list, hwpt, next);

    return hwpt;
}

static void vfio_container_put_hwpt(VFIOIOASHwpt *hwpt)
{
    if (!QLIST_EMPTY(&hwpt->device_list)) {
        g_assert_not_reached();
    }
    QLIST_REMOVE(hwpt, next);
    g_free(hwpt);
}

static VFIOIOASHwpt *vfio_find_hwpt_for_dev(VFIOIOMMUFDContainer *container,
                                            VFIODevice *vbasedev)
{
    VFIOIOASHwpt *hwpt;
    VFIODevice *vbasedev_iter;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        QLIST_FOREACH(vbasedev_iter, &hwpt->device_list, hwpt_next) {
            if (vbasedev_iter == vbasedev) {
                return hwpt;
            }
        }
    }
    return NULL;
}

static void vfio_kvm_device_add_device(VFIODevice *vbasedev)
{
    if (vfio_kvm_device_add_fd(vbasedev->fd)) {
        error_report("Failed to add device %s to KVM VFIO device",
                     vbasedev->sysfsdev);
    }
}

static void vfio_kvm_device_del_device(VFIODevice *vbasedev)
{
    if (vfio_kvm_device_del_fd(vbasedev->fd)) {
        error_report("Failed to del device %s to KVM VFIO device",
                     vbasedev->sysfsdev);
    }
}

static int
__vfio_device_detach_hwpt(VFIODevice *vbasedev, Error **errp)
{
    struct vfio_device_detach_iommufd_pt detach_data = {
        .argsz = sizeof(detach_data),
        .flags = 0,
    };
    int ret;

    ret = ioctl(vbasedev->fd, VFIO_DEVICE_DETACH_IOMMUFD_PT, &detach_data);
    if (ret) {
        ret = -errno;
        error_setg_errno(errp, errno, "detach %s from ioas failed",
                         vbasedev->name);
    }
    return ret;
}

static void
__vfio_device_detach_container(VFIODevice *vbasedev,
                               VFIOIOMMUFDContainer *container, Error **errp)
{
    __vfio_device_detach_hwpt(vbasedev, errp);
    trace_vfio_iommufd_detach_device(container->be->fd, vbasedev->name,
                                     container->ioas_id);
    vfio_kvm_device_del_device(vbasedev);

    /* iommufd unbind is done per device fd close */
}

static void vfio_device_detach_container(VFIODevice *vbasedev,
                                         VFIOIOMMUFDContainer *container,
                                         Error **errp)
{
    VFIOIOASHwpt *hwpt;

    hwpt = vfio_find_hwpt_for_dev(container, vbasedev);
    if (hwpt) {
        QLIST_REMOVE(vbasedev, hwpt_next);
        if (QLIST_EMPTY(&hwpt->device_list)) {
            vfio_container_put_hwpt(hwpt);
        }
    }

    __vfio_device_detach_container(vbasedev, container, errp);
}

static int vfio_device_attach_container(VFIODevice *vbasedev,
                                        VFIOIOMMUFDContainer *container,
                                        Error **errp)
{
    struct vfio_device_bind_iommufd bind = {
        .argsz = sizeof(bind),
        .flags = 0,
        .iommufd = container->be->fd,
        .dev_cookie = (uint64_t)vbasedev,
    };
    struct vfio_device_attach_iommufd_pt attach_data = {
        .argsz = sizeof(attach_data),
        .flags = 0,
        .pt_id = container->ioas_id,
    };
    VFIOIOASHwpt *hwpt;
    uint32_t hwpt_id;
    int ret;

    /*
     * Add device to kvm-vfio to be prepared for the tracking
     * in KVM. Especially for some emulated devices, it requires
     * to have kvm information in the device open.
     */
    vfio_kvm_device_add_device(vbasedev);

    /* Bind device to iommufd */
    ret = ioctl(vbasedev->fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
    if (ret) {
        vfio_kvm_device_del_device(vbasedev);
        error_setg_errno(errp, errno, "error bind device fd=%d to iommufd=%d",
                         vbasedev->fd, bind.iommufd);
        return ret;
    }

    vbasedev->devid = bind.out_devid;
    trace_vfio_iommufd_bind_device(bind.iommufd, vbasedev->name,
                                   vbasedev->fd, vbasedev->devid);

    /* Allocate and attach device to a default hwpt */
    ret = iommufd_backend_alloc_hwpt(bind.iommufd, vbasedev->devid,
                                     container->ioas_id,
                                     container->nested_data.type,
                                     container->nested_data.len,
                                     container->nested_data.ptr, &hwpt_id);
    if (ret) {
        error_setg_errno(errp, errno, "error alloc nested S2 hwpt");
        return ret;
    }

    attach_data.pt_id = hwpt_id;
    /* Attach device to an ioas within iommufd */
    ret = ioctl(vbasedev->fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
    if (ret) {
        vfio_kvm_device_del_device(vbasedev);
        error_setg_errno(errp, errno,
                         "[iommufd=%d] error attach %s (%d) to ioasid=%d",
                         container->be->fd, vbasedev->name, vbasedev->fd,
                         attach_data.pt_id);
    }

    trace_vfio_iommufd_attach_device(bind.iommufd, vbasedev->name,
                                     vbasedev->fd, container->ioas_id,
                                     attach_data.pt_id);

    hwpt = vfio_container_get_hwpt(container, attach_data.pt_id);
    if (!hwpt) {
        __vfio_device_detach_container(vbasedev, container, errp);
        return -1;
    }

    QLIST_INSERT_HEAD(&hwpt->device_list, vbasedev, hwpt_next);
    return 0;
}

static int vfio_device_reset(VFIODevice *vbasedev)
{
    if (vbasedev->dev->realized) {
        vbasedev->ops->vfio_compute_needs_reset(vbasedev);
        if (vbasedev->needs_reset) {
            return vbasedev->ops->vfio_hot_reset_multi(vbasedev);
        }
    }
    return 0;
}

static int vfio_iommufd_container_reset(VFIOContainer *bcontainer)
{
    VFIOIOMMUFDContainer *container;
    int ret, final_ret = 0;
    VFIODevice *vbasedev;
    VFIOIOASHwpt *hwpt;

    container = container_of(bcontainer, VFIOIOMMUFDContainer, bcontainer);

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        QLIST_FOREACH(vbasedev, &hwpt->device_list, hwpt_next) {
            ret = vfio_device_reset(vbasedev);
            if (ret) {
                error_report("failed to reset %s (%d)", vbasedev->name, ret);
                final_ret = ret;
            } else {
                trace_vfio_iommufd_container_reset(vbasedev->name);
            }
        }
    }
    return final_ret;
}

static void vfio_iommufd_container_destroy(VFIOIOMMUFDContainer *container)
{
    vfio_container_destroy(&container->bcontainer);
    g_free(container);
}

static int vfio_ram_block_discard_disable(bool state)
{
    /*
     * We support coordinated discarding of RAM via the RamDiscardManager.
     */
    return ram_block_uncoordinated_discard_disable(state);
}

static void iommufd_detach_device(VFIODevice *vbasedev);

static int iommufd_attach_device(VFIODevice *vbasedev, AddressSpace *as,
                                 Error **errp)
{
    VFIOIOMMUBackendOpsClass *ops = VFIO_IOMMU_BACKEND_OPS_CLASS(
        object_class_by_name(TYPE_VFIO_IOMMU_BACKEND_IOMMUFD_OPS));
    VFIOContainer *bcontainer;
    VFIOIOMMUFDContainer *container;
    VFIOAddressSpace *space;
    IOMMUFDDevice *idev = &vbasedev->idev;
    struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
    VFIOIOASHwpt *hwpt;
    int ret, devfd;
    uint32_t ioas_id;
    Error *err = NULL;

    devfd = vfio_get_devicefd(vbasedev->sysfsdev, errp);
    if (devfd < 0) {
        return devfd;
    }
    vbasedev->fd = devfd;

    space = vfio_get_address_space(as);

    /* try to attach to an existing container in this space */
    QLIST_FOREACH(bcontainer, &space->containers, next) {
        if (bcontainer->ops != ops) {
            continue;
        }
        container = container_of(bcontainer, VFIOIOMMUFDContainer, bcontainer);
        if (vfio_device_attach_container(vbasedev, container, &err)) {
            const char *msg = error_get_pretty(err);

            trace_vfio_iommufd_fail_attach_existing_container(msg);
            error_free(err);
            err = NULL;
        } else {
            ret = vfio_ram_block_discard_disable(true);
            if (ret) {
                vfio_device_detach_container(vbasedev, container, &err);
                error_propagate(errp, err);
                vfio_put_address_space(space);
                close(vbasedev->fd);
                error_prepend(errp,
                              "Cannot set discarding of RAM broken (%d)", ret);
                return ret;
            }
            goto out;
        }
    }

    /* Need to allocate a new dedicated container */
    ret = iommufd_backend_get_ioas(vbasedev->iommufd, &ioas_id);
    if (ret < 0) {
        vfio_put_address_space(space);
        close(vbasedev->fd);
        error_report("Failed to alloc ioas (%s)", strerror(errno));
        return ret;
    }

    trace_vfio_iommufd_alloc_ioas(vbasedev->iommufd->fd, ioas_id);

    container = g_malloc0(sizeof(*container));
    container->be = vbasedev->iommufd;
    container->ioas_id = ioas_id;
    QLIST_INIT(&container->hwpt_list);

    bcontainer = &container->bcontainer;
    vfio_container_init(bcontainer, space, ops);

    if (memory_region_is_iommu(as->root)) {
        IOMMUMemoryRegion *iommu_mr = IOMMU_MEMORY_REGION(as->root);

        ret = memory_region_iommu_get_attr(iommu_mr, IOMMU_ATTR_VFIO_NESTED,
                                           (void *)&bcontainer->nested);
        if (ret) {
            bcontainer->nested = false;
        }
        ret = memory_region_iommu_get_attr(iommu_mr, IOMMU_ATTR_IOMMUFD_DATA,
                                           (void *)&container->nested_data);
        if (ret) {
            container->nested_data.type = IOMMU_HWPT_TYPE_DEFAULT;
            container->nested_data.len = 0;
            container->nested_data.ptr = NULL;
        }
        trace_vfio_iommufd_nested(vbasedev->iommufd->fd,
                                  bcontainer->nested,
                                  container->nested_data.type,
                                  (uint64_t)container->nested_data.ptr);
    }

    ret = vfio_device_attach_container(vbasedev, container, &err);
    if (ret) {
        /* todo check if any other thing to do */
        error_propagate(errp, err);
        vfio_iommufd_container_destroy(container);
        iommufd_backend_put_ioas(vbasedev->iommufd, ioas_id);
        vfio_put_address_space(space);
        close(vbasedev->fd);
        return ret;
    }

    ret = vfio_ram_block_discard_disable(true);
    if (ret) {
        goto error;
    }

    /*
     * TODO: for now iommufd BE is on par with vfio iommu type1, so it's
     * fine to add the whole range as window. For SPAPR, below code
     * should be updated.
     */
    vfio_host_win_add(bcontainer, 0, (hwaddr)-1, sysconf(_SC_PAGE_SIZE));
    bcontainer->pgsizes = sysconf(_SC_PAGE_SIZE);

    if (bcontainer->nested) {
        bcontainer->prereg_listener = vfio_nested_prereg_listener;
        memory_listener_register(&bcontainer->prereg_listener,
                                 &address_space_memory);
        if (bcontainer->error) {
            memory_listener_unregister(&bcontainer->prereg_listener);
            ret = -1;
            error_propagate_prepend(errp, bcontainer->error,
                                "RAM memory listener initialization failed "
                                "for container");
            goto error;
        }
    }

    vfio_as_add_container(space, bcontainer);

    if (bcontainer->error) {
        ret = -1;
        error_propagate_prepend(errp, bcontainer->error,
            "memory listener initialization failed: ");
        goto error;
    }
    bcontainer->initialized = true;

out:
    vbasedev->container = bcontainer;

    /*
     * TODO: examine RAM_BLOCK_DISCARD stuff, should we do group level
     * for discarding incompatibility check as well?
     */
    if (vbasedev->ram_block_discard_allowed) {
        vfio_ram_block_discard_disable(false);
    }

    ret = ioctl(devfd, VFIO_DEVICE_GET_INFO, &dev_info);
    if (ret) {
        error_setg_errno(errp, errno, "error getting device info");
        vfio_as_del_container(space, bcontainer);
        goto error;
    }

    vbasedev->group = 0;
    vbasedev->num_irqs = dev_info.num_irqs;
    vbasedev->num_regions = dev_info.num_regions;
    vbasedev->flags = dev_info.flags;
    vbasedev->reset_works = !!(dev_info.flags & VFIO_DEVICE_FLAGS_RESET);

    hwpt = vfio_find_hwpt_for_dev(container, vbasedev);
    iommufd_device_init(idev, sizeof(*idev), TYPE_VFIO_IOMMU_DEVICE,
                        container->be->fd, vbasedev->devid, ioas_id,
                        hwpt->hwpt_id);
    trace_vfio_iommufd_device_info(vbasedev->name, devfd, vbasedev->num_irqs,
                                   vbasedev->num_regions, vbasedev->flags);
    return 0;
error:
    vfio_device_detach_container(vbasedev, container, &err);
    error_propagate(errp, err);
    vfio_iommufd_container_destroy(container);
    iommufd_backend_put_ioas(vbasedev->iommufd, ioas_id);
    vfio_put_address_space(space);
    close(vbasedev->fd);
    return ret;
}

static void iommufd_detach_device(VFIODevice *vbasedev)
{
    VFIOContainer *bcontainer = vbasedev->container;
    VFIOIOMMUFDContainer *container;
    VFIODevice *vbasedev_iter;
    VFIOIOASHwpt *hwpt;
    VFIOAddressSpace *space;
    Error *err = NULL;

    if (!bcontainer) {
        goto out;
    }

    if (!vbasedev->ram_block_discard_allowed) {
        vfio_ram_block_discard_disable(false);
    }

    container = container_of(bcontainer, VFIOIOMMUFDContainer, bcontainer);
    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        QLIST_FOREACH(vbasedev_iter, &hwpt->device_list, hwpt_next) {
            if (vbasedev_iter == vbasedev) {
                goto found;
            }
        }
    }
    g_assert_not_reached();
found:
    QLIST_REMOVE(vbasedev, hwpt_next);
    if (QLIST_EMPTY(&hwpt->device_list)) {
        vfio_container_put_hwpt(hwpt);
    }

    space = bcontainer->space;
    /*
     * Needs to remove the bcontainer from space->containers list before
     * detach container. Otherwise, detach container may destroy the
     * container if it's the last device. By removing bcontainer from the
     * list, container is disconnected with address space memory listener.
     */
    if (QLIST_EMPTY(&container->hwpt_list)) {
        vfio_as_del_container(space, bcontainer);
    }
    __vfio_device_detach_container(vbasedev, container, &err);
    if (err) {
        error_report_err(err);
    }
    if (QLIST_EMPTY(&container->hwpt_list)) {
        uint32_t ioas_id = container->ioas_id;

        vfio_iommufd_container_destroy(container);
        iommufd_backend_put_ioas(vbasedev->iommufd, ioas_id);
        vfio_put_address_space(space);
    }
    vbasedev->container = NULL;
out:
    close(vbasedev->fd);
    g_free(vbasedev->name);
}

static void vfio_iommu_backend_iommufd_ops_class_init(ObjectClass *oc,
                                                     void *data) {
    VFIOIOMMUBackendOpsClass *ops = VFIO_IOMMU_BACKEND_OPS_CLASS(oc);

    ops->check_extension = iommufd_check_extension;
    ops->dma_map = iommufd_map;
    ops->dma_copy = iommufd_copy;
    ops->dma_unmap = iommufd_unmap;
    ops->attach_device = iommufd_attach_device;
    ops->detach_device = iommufd_detach_device;
    ops->reset = vfio_iommufd_container_reset;
}

static int vfio_iommu_device_attach_hwpt(IOMMUFDDevice *idev,
                                         uint32_t hwpt_id)
{
    VFIODevice *vbasedev = container_of(idev, VFIODevice, idev);
    struct vfio_device_attach_iommufd_pt attach = {
        .argsz = sizeof(attach),
        .flags = 0,
        .pt_id = hwpt_id,
    };
    int ret;

    ret = ioctl(vbasedev->fd, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach);
    if (ret) {
        ret = -errno;
    }

    return ret;
}

static int vfio_iommu_device_detach_hwpt(IOMMUFDDevice *idev)
{
    VFIODevice *vbasedev = container_of(idev, VFIODevice, idev);
    Error *err = NULL;
    int ret;

    ret = __vfio_device_detach_hwpt(vbasedev, &err);
    error_free(err);
    return ret;
}

static void vfio_iommu_device_class_init(ObjectClass *klass,
                                         void *data)
{
    IOMMUFDDeviceClass *idevc = IOMMU_DEVICE_CLASS(klass);

    idevc->attach_hwpt = vfio_iommu_device_attach_hwpt;
    idevc->detach_hwpt = vfio_iommu_device_detach_hwpt;
}

static const TypeInfo vfio_iommu_device_info = {
    .parent = TYPE_IOMMUFD_DEVICE,
    .name = TYPE_VFIO_IOMMU_DEVICE,
    .class_init = vfio_iommu_device_class_init,
};

static void vfio_iommufd_register_types(void)
{
    type_register_static(&vfio_iommu_device_info);
}

type_init(vfio_iommufd_register_types)

static const TypeInfo vfio_iommu_backend_iommufd_ops_type = {
    .name = TYPE_VFIO_IOMMU_BACKEND_IOMMUFD_OPS,

    .parent = TYPE_VFIO_IOMMU_BACKEND_OPS,
    .class_init = vfio_iommu_backend_iommufd_ops_class_init,
    .abstract = true,
};
static void vfio_iommu_backend_iommufd_ops_register_types(void)
{
    type_register_static(&vfio_iommu_backend_iommufd_ops_type);
}
type_init(vfio_iommu_backend_iommufd_ops_register_types);
