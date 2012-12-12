CFLAGS?=-pipe -O2 -g -Wall -Wextra -Werror \
        -Wbad-function-cast \
        -Wcast-qual \
        -Wcast-align \
        -Wconversion \
        -Wformat-nonliteral \
        -Wformat-security \
        -Winit-self \
        -Wmissing-declarations \
        -Wmissing-include-dirs \
        -Wmissing-format-attribute \
        -Wmissing-prototypes \
        -Wnested-externs \
        -Wpointer-arith \
        -Wredundant-decls \
        -Wshadow \
        -Wstrict-prototypes \
        -Wwrite-strings \
        -Wundef \
        -Wunused \
        -Wno-unused-parameter \
        -Wno-format-zero-length \
        -Wno-format-y2k \
        -Wunsafe-loop-optimizations

RT_LIBS ?= -lrt
EV_LIBS ?= -lev

all: getdomid propose_content get_content waiter notifyer pipe_reader pipe_writer pipe_perf ping_client ping_server bandwidth

test: all
	./getdomid

getdomid: getdomid.o
	$(LINK.c) $^ $(LOADLIBES) -o $@

propose_content: propose_content.o
	$(LINK.c) $^ $(LOADLIBES) -o $@

get_content: get_content.o
	$(LINK.c) $^ $(LOADLIBES) -o $@

waiter: waiter.o
	$(LINK.c) $^ $(LOADLIBES) -o $@
	
notifyer: notifyer.o
	$(LINK.c) $^ $(LOADLIBES) -o $@
	
pipe_reader: pipe_reader.o ../xen_shm_pipe.o
	$(LINK.c) $^ $(LOADLIBES) -o $@
	
pipe_writer: pipe_writer.o ../xen_shm_pipe.o
	$(LINK.c) $^ $(LOADLIBES) -o $@	
	
pipe_perf: pipe_perf.o ../xen_shm_pipe.o
	$(LINK.c) $^ $(LOADLIBES) -o $@	

ping_client: ping_client.o ../client_lib.o ../xen_shm_pipe.o ../handler_lib.o
	$(LINK.c) $^ $(LOADLIBES) $(RT_LIBS) -o $@

ping_server: ping_server.o ../server_lib.o ../xen_shm_pipe.o ../handler_lib.o
	$(LINK.c) $^ $(LOADLIBES) $(RT_LIBS) $(EV_LIBS) -o $@
	
bandwidth: bandwidth.o ../server_lib.o ../client_lib.o ../xen_shm_pipe.o ../handler_lib.o
	$(LINK.c) $^ $(LOADLIBES) $(RT_LIBS) $(EV_LIBS) -o $@
