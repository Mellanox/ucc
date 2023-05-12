#!/bin/bash -eE
# Script to build and test DPU server and dependency
# Input:
#  env $ARCH= dpu or x86
#  ${1} parameter:
#    download - dowloading all components
#    run_dpu  - running dpu server on bf
#    run_omb  - running tests from hosts
#      ${2} parameter:
#        all  - running all tests
#        alltoall - running 'alltoall' test only
#        allreduce - running 'allreduce' only

DIR=$(dirname "$(realpath "$0")")
pushd "${DIR}"
. ./config.sh
echo "#### WORK DIR  : $WORK_DIR  ####"
echo "#### BUILD DIR : $BUILD_DIR ####"
mkdir -p "${WORK_DIR}"
mkdir -p "${BUILD_DIR}"
mkdir -p "${GIT[UCX, SRC]}"
mkdir -p "${GIT[UCC, SRC]}"
mkdir -p "${GIT[OMPI, SRC]}"
mkdir -p "${GIT[OMB, SRC]}"
cd "${WORK_DIR}"

########## function git_clone
#
# Clones repository in accordance with parameters
# input:
#   ${1} package`s name
#   ${2} URL of repository
#   ${3} branch name
#
git_clone() {
    pkg="${1}"
    git_url="${2}"
    branch="${3}"
    if [ ! -d "${GIT[$pkg, $src_dir]}" ]; then
        echo "${GH_FOLD}#### Downloading ${pkg} from ${git_url}:${branch} ####"
        git clone -b "${branch}" "${git_url}" "${pkg}"
        echo "${GH_UNFOLD}"
    fi
    # Additional update submodules for OMPI
    if [ "$pkg" == "${PKG_INDEX[2]}" ]; then
        cd "${pkg}"
        git submodule update --init --recursive
        cd ..
    fi
}

########## function download_all
#
# Runs'git clone' for all packages in ${PKG_INDEX}
#
download_all() {
    if [ "${DOWNLOAD}" -eq 1 ]; then
        for pkg in "${PKG_INDEX[@]}"; do
            git_clone "${pkg}" "${GIT[$pkg, URL]}" "${GIT[$pkg, BRANCH]}"
        done
    fi
}

########## function build
#
# Configures and builds packages
# Input:
#   ${1} package name. See ${PKG_index}
#
build() {
    build_dir="${GIT[$1, BUILD]}"
    src_dir="${GIT[$1, SRC]}"
    pkg_name="${1}"
    cd "${src_dir}"

    case "${pkg_name}" in
    UCX)
        ./autogen.sh
        mkdir -p "${build_dir}"
        cd "${build_dir}"
        echo "#### Configuring ${pkg_name} ####"
        config_opts="--enable-mt --prefix=${build_dir} --without-valgrind --without-cuda"
        "${src_dir}"/contrib/configure-opt -C ${config_opts}
        make -j install
        ;;
    UCC)
        ./autogen.sh
        mkdir -p "${build_dir}"
        cd "${build_dir}"
        echo "#### Configuring ${pkg_name} ####"
        config_opts="--prefix=${build_dir} --with-ucx=${GIT[UCX, BUILD]} ${ARCH_UCC_OPTS}"
        "${src_dir}"/configure -C ${config_opts}
        make -j install
        ;;
    OMPI)
        ./autogen.pl
        mkdir -p "${build_dir}"
        cd "${build_dir}"
        echo "#### Configuring ${pkg_name} ####"
        config_opts="--prefix=${build_dir} --with-ucx=${GIT[UCX, BUILD]} --with-ucc=${GIT[UCC, BUILD]} --without-verbs --disable-man-pages --with-pmix=internal --with-hwloc=internal"
        "${src_dir}"/configure -C ${config_opts}
        make -j install
        ;;
    OMB)
        autoreconf -ivf
        mkdir -p "${build_dir}"
        cd "${build_dir}"
        echo "#### Configuring ${pkg_name} ####"
        config_opts="--prefix=${build_dir} CC=${GIT[OMPI, BUILD]}/bin/mpicc CXX=${GIT[OMPI, BUILD]}/bin/mpicxx"
        "${src_dir}"/configure -C ${config_opts}
        make -j install
        ;;
    DPU)
        cd "${GIT[UCC, SRC]}"/contrib/dpu_daemon
        export OMPI_DIR="${GIT[OMPI, BUILD]}"
        export UCX_DIR="${GIT[UCX, BUILD]}"
        export UCC_DIR="${GIT[UCC, BUILD]}"
        export UCC_SRC="${GIT[UCC, SRC]}"
        make BUILD_DIR="${BUILD_DIR}"
        ;;
    *)
        echo "Do nothing, no such option to build !!!"
        ;;
    esac
}

########### function run_dpu
#
# Runs DPU serve on BF side
#
run_dpu() {
    # Show running command
    set -x
    "${GIT[OMPI, BUILD]}"/bin/mpirun ${DPU_CMD} || handle_exit $?
    set +x
}

########## function handle_exit
#
# Checks exit code 130 (Ctrl+c) and set exit code 0
# Input:
#   ${1} exit code
#
handle_exit() {
    set +x
    exit_code="${1}"
    if [ "${exit_code}" -eq 130 ]; then
        echo "DPU server interrupted correct, exiting"
    else
        echo "DPU server error"
        exit "${exit_code}"
    fi
}

########## function run_omb
#
# Runs OMB tests on x86 side
# Input:
#   ${1} Test`s name. See ${OMB_TESTS}
#
run_omb() {
    echo "${GH_FOLD}##### Running ${1}..."
    set -x
    "${GIT[OMPI, BUILD]}"/bin/mpirun ${OMB_TESTS[$1]}
    set +x
    echo "${GH_UNFOLD}"
}

########## function main
#
# The main function/entrypoint
#
main() {
    case "${ARCH}" in
    x86)
        ARCH_UCC_OPTS="--with-dpu=yes"
        ALTER_ARCH="dpu"
        ;;
    dpu)
        ARCH_UCC_OPTS="--with-dpu=no --enable-optimizations --enable-openmp"
        ALTER_ARCH="x86"
        ;;
    *)
        echo "Platform \$ARCH doesn't set or It's worng. It's mandatory option \$ARCH(x86 or dpu) !!!"
        exit 1
        ;;
    esac

    case "${1}" in
    download)
        download_all
        cp -R "${WORK_DIR}" "${WORK_DIR}"/../"${ALTER_ARCH}"
        cp -f "${DIR}"/../Makefile "${WORK_DIR}"/../dpu/UCC/contrib/dpu_daemon/ # needed while isnot merged
        ;;
    build)
        for pkg in "${BUILD_INDEX[@]}"; do
            if [ "${BUILD[$ARCH, $pkg]}" -eq 1 ]; then
                echo "${GH_FOLD}#### Building ${pkg} ####"
                build "${pkg}"
                echo "${GH_UNFOLD}"
            fi
        done
        ;;
    run_dpu)
        run_dpu
        ;;
    run_omb)
        case "${2}" in
        all)
            run_omb allreduce
            run_omb alltoall
            ;;
        allreduce)
            run_omb allreduce
            ;;
        alltoall)
            run_omb alltoall
            ;;
        *)
            echo "Warning !!! ${2} is not supported. Use allreduce or altoall"
            exit 1
            ;;
        esac
        ;;
    *)
        echo "ERROR,You can use download,build,run_dpu,run_omb options !!!"
        exit 1
        ;;
    esac
}

main "${@}"
popd
