# If possible, add new files to other directories, by using "if_false".
# If you need them here, try to add them under one of the if statements
# below, so that it is clear who needs the stubbed functionality.

stub_ss.add(files('cpu-get-clock.cpp'))
stub_ss.add(files('error-printf.cpp'))
stub_ss.add(files('fdset.cpp'))
stub_ss.add(files('iothread-lock.cpp'))
stub_ss.add(files('is-daemonized.cpp'))
stub_ss.add(files('monitor-core.cpp'))
stub_ss.add(files('replay-mode.cpp'))
stub_ss.add(files('trace-control.cpp'))

if have_block
  stub_ss.add(files('bdrv-next-monitor-owned.cpp'))
  stub_ss.add(files('blk-commit-all.cpp'))
  stub_ss.add(files('blk-exp-close-all.cpp'))
  stub_ss.add(files('blockdev-close-all-bdrv-states.cpp'))
  stub_ss.add(files('change-state-handler.cpp'))
  stub_ss.add(files('get-vm-name.cpp'))
  stub_ss.add(files('iothread-lock-block.cpp'))
  stub_ss.add(files('migr-blocker.cpp'))
  stub_ss.add(files('physmem.cpp'))
  stub_ss.add(files('ram-block.cpp'))
  stub_ss.add(files('runstate-check.cpp'))
  stub_ss.add(files('uuid.cpp'))
endif

if have_block or have_ga
  stub_ss.add(files('replay-tools.cpp'))
  # stubs for hooks in util/main-loop.c, util/async.c etc.
  stub_ss.add(files('cpus-virtual-clock.cpp'))
  stub_ss.add(files('icount.cpp'))
  stub_ss.add(files('graph-lock.cpp'))
  if libaio.found()
    stub_ss.add(files('linux-aio.cpp'))
  endif
  stub_ss.add(files('qemu-timer-notify-cb.cpp'))

  # stubs for monitor
  stub_ss.add(files('monitor-internal.cpp'))
  stub_ss.add(files('qmp-command-available.cpp'))
  stub_ss.add(files('qmp-quit.cpp'))
endif

if have_block or have_user
  stub_ss.add(files('qtest.cpp'))
  stub_ss.add(files('vm-stop.cpp'))
  stub_ss.add(files('vmstate.cpp'))
endif

if have_user
  # Symbols that are used by hw/core.
  stub_ss.add(files('cpu-synchronize-state.cpp'))
  stub_ss.add(files('cpu-destroy-address-spaces.cpp'))

  # Stubs for QAPI events.  Those can always be included in the build, but
  # they are not built at all for --disable-system builds.
  if not have_system
    stub_ss.add(files('qdev.cpp'))
  endif

  stub_ss.add(files('monitor-internal.cpp'))
endif

if have_system
  # Symbols that are only needed in some configurations.  Try not
  # adding more of these.  If the symbol is used in specific_ss,
  # in particular, consider defining a preprocessor macro via
  # Kconfig or configs/targets/.
  stub_ss.add(files('dump.cpp'))
  stub_ss.add(files('cmos.cpp'))
  stub_ss.add(files('fw_cfg.cpp'))
  stub_ss.add(files('igvm.cpp'))
  stub_ss.add(files('target-get-monitor-def.cpp'))
  stub_ss.add(files('target-monitor-defs.cpp'))
  stub_ss.add(files('win32-kbd-hook.cpp'))
  stub_ss.add(files('xen-hw-stub.cpp'))
  stub_ss.add(files('monitor-arm-gic.cpp'))
  stub_ss.add(files('monitor-i386-rtc.cpp'))
  stub_ss.add(files('monitor-i386-sev.cpp'))
  stub_ss.add(files('monitor-i386-sgx.cpp'))
  stub_ss.add(files('monitor-i386-xen.cpp'))
  stub_ss.add(files('monitor-cpu.cpp'))
  stub_ss.add(files('monitor-cpu-s390x.cpp'))
  stub_ss.add(files('monitor-cpu-s390x-kvm.cpp'))
endif

if have_system or have_user
  stub_ss.add(files('gdbstub.cpp'))

  # Also included in have_system for --disable-tcg builds
  stub_ss.add(files('replay.cpp'))

  # Also included in have_system for tests/unit/test-qdev-global-props
  stub_ss.add(files('hotplug-stubs.cpp'))
  stub_ss.add(files('sysbus.cpp'))
endif
