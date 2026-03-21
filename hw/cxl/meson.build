system_ss.add(when: 'CONFIG_CXL',
               if_true: files(
                   'cxl-component-utils.c',
                   'cxl-device-utils.c',
                   'cxl-mailbox-utils.c',
                   'cxl-host.cpp',
                   'cxl-cdat.cpp',
                   'cxl-events.cpp',
                   'switch-mailbox-cci.cpp',
               ),
               if_false: files(
                   'cxl-host-stubs.cpp',
               ))
