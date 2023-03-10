#!/bin/bash
set -e

arch=arm
download=0
build_ucx=1
build_ucc=1
build_mpi=1
build_omb=0

WORK_DIR='/global/scratch/users/dpu/deploy'
BUILD_DIR="$WORK_DIR/build-$arch"

UCX_SRC="$WORK_DIR/ucx"
UCX_DIR="$BUILD_DIR/ucx"
UCX_URL='https://github.com/openucx/ucx.git'
UCX_BRANCH='master'

UCC_SRC="$WORK_DIR/ucc"
UCC_DIR="$BUILD_DIR/ucc"
UCC_URL='https://github.com/Mellanox/ucc.git'
UCC_BRANCH='v0.4.x'

MPI_SRC="$WORK_DIR/ompi"
MPI_DIR="$BUILD_DIR/ompi"
MPI_URL='https://github.com/open-mpi/ompi.git'
MPI_BRANCH='v5.0.x'

OMB_SRC="$WORK_DIR/omb"
OMB_DIR="$BUILD_DIR/omb"
OMB_URL='https://github.com/paklui/osu-micro-benchmarks.git'
OMB_BRANCH='master'

echo "#### WORK DIR  : $WORK_DIR  ####"
echo "#### BUILD DIR : $BUILD_DIR ####"
cd $WORK_DIR

if [ $download -eq 1 ]; then
    if [ ! -d $UCX_SRC ]; then
        echo "#### Downloading UCX from $UCX_URL:$UCX_BRANCH ####"
        git clone -b $UCX_BRANCH $UCX_URL ucx
    fi
    if [ ! -d $UCC_SRC ]; then
        echo "#### Downloading UCC from $UCC_URL:$UCC_BRANCH ####"
        git clone -b $UCC_BRANCH $UCC_URL ucc
    fi
    if [ ! -d $MPI_SRC ]; then
        echo "#### Downloading MPI from $MPI_URL:$MPI_BRANCH ####"
        git clone -b $MPI_BRANCH $MPI_URL ompi
        git submodule update --init --recursive
    fi
    if [ ! -d $OMB_SRC ]; then
        echo "#### Downloading OMB from $OMB_URL:$OMB_BRANCH ####"
        git clone -b $OMB_BRANCH $OMB_URL omb
    fi
fi

if [ $build_ucx -eq 1 ]; then
    echo "#### Building UCX ####"
	cd $UCX_SRC
	#./autogen.sh
	mkdir -p $UCX_DIR
	cd $UCX_DIR

    echo "#### Confguring UCX ####"
    config_opts="--enable-mt --prefix=$UCX_DIR --without-valgrind --without-cuda"
	$UCX_SRC/contrib/configure-opt -C $config_opts

	make -j install
    echo "#### Done Building UCX ####"
fi

if [ $build_ucc -eq 1 ]; then
    echo "#### Building UCC ####"
	cd $UCC_SRC
	#./autogen.sh
	mkdir -p $UCC_DIR
	cd $UCC_DIR

    echo "#### Confguring UCC ####"
    config_opts="--prefix=$UCC_DIR --with-ucx=$UCX_DIR --with-dpu=no --enable-optimizations --enable-openmp"
	$UCC_SRC/configure -C $config_opts

	make -j install
    echo "#### Done Building UCC ####"

    echo "#### Building DPU Server ####"
    cd $UCC_SRC/contrib/dpu_daemon
    make BUILD_DIR=$BUILD_DIR
fi

if [ $build_mpi -eq 1 ]; then
    echo "#### Building MPI ####"
	cd $MPI_SRC
	#./autogen.pl
	mkdir -p $MPI_DIR
	cd $MPI_DIR

    echo "#### Confguring MPI ####"
    config_opts="--prefix=$MPI_DIR --with-ucx=$UCX_DIR --with-ucc=$UCC_DIR --without-verbs --disable-man-pages --with-pmix=internal --with-hwloc=internal"
	$MPI_SRC/configure -C $config_opts

	make -j install
    echo "#### Done Building MPI ####"
fi

if [ $build_omb -eq 1 ]; then
    echo "#### Building OMB ####"
	cd $OMB_SRC
	#autoreconf -ivf
	mkdir -p $OMB_DIR
	cd $OMB_DIR

    config_opts="--prefix=$OMB_DIR CC=$MPI_DIR/bin/mpicc CXX=$MPI_DIR/bin/mpicxx"
    $OMB_SRC/configure -C $config_opts

	make -j install
    echo "#### Done Building OMB ####"
fi

