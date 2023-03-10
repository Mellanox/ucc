#
# Copyright (c) 2020 Mellanox Technologies.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

MPI_DIR=$(OMPI_DIR)
#UCX_DIR=$(UCX_DIR)
#UCC_DIR=/global/scratch/users/sourav/build-arm/ucc
UCC_SRC=/global/scratch/users/tomislavj/dpu-dev/ucc
MPICC=$(MPI_DIR)/bin/mpicc
CC=gcc

DBGOPTS = -O0 -g
RELOPTS = -march=native -Ofast -g -DNDEBUG
CFLAGS = -std=c11 -I$(UCC_SRC)/src -I$(UCC_DIR)/include -I$(UCX_DIR)/include
LDFLAGS = -L$(UCC_DIR)/lib $(UCC_DIR)/lib/libucc.so $(UCX_DIR)/lib/libucs.so $(UCX_DIR)/lib/libucp.so -Wl,-rpath -Wl,$(UCC_DIR)/lib -Wl,-rpath -Wl,$(UCX_DIR)/lib -lpthread

rel:
	$(MPICC) $(RELOPTS) $(CFLAGS) -o dpu_server dpu_server.c host_channel.c server_ucc.c $(LDFLAGS)
	$(MPICC) $(RELOPTS) $(CFLAGS) -o dpu_master dpu_master.c $(LDFLAGS)

dbg:
	$(MPICC) $(DBGOPTS) $(CFLAGS) -o dpu_server dpu_server.c host_channel.c server_ucc.c $(LDFLAGS)
	$(MPICC) $(DBGOPTS) $(CFLAGS) -o dpu_master dpu_master.c $(LDFLAGS)

clean:
	rm -f dpu_server dpu_master
