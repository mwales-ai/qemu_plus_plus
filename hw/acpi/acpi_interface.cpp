#include "qemu/osdep.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "hw/acpi/acpi_aml_interface.h"
#include "qemu/module.h"
#include "qemu/queue.h"
#include "qom/cpp/object.h"

void acpi_send_event(DeviceState *dev, AcpiEventStatusBits event)
{
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_GET_CLASS(dev);
    if (adevc->send_event) {
        AcpiDeviceIf *adev = ACPI_DEVICE_IF(dev);
        adevc->send_event(adev, event);
    }
}

void qbus_build_aml(BusState *bus, Aml *scope)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->children, sibling) {
        call_dev_aml_func(DEVICE(kid->child), scope);
    }
}

REGISTER_QEMU_INTERFACE(AcpiDeviceIfClass, TYPE_ACPI_DEVICE_IF)
REGISTER_QEMU_INTERFACE(AcpiDevAmlIfClass, TYPE_ACPI_DEV_AML_IF)
