/*
 * common header for iommu devices
 *
 * Copyright Red Hat, Inc. 2019
 *
 * Authors:
 *  Eric Auger <eric.auger@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef QEMU_HW_IOMMU_IOMMU_H
#define QEMU_HW_IOMMU_IOMMU_H
#ifdef __linux__
#include <linux/iommu.h>
#endif

typedef struct IOMMUConfig {
    union {
#ifdef __linux__
        struct iommu_pasid_table_config pasid_cfg;
        struct iommu_inv_pasid_info inv_pasid_info;
#endif
          };
} IOMMUConfig;

typedef struct IOMMUPageResponse {
    union {
#ifdef __linux__
        struct iommu_page_response resp;
#endif
          };
} IOMMUPageResponse;

#include "exec/hwaddr.h"
#include "exec/cpu-common.h"
#include <linux/iommufd.h>

int iommufd_get(void);
void iommufd_put(int fd);
int iommufd_alloc_ioas(int fd, uint32_t *ioas);
void iommufd_free_ioas(int fd, uint32_t ioas);
int iommufd_vfio_ioas(int fd, uint32_t ioas);
int iommufd_unmap_dma(int iommufd, uint32_t ioas, hwaddr iova, ram_addr_t size);
int iommufd_map_dma(int iommufd, uint32_t ioas, hwaddr iova, ram_addr_t size, void *vaddr, bool readonly);

#endif /* QEMU_HW_IOMMU_IOMMU_H */
