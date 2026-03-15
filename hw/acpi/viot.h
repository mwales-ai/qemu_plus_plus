/*
 * ACPI Virtual I/O Translation Table implementation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef VIOT_H
#define VIOT_H

#ifdef __cplusplus
extern "C" {
#endif

void build_viot(MachineState *ms, GArray *table_data, BIOSLinker *linker,
                uint16_t virtio_iommu_bdf, const char *oem_id,
                const char *oem_table_id);

#ifdef __cplusplus
}
#endif

#endif /* VIOT_H */
