/*
 * Block utility functions
 *
 * Copyright IBM, Corp. 2011
 * Copyright (c) 2020 Coiby Xu <coiby.xu@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

extern "C" {
#include "qapi/error.h"
#include "block-helpers.h"
}

extern "C"
bool check_block_size(const char *name, int64_t value, Error **errp)
{
    if (!value) {
        /* unset */
        return true;
    }

    if (value < MIN_BLOCK_SIZE || value > MAX_BLOCK_SIZE
        || (value & (value - 1))) {
        error_setg(errp,
                   "parameter %s must be a power of 2 between %" PRId64
                   " and %" PRId64,
                   name, MIN_BLOCK_SIZE, MAX_BLOCK_SIZE);
        return false;
    }
    return true;
}
