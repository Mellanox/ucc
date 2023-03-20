#### How to Build ####
0. Make sure there is a shared file system available and mounted on all hosts and dpus
1. Make sure autotools (automake, autoconf, libtool, m4, etc.) are installed
2. Log in to host, edit ./config.sh to provide location to build
   This script will download and build UCX, UCC, OpenMPI, and OSU MPI Benchmarks
3. Run ./run.sh download, look out for errors during clone process
4. Run ./run.sh build on the host side and on the dpu side, 
   look out for errors during build process


#### Prerequisites ####
1. Set up passwordless ssh between all hosts and DPUs
2. Edit hostfile.cpu to provide correct CPU/x86 hostnames
3. Edit hostfile.dpu to provide correct BF2/arm hostnames
4. Edit host_to_dpu.list to provide correct HOST->DPU mapping
5. Edit host_to_dpu.list to provide correct NIC/HCA (e.g. mlx5_0:1)
6. Edit run_omb.sh and run_dpu.sh to correct build locations


#### How to Run ####
1. Open two terminals, one for Host and one for DPU
2. ssh to Host and DPU on terminal 1 and 2 respectively
3. Launch run.sh run_dpu script on DPU terminal
4. Wait for the following message to appear:
[1,0]<stdout>:DPU server: Running with 8 worker threads on port 10001
[1,1]<stdout>:DPU server: Running with 8 worker threads on port 10001
5. Launch run_omb.sh script on Host terminal

Example:

#### HOST ####                              #### DPU ####
# ssh host-01                               # ssh bf2-01
# cd /nfs/dpu/scripts                       # cd /nfs/dpu/scripts
# ARCH="x86" ./run.sh download              # ARCH="dpu"./run.sh build
# ARCH="x86" ./run.sh build                                          #
#                                           # ARCH="dpu" ./run.sh run_dpu
# ARCH="x86" ./run.sh run_omb


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
5. Building UCX, OpenMPI, and OSU Benchmarks can be avoided by using HPCX.
