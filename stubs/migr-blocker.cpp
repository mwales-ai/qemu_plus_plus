#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "migration/blocker.h"


int migrate_add_blocker(Error **reasonp, Error **errp)
{
    return 0;
}

int migrate_add_blocker_normal(Error **reasonp, Error **errp)
{
    return 0;
}

int migrate_add_blocker_modes(Error **reasonp, unsigned modes, Error **errp)
{
    return 0;
}

void migrate_del_blocker(Error **reasonp)
{
}


} /* extern "C" */
