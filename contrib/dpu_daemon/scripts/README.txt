#### How to Build Binary ####
0. Make sure there is a shared file system available and mounted on all hosts and dpus
1. Make sure autotools (automake, autoconf, libtool, m4, etc.) are installed
2. Log in to host, edit build_x86.dpu to provide location to build
   This script will download and build UCX, UCC, OpenMPI, and OSU MPI Benchmarks
3. Run ./build_x86.sh , look out for errors during build process
4. Log in to DPU, edit build_arm.sh to correct location to build
5. Run ./build_arm.sh , look out for errors during build process


#### Prerequisites ####
1. Set up passwordless ssh between all hosts and DPUs
2. Edit hostfile.cpu to provide correct CPU/x86 hostnames
3. Edit hostfile.dpu to provide correct BF2/arm hostnames
4. Edit host_to_dpu.list to provide correct HOST->DPU mapping
5. Edit host_to_dpu.list to provide correct NIC/HCA (e.g. mlx5_0:1)
6. Edit run_omb.sh and run_dpu.sh to correct build locations


#### How to Run Binary ####
1. Open two terminals, one for Host and one for DPU
2. ssh to Host and DPU on terminal 1 and 2 respectively
3. Launch run_dpu.sh script on DPU terminal
4. Wait for the following message to appear:
[1,0]<stdout>:DPU daemon: Running with 4 compute threads
[1,1]<stdout>:DPU daemon: Running with 4 compute threads
5. Launch run_omb.sh script on Host terminal

Example:

#### HOST ####                              #### DPU ####
# ssh host-01                               # ssh bf2-01
# cd /nfs/dpu/scripts                       # cd /nfs/dpu/scripts
# ./build_x86.sh                            # ./build_arm.sh
#                                           #
#                                           # ./run_dpu.sh
# ./run_omb.sh

#### How to Build Docker Image ####
0. Log in to DPU, enter contrib\dpu_daemon\scripts directory.
1. Run "docker build . -t ucc_daemon:v1"

#### How to RUN Docker Image ####
1. Open three terminals, one for Host and two for two DPUs
2. ssh to Host on terminal 1, ssh to DPUs on terminal 2 and 3 respectively
3. Run "docker run --cap-add=IPC_LOCK --device=/dev/infiniband/uverbs0 --net=host --rm -it ucc_daemon:v1" on two DPU terminals
4. Wait for the following message to appear:
DPU daemon: Running with 4 compute threads
5. Launch run_omb.sh script on Host terminal

#### Notes ####
1. The provided run scripts are for 2 Nodes with 1 DPU each.
   Edit hostfiles and run scripts for larger of nodes as required.
2. Currently the following collectives are supported for DPU offload:
   Allreduce, Iallreduce, Alltoall, Ialltoall, Alltoallv, Ialltoallv
   Edit the run_omb.sh script to run different collectives and message sizes.
3. If you run the run_omb.sh on host without launching the DPU script,
   it will launch MPI without DPU enabled and nothing will be offloaded.
4. Building OpenMPI takes a long time, especially on BF2.
   Avoid unnecessary rebuilds by editing the build scripts appropriately.
