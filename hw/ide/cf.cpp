/*
 * ide CompactFlash support
 *
 * This code is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/ide/ide-dev.h"
#include "qapi/qapi-types-block.h"

static void ide_cf_realize(IDEDevice *dev, Error **errp)
{
    ide_dev_initfn(dev, IDE_CFATA, errp);
}

static const Property ide_cf_properties[] = {
    DEFINE_IDE_DEV_PROPERTIES(),
    DEFINE_BLOCK_CHS_PROPERTIES(IDEDrive, dev.conf),
    DEFINE_PROP_BIOS_CHS_TRANS("bios-chs-trans",
                IDEDrive, dev.chs_trans, BIOS_ATA_TRANSLATION_AUTO),
};

struct IDECFState {
    IDEDrive parent_obj;

    static void classInit(DeviceClass *dc);
};

void IDECFState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    IDEDeviceClass *k = IDE_DEVICE_CLASS(klass);

    k->realize  = ide_cf_realize;
    dc->fw_name = "drive";
    dc->desc    = "virtual CompactFlash card";
    device_class_set_props(dc, ide_cf_properties);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(IDECFState, "ide-cf", TYPE_IDE_DEVICE)
