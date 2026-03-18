/*
 * Copyright (c) 2025 Intel Corporation
 * Author: Isaku Yamahata <isaku.yamahata at gmail.com>
 *                        <isaku.yamahata at intel.com>
 *         Xiaoyao Li <xiaoyao.li@intel.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "standard-headers/uefi/uefi.h"
#include "hw/pci/pcie_host.h"
#include "tdvf-hob.h"

typedef struct TdvfHob {
    hwaddr hob_addr;
    uint8_t *ptr;
    int size;

    /* working area */
    uint8_t *current;
    uint8_t *end;
} TdvfHob;

static uint64_t tdvf_current_guest_addr(const TdvfHob *hob)
{
    return hob->hob_addr + (hob->current - hob->ptr);
}

static void tdvf_align(TdvfHob *hob, size_t align)
{
    hob->current = static_cast<uint8_t *>(QEMU_ALIGN_PTR_UP(hob->current, align));
}

static void *tdvf_get_area(TdvfHob *hob, uint64_t size)
{
    void *ret;

    if (hob->current + size > hob->end) {
        error_report("TD_HOB overrun, size = 0x%" PRIx64, size);
        exit(1);
    }

    ret = hob->current;
    hob->current += size;
    tdvf_align(hob, 8);
    return ret;
}

static void tdvf_hob_add_memory_resources(TdxGuest *tdx, TdvfHob *hob)
{
    EFI_HOB_RESOURCE_DESCRIPTOR *region;
    EFI_RESOURCE_ATTRIBUTE_TYPE attr;
    EFI_RESOURCE_TYPE resource_type;

    TdxRamEntry *e;
    int i;

    for (i = 0; i < tdx->nr_ram_entries; i++) {
        e = &tdx->ram_entries[i];

        if (e->type == TDX_RAM_UNACCEPTED) {
            resource_type = EFI_RESOURCE_MEMORY_UNACCEPTED;
            attr = EFI_RESOURCE_ATTRIBUTE_TDVF_UNACCEPTED;
        } else if (e->type == TDX_RAM_ADDED) {
            resource_type = EFI_RESOURCE_SYSTEM_MEMORY;
            attr = EFI_RESOURCE_ATTRIBUTE_TDVF_PRIVATE;
        } else {
            error_report("unknown TDX_RAM_ENTRY type %d", e->type);
            exit(1);
        }

        region = static_cast<EFI_HOB_RESOURCE_DESCRIPTOR *>(
            tdvf_get_area(hob, sizeof(*region)));
        memset(region, 0, sizeof(*region));
        region->Header.HobType = EFI_HOB_TYPE_RESOURCE_DESCRIPTOR;
        region->Header.HobLength = cpu_to_le16(sizeof(*region));
        region->Header.Reserved = cpu_to_le32(0);
        region->Owner = EFI_HOB_OWNER_ZERO;
        region->ResourceType = cpu_to_le32(resource_type);
        region->ResourceAttribute = cpu_to_le32(attr);
        region->PhysicalStart = cpu_to_le64(e->address);
        region->ResourceLength = cpu_to_le64(e->length);
    }
}

void tdvf_hob_create(TdxGuest *tdx, TdxFirmwareEntry *td_hob)
{
    TdvfHob hob;
    memset(&hob, 0, sizeof(hob));
    hob.hob_addr = td_hob->address;
    hob.size = td_hob->size;
    hob.ptr = static_cast<uint8_t *>(td_hob->mem_ptr);
    hob.current = static_cast<uint8_t *>(td_hob->mem_ptr);
    hob.end = static_cast<uint8_t *>(td_hob->mem_ptr) + td_hob->size;

    EFI_HOB_GENERIC_HEADER *last_hob;
    EFI_HOB_HANDOFF_INFO_TABLE *hit;

    /* Note, Efi{Free}Memory{Bottom,Top} are ignored, leave 'em zeroed. */
    hit = static_cast<EFI_HOB_HANDOFF_INFO_TABLE *>(
        tdvf_get_area(&hob, sizeof(*hit)));
    memset(hit, 0, sizeof(*hit));
    hit->Header.HobType = EFI_HOB_TYPE_HANDOFF;
    hit->Header.HobLength = cpu_to_le16(sizeof(*hit));
    hit->Header.Reserved = cpu_to_le32(0);
    hit->Version = cpu_to_le32(EFI_HOB_HANDOFF_TABLE_VERSION);
    hit->BootMode = cpu_to_le32(0);
    hit->EfiMemoryTop = cpu_to_le64(0);
    hit->EfiMemoryBottom = cpu_to_le64(0);
    hit->EfiFreeMemoryTop = cpu_to_le64(0);
    hit->EfiFreeMemoryBottom = cpu_to_le64(0);
    hit->EfiEndOfHobList = cpu_to_le64(0); /* initialized later */

    tdvf_hob_add_memory_resources(tdx, &hob);

    last_hob = static_cast<EFI_HOB_GENERIC_HEADER *>(
        tdvf_get_area(&hob, sizeof(*last_hob)));
    memset(last_hob, 0, sizeof(*last_hob));
    last_hob->HobType = EFI_HOB_TYPE_END_OF_HOB_LIST;
    last_hob->HobLength = cpu_to_le16(sizeof(*last_hob));
    last_hob->Reserved = cpu_to_le32(0);
    hit->EfiEndOfHobList = tdvf_current_guest_addr(&hob);
}
