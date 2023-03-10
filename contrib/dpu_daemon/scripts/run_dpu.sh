#!/bin/bash

# Default 2 nodes
NP=${1:-2}

# Default 8 worker threads
NT=${2:-8}

WORKSPACE="/global/scratch/users/tomislavj/dpu-dev"
hostfile="$PWD/hostfile.dpu"
MPI_DIR=$OMPI_DIR
UCC_DIR="$UCC_DIR"
DPU_BIN="$WORKSPACE/ucc/contrib/dpu_daemon/dpu_master"

cmd="$MPI_DIR/bin/mpirun --np ${NP} \
     --map-by ppr:1:node \
     --mca pml ucx \
     --mca btl '^openib,vader' \
     --hostfile ${hostfile} \
     --bind-to none \
     --tag-output \
     -x PATH -x LD_LIBRARY_PATH=$UCC_DIR/lib:$LD_LIBRARY_PATH \
     -x UCX_NET_DEVICES=mlx5_0:1 \
     -x UCX_TLS=self,rc_x \
     -x UCX_MAX_RNDV_RAILS=1 \
     -x UCC_CL_BASIC_TLS=ucp \
     -x UCC_TL_DPU_PRINT_SUMMARY=1 \
     -x UCC_TL_DPU_NUM_THREADS=${NT} \
     -x UCC_INTERNAL_OOB=1 \
     ${DPU_BIN} "

echo $cmd
eval "$cmd"

