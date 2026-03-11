/*
 * Simple transactions API
 *
 * Copyright (c) 2021 Virtuozzo International GmbH.
 *
 * Author:
 *  Sementsov-Ogievskiy Vladimir <vsementsov@virtuozzo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/transactions.h"
#include "qemu/queue.h"
}

typedef struct TransactionAction {
    TransactionActionDrv *drv;
    void *opaque;
    QSLIST_ENTRY(TransactionAction) entry;
} TransactionAction;

struct Transaction {
    QSLIST_HEAD(, TransactionAction) actions;
};

extern "C"
Transaction *tran_new(void)
{
    Transaction *tran = g_new(Transaction, 1);

    QSLIST_INIT(&tran->actions);

    return tran;
}

extern "C"
void tran_add(Transaction *tran, TransactionActionDrv *drv, void *opaque)
{
    TransactionAction *act;

    act = g_new(TransactionAction, 1);
    act->drv = drv;
    act->opaque = opaque;

    QSLIST_INSERT_HEAD(&tran->actions, act, entry);
}

extern "C"
void tran_abort(Transaction *tran)
{
    TransactionAction *act, *next;

    QSLIST_FOREACH(act, &tran->actions, entry) {
        if (act->drv->abort) {
            act->drv->abort(act->opaque);
        }
    }

    QSLIST_FOREACH_SAFE(act, &tran->actions, entry, next) {
        if (act->drv->clean) {
            act->drv->clean(act->opaque);
        }

        g_free(act);
    }

    g_free(tran);
}

extern "C"
void tran_commit(Transaction *tran)
{
    TransactionAction *act, *next;

    QSLIST_FOREACH(act, &tran->actions, entry) {
        if (act->drv->commit) {
            act->drv->commit(act->opaque);
        }
    }

    QSLIST_FOREACH_SAFE(act, &tran->actions, entry, next) {
        if (act->drv->clean) {
            act->drv->clean(act->opaque);
        }

        g_free(act);
    }

    g_free(tran);
}
