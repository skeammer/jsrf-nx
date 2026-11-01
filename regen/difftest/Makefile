# The generated code's own flags (jsrf-switch/CMakeLists.txt, JSRF_GEN_FLAGS).
# Built by run.sh: HERE (this directory), NEW (the rewritten gen tree, whose
# recomp_types.h both versions compile against) and REPO (jsrf-switch).
# RECOMP_NO_ICALL_CACHE: dt_rt.c's lookups log every indirect call, so each
# one has to reach them.
F = -O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off -DRECOMP_MMIO_RUNTIME=1 -DRECOMP_NO_ICALL_CACHE \
    -DRECOMP_MEM_WATCH_RUNTIME=1 -I$(NEW) -I$(REPO)/src -I$(REPO)/xboxrecomp/include
TU = $(patsubst %.c,%.o,$(wildcard t_*.c))

dt: $(TU) dt_rt.o stubs.o tables.o
	$(CC) -o $@ $^ -lm
dt_rt.o: $(HERE)/dt_rt.c
	$(CC) $(F) -c $< -o $@
stubs.o: stubs.c
	$(CC) -O1 -c $< -o $@
tables.o: tables.c
	$(CC) -c $< -o $@
t_%.o: t_%.c
	$(CC) $(F) -c $< -o $@
