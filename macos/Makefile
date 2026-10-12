# qemu-guest-agent: QEMU guest agent for Mac OS X, PowerPC (10.0-10.5) and Intel (10.4-10.15).
#
# Builds natively on the host for development (`make test`) and inside a guest
# with that release's own Developer Tools for deployment:
#
#   make ARCH_FLAGS="-arch ppc"       # on a PPC guest the default arch already is ppc
#   make intel                        # on 10.6 with Xcode 3.2.6: one binary for 10.4-10.15
#
# Source is C89 + `long long` (GNU extension, accepted by GCC 2.95); keep it so.

CC        ?= cc
ARCH_FLAGS ?=
OPT       ?= -Os
# Flags are probed because GCC 2.95 (10.0/10.1) and 3.1 (10.2) reject newer ones
# (-Wextra, -Wdeclaration-after-statement); -W is the old spelling of -Wextra.
# (GCC 2.95 only warns about unknown options and exits 0, so judge by silence.)
cc_opt = $(shell echo 'int x;' > .probe.c; if [ -z "`$(CC) $(1) -c -o .probe.o .probe.c 2>&1`" ]; then echo $(1); fi; rm -f .probe.c .probe.o)
STD       := $(call cc_opt,-std=gnu89)
WARN      := -Wall -W $(call cc_opt,-Wdeclaration-after-statement)
CFLAGS    += $(STD) $(WARN) $(OPT) $(ARCH_FLAGS)
FRAMEWORKS ?= -framework IOKit -framework CoreFoundation
LDFLAGS   += $(ARCH_FLAGS) $(FRAMEWORKS)

SRCS = main.c selftest.c \
       core/util.c core/json.c core/base64.c core/dispatch.c \
       commands/protocol.c commands/file.c commands/exec.c commands/system.c commands/hw.c commands/freeze.c commands/ssh.c commands/user.c commands/stat.c \
       install.c transport/tty.c transport/serve.c transport/vsock.c transport/logstream.c
OBJS = $(SRCS:.c=.o)

all: qemu-guest-agent

qemu-guest-agent: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# The Intel releases' one binary, i386 and x86_64, built on Snow Leopard with Xcode 3.2.6 from its
# DVD and the 10.4 SDK it carries (guest/software/qemu-guest-agent/README.md's "Intel"). i386 runs
# on 10.4 to 10.14. x86_64 is for 10.15, which runs nothing else -- and for Tiger on a 64-bit CPU,
# whose kernel picks the x86_64 slice of a universal binary over the i386 one
# (mav2287/mac-guest-agent's issue #9). Tiger has an x86_64 libSystem from 10.4.7 but no x86_64
# IOKit or CoreFoundation, so that slice links both weakly and goes without them there
# (commands/hw.c); 10.4 as its minimum keeps the 10.5 SDK's newer symbols out of it. Each slice is compiled in one invocation, so the two share no objects.
INTEL_CC_I386   ?= gcc-4.0
INTEL_CC_X86_64 ?= gcc-4.2
# Xcode.mpkg puts the SDKs in /Developer/SDKs; its packages installed one by one put them in /SDKs.
INTEL_SDKS      ?= $(firstword $(wildcard /Developer/SDKs /SDKs))
INTEL_SDK_I386  ?= $(INTEL_SDKS)/MacOSX10.4u.sdk
INTEL_SDK_X86_64 ?= $(INTEL_SDKS)/MacOSX10.5.sdk
INTEL_CFLAGS     = $(STD) $(WARN) $(OPT) -mmacosx-version-min=10.4

intel: build/intel/qemu-guest-agent

build/intel/qemu-guest-agent: $(SRCS) $(wildcard */*.h *.h)
	mkdir -p build/intel
	$(INTEL_CC_I386) $(INTEL_CFLAGS) -arch i386 -isysroot $(INTEL_SDK_I386) \
		-o build/intel/qemu-guest-agent-i386 $(SRCS) -framework IOKit -framework CoreFoundation
	$(INTEL_CC_X86_64) $(INTEL_CFLAGS) -arch x86_64 -isysroot $(INTEL_SDK_X86_64) \
		-o build/intel/qemu-guest-agent-x86_64 $(SRCS) -weak_framework IOKit -weak_framework CoreFoundation
	lipo -create -output $@ build/intel/qemu-guest-agent-i386 build/intel/qemu-guest-agent-x86_64

test: qemu-guest-agent
	./qemu-guest-agent --self-test
	sh tests/stdio.sh ./qemu-guest-agent

clean:
	rm -rf qemu-guest-agent $(OBJS) build

.PHONY: all intel test clean
