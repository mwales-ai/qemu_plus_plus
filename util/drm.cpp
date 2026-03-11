/*
 * Copyright (C) 2015-2016 Gerd Hoffmann <kraxel@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
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

extern "C" {
#include "qemu/drm.h"
}

#include <glob.h>
#include <dirent.h>
#include <string>

extern "C"
int qemu_drm_rendernode_open(const char *rendernode)
{
    DIR *dir;
    struct dirent *e;
    struct stat st;
    int r, fd, ret;

    if (rendernode) {
        return open(rendernode, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    }

    dir = opendir("/dev/dri");
    if (!dir) {
        return -1;
    }

    fd = -1;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "renderD", 7)) {
            continue;
        }

        /* std::string replaces g_strdup_printf + g_free */
        std::string path = std::string("/dev/dri/") + e->d_name;

        r = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
        if (r < 0) {
            continue;
        }

        /*
         * prefer fstat() over checking e->d_type == DT_CHR for
         * portability reasons
         */
        ret = fstat(r, &st);
        if (ret < 0 || (st.st_mode & S_IFMT) != S_IFCHR) {
            close(r);
            continue;
        }

        fd = r;
        break;
    }

    closedir(dir);
    if (fd < 0) {
        return -1;
    }
    return fd;
}
