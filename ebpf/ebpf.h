/*
 * QEMU eBPF binary declaration routine.
 *
 * Developed by Daynix Computing LTD (http://www.daynix.com)
 *
 * Authors:
 *  Andrew Melnychenko <andrew@daynix.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef EBPF_H
#define EBPF_H

#ifdef __cplusplus
extern "C" {
#endif


void ebpf_register_binary_data(int id, const void *data,
                               size_t datalen);
const void *ebpf_find_binary_by_id(int id, size_t *sz,
                                   struct Error **errp);

#define ebpf_binary_init(id, fn)                                           \
static void __attribute__((constructor)) ebpf_binary_init_ ## fn(void)     \
{                                                                          \
    size_t datalen = 0;                                                    \
    const void *data = fn(&datalen);                                       \
    ebpf_register_binary_data(id, data, datalen);                          \
}

#ifdef __cplusplus
}
#endif

#endif /* EBPF_H */
