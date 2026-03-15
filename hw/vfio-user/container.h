/*
 * vfio-user specific definitions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_VFIO_USER_CONTAINER_H
#define HW_VFIO_USER_CONTAINER_H


#include "hw/vfio/vfio-container.h"
#include "hw/vfio-user/proxy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MMU container sub-class for vfio-user. */
struct VFIOUserContainer {
    VFIOContainer parent_obj;

    VFIOUserProxy *proxy;
};

OBJECT_DECLARE_SIMPLE_TYPE(VFIOUserContainer, VFIO_IOMMU_USER);

#ifdef __cplusplus
}
#endif

#endif /* HW_VFIO_USER_CONTAINER_H */
