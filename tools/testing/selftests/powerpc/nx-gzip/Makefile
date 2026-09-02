TEST_GEN_FILES := gzfht_test gunz_test
TEST_GEN_PROGS := nx_gzip_bench nx_gzip_stress
TEST_PROGS := nx-gzip-test.sh

include ../../lib.mk
include ../flags.mk

CFLAGS = -O3 -m64 -I./include -I../include

$(TEST_GEN_FILES): gzip_vas.c ../utils.c

# The benchmark reports through the kselftest harness, so unlike the two
# tools above it is run directly rather than by the shell script. zlib is
# the independent reference its checksum assertions are made against.
$(TEST_GEN_PROGS): gzip_vas.c ../utils.c ../harness.c
$(OUTPUT)/nx_gzip_bench: CFLAGS += -DGIT_VERSION='"$(GIT_VERSION)"'
$(OUTPUT)/nx_gzip_bench: LDLIBS += -lz

# The stress test reports through the harness too, and reads zlib both as the
# reference for the engine's checksums and as the producer of the multi-block
# streams it cuts apart.
$(OUTPUT)/nx_gzip_stress: CFLAGS += -DGIT_VERSION='"$(GIT_VERSION)"'
$(OUTPUT)/nx_gzip_stress: LDLIBS += -lz
