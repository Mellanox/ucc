FROM nvcr.io/nvidia/doca/doca:1.4.0-devel AS builder

# Set ucx/ucc url and branch
ARG UCX_BRANCH=v1.13.0
ARG UCC_BRANCH=dpu-v0.3.x
ARG UCX_URL=https://github.com/openucx/ucx.git
ARG UCC_URL=https://github.com/Mellanox/ucc.git
ARG SRC=/src
ARG BUILD_DIR=/output

# Install libraries for NVIDIA OFED and UCC server build
RUN set -x && \
    apt-get update && \
    apt-get -y install apt-utils && \
    apt-get install -y --allow-downgrades --allow-change-held-packages --no-install-recommends \ 
        autoconf automake libtool build-essential


RUN mkdir $SRC && mkdir -p $BUILD_DIR/ucx && mkdir -p $BUILD_DIR/ucc
# Build UCX
RUN git clone -b $UCX_BRANCH $UCX_URL $SRC/ucx && cd $SRC/ucx && \
        ./autogen.sh && contrib/configure-opt --enable-mt --prefix=$BUILD_DIR/ucx --without-valgrind --without-cuda && \
        make -j install
# Build UCC
RUN git clone -b $UCC_BRANCH $UCC_URL $SRC/ucc && cd $SRC/ucc && \
        ./autogen.sh && ./configure --prefix=$BUILD_DIR/ucc --with-ucx=$BUILD_DIR/ucx --with-dpu=no --enable-optimizations --enable-openmp && \
        make -j install
# Build dpu server
RUN cd $SRC/ucc/contrib/dpu_daemon && \
    make BUILD_DIR=$BUILD_DIR && \
    cp dpu_server /


FROM nvcr.io/nvidia/doca/doca:1.4.0-full-rt

ARG LIB_PATH=/dpu_libs
# Set dpu server runtime environment
ENV UCX_NET_DEVICES mlx5_0:1
ENV UCX_TLS rc_x
ENV UCC_CL_BASIC_TLS ucp
ENV UCC_MC_CPU_REDUCE_NUM_THREADS 4
ENV UCC_TL_DPU_BCAST_WINDOW 8
ENV LD_LIBRARY_PATH $LIB_PATH/ucc_lib:$LIB_PATH/ucx_lib

COPY --from=builder /output/ucc/lib $LIB_PATH/ucc_lib
COPY --from=builder /output/ucx/lib $LIB_PATH/ucx_lib
COPY --from=builder /src/ucc/contrib/dpu_daemon/dpu_server /dpu_server

WORKDIR /
ENTRYPOINT ["/dpu_server", "1"]