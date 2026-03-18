#ifndef MICROBLAZE_BOOT_H
#define MICROBLAZE_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

void microblaze_load_kernel(MicroBlazeCPU *cpu, bool is_little_endian,
                            hwaddr ddr_base, uint32_t ramsize,
                            const char *initrd_filename,
                            const char *dtb_filename,
                            void (*machine_cpu_reset)(MicroBlazeCPU *));

#ifdef __cplusplus
}
#endif

#endif /* MICROBLAZE_BOOT_H */
