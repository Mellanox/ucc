#!/bin/bash -eE
#
# Configuration for running script ./run.sh to build and test DPU server
#

export WORK_DIR="${WORK_DIR:-/GIT/WORK/${ARCH}}" # Base work dir
export BUILD_DIR="${WORK_DIR}/build"
export SCRIPT_DIR="${PWD}"
export DOWNLOAD="${DOWNLOAD:-1}"
#= The name of IB on the host
export MLX_DEV_HOST="mlx5_4:1"

#= Build components
declare -a BUILD_INDEX=(UCX UCC OMPI OMB DPU)
#= Configuration of build components
declare -A BUILD=(
  [dpu, ${BUILD_INDEX[0]}]=1 #UCX
  [dpu, ${BUILD_INDEX[1]}]=1 #UCC
  [dpu, ${BUILD_INDEX[2]}]=1 #OMPI
  [dpu, ${BUILD_INDEX[3]}]=0 #OMB
  [dpu, ${BUILD_INDEX[4]}]=1 #DPU

  [x86, ${BUILD_INDEX[0]}]=1 #UCX
  [x86, ${BUILD_INDEX[1]}]=1 #UCC
  [x86, ${BUILD_INDEX[2]}]=1 #OMPI
  [x86, ${BUILD_INDEX[3]}]=1 #OMB
  [x86, ${BUILD_INDEX[4]}]=0 #DPU
)

#= List of packages
declare -a PKG_INDEX=(${BUILD_INDEX[@]:0:4})

#= Configurations for 'git clone'
declare -A GIT=(
  #UCX
  [${PKG_INDEX[0]}, URL]='https://github.com/openucx/ucx.git'
  [${PKG_INDEX[0]}, BRANCH]="${UCX_BRANCH:-v1.14.x}"
  [${PKG_INDEX[0]}, SRC]="${WORK_DIR}/${PKG_INDEX[0]}"
  [${PKG_INDEX[0]}, BUILD]="${BUILD_DIR}/${PKG_INDEX[0]}"
  #UCC
  [${PKG_INDEX[1]}, URL]='https://github.com/Mellanox/ucc'
  [${PKG_INDEX[1]}, BRANCH]="${UCC_BRANCH:-v0.4.x}"
  [${PKG_INDEX[1]}, SRC]="${WORK_DIR}/${PKG_INDEX[1]}"
  [${PKG_INDEX[1]}, BUILD]="${BUILD_DIR}/${PKG_INDEX[1]}"
  #OMPI
  [${PKG_INDEX[2]}, URL]='https://github.com/open-mpi/ompi.git'
  [${PKG_INDEX[2]}, BRANCH]="${MPI_BRANCH:-v5.0.x}"
  [${PKG_INDEX[2]}, SRC]="${WORK_DIR}/${PKG_INDEX[2]}"
  [${PKG_INDEX[2]}, BUILD]="${BUILD_DIR}/${PKG_INDEX[2]}"
  #OMB
  [${PKG_INDEX[3]}, URL]='https://github.com/paklui/osu-micro-benchmarks.git'
  [${PKG_INDEX[3]}, BRANCH]="${OMB_BRANCH:-master}"
  [${PKG_INDEX[3]}, SRC]="${WORK_DIR}/${PKG_INDEX[3]}"
  [${PKG_INDEX[3]}, BUILD]="${BUILD_DIR}/${PKG_INDEX[3]}"
)
########### Parameters running DPU`s server
# Default 2 nodes
NP=2
# Default 8 worker threads
NT=8
HOSTFILEDPU="$DIR/hostfile.dpu"
DPU_BIN="${WORK_DIR}/UCC/contrib/dpu_daemon/dpu_master"
#= Options to run DPU server
DPU_CMD="--np ${NP} --map-by ppr:1:node --mca pml ucx --mca btl ^openib,vader --hostfile ${HOSTFILEDPU} --bind-to none --tag-output \
           -x PATH -x LD_LIBRARY_PATH=${GIT[UCC, BUILD]}/lib:$LD_LIBRARY_PATH -x UCX_NET_DEVICES=mlx5_0:1 -x UCX_TLS=self,rc_x -x UCX_MAX_RNDV_RAILS=1 \
           -x UCC_CL_BASIC_TLS=ucp -x UCC_TL_DPU_PRINT_SUMMARY=1 -x UCC_TL_DPU_NUM_THREADS=${NT} -x UCC_INTERNAL_OOB=1 ${DPU_BIN} "
########### Test`s parameters
BSZ=$((4))
ESZ=$((128 * 1024 * 1024))
MEMSZ=$((16 * 1024 * 1024 * 1024))
ITER=200
WARM=20
HOSTS=2
PPN=1
NPROCS=$(($HOSTS * $PPN))
NBUF=7
BUFSZ=$((128 * 1024))
HOSTFILECPU="${DIR}/hostfile.cpu"
HOST2DPUFILE="${DIR}/host_to_dpu.list"
#= List of supported tests with options
declare -A OMB_TESTS=(
  [allreduce]="-np ${NPROCS} --bind-to none --map-by ppr:$PPN:node --hostfile ${HOSTFILECPU} --mca pml ucx --mca btl ^openib,vader --mca opal_common_ucx_opal_mem_hooks 1 \
               --mca coll_ucc_enable 1 --mca coll_ucc_priority 100 --mca coll_ucc_verbose 0 \
               -x UCC_TL_DPU_TUNE=0-64:0 -x UCC_TL_SHARP_TUNE=0 -x UCC_LOG_LEVEL=warn -x UCC_CL_BASIC_TLS=ucp,dpu -x UCC_TL_DPU_PIPELINE_BLOCK_SIZE=$BUFSZ \
               -x UCC_TL_DPU_PIPELINE_BUFFERS=$NBUF -x UCC_TL_DPU_HOST_DPU_LIST=${HOST2DPUFILE} -x UCX_NET_DEVICES=${MLX_DEV_HOST} -x UCX_TLS=self,rc_x -x UCX_MAX_RNDV_RAILS=1 \
               -x LD_LIBRARY_PATH=${GIT[UCC, BUILD]}/lib:$LD_LIBRARY_PATH ${GIT[OMB, BUILD]}/c/mpi/collective/osu_allreduce -i $ITER -x $WARM -M $MEMSZ -m $BSZ:$ESZ -f"

  [alltoall]="-np ${NPROCS} --map-by ppr:1:node --hostfile ${HOSTFILECPU} --mca pml ucx --mca btl ^openib,vader --mca opal_common_ucx_opal_mem_hooks 1 \
              --mca coll_ucc_enable 1 --mca coll_ucc_priority 100 --mca coll_ucc_verbose 0 \
              -x UCC_TL_DPU_TUNE=0-64K:0 -x UCC_LOG_LEVEL=warn -x UCC_CL_BASIC_TLS=ucp,dpu -x UCC_TL_DPU_PIPELINE_BLOCK_SIZE=1048576 -x UCC_TL_DPU_HOST_DPU_LIST=${HOST2DPUFILE} \
              -x UCX_NET_DEVICES=${MLX_DEV_HOST} -x UCX_TLS=rc_x ${GIT[OMB, BUILD]}/c/mpi/collective/osu_alltoall -i 100 -x 20 -m 1048576:134217728"
)
########## GA formating
# GA checker
GH="${GH:-0}"
if [ "${GH}" -eq 1 ]; then
  # Github acctions formating fold/unfold

  GH_FOLD="::group::"
  GH_UNFOLD="::endgroup::"
fi
