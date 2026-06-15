import os

# toolchains options
ARCH = 'arm'
CPU = 'cortex-a'
CROSS_TOOL = 'gcc'
PLATFORM = 'gcc'
EXEC_PATH = os.getenv('RTT_EXEC_PATH') or '/usr/bin'
BUILD = 'debug'

LINK_SCRIPT = 'board/link_boot.lds'

if PLATFORM == 'gcc':
    PREFIX = os.getenv('RTT_CC_PREFIX') or 'arm-none-eabi-'
    CC = PREFIX + 'gcc'
    CXX = PREFIX + 'g++'
    AS = PREFIX + 'gcc'
    AR = PREFIX + 'ar'
    LINK = PREFIX + 'gcc'
    TARGET_EXT = 'elf'
    SIZE = PREFIX + 'size'
    OBJCOPY = PREFIX + 'objcopy'
    CFPFLAGS = ' -msoft-float'
    AFPFLAGS = ' -mfloat-abi=softfp -mfpu=neon'
    DEVICE = ' -march=armv7-a -mtune=cortex-a7 -ftree-vectorize -ffast-math -funwind-tables -fno-strict-aliasing'
    CXXFLAGS = DEVICE + CFPFLAGS + ' -Wall -fdiagnostics-color=always'
    CFLAGS = DEVICE + CFPFLAGS + ' -Wall -Wno-cpp -std=gnu99 -D_POSIX_SOURCE -fdiagnostics-color=always'
    AFLAGS = DEVICE + ' -c' + AFPFLAGS + ' -x assembler-with-cpp'
    LFLAGS = DEVICE + ' -Wl,--gc-sections,-Map=qboot.map,-cref,-u,system_vectors -T ' + LINK_SCRIPT + ' -lsupc++ -lgcc -static'
    if BUILD == 'debug':
        CFLAGS += ' -O0 -gdwarf-2'
        CXXFLAGS += ' -O0 -gdwarf-2'
        AFLAGS += ' -gdwarf-2'
    else:
        CFLAGS += ' -Os'
        CXXFLAGS += ' -Os'
    POST_ACTION = OBJCOPY + ' -O binary $TARGET qboot.bin\n' + SIZE + ' $TARGET \n'
