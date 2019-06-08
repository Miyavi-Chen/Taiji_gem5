#!/bin/bash

export M5_PATH=~/6TDisk/m5/yeah

OUTDIR_BASE="/home/miyavi/workspace"
CHECKPOINT_DIR="${OUTDIR_BASE}/checkpointARM"
WORKLOAD=505.mcf_r
GROUP=cpu2017_arm
WORKNAME=O3_fs_4cores_LFUDA_Hybrid_80E
FAST_FORWARD=30000000
MAXINSTS=8000000000
M5_OUT_ROOT=/home/miyavi/Dropbox/gem5/experiment/workload
OUT_DIR=${M5_OUT_ROOT}/${GROUP}/${WORKLOAD}/${WORKNAME}
SCRIPT=./.vscode/cpu2017.sh

################################################################################



# CPU_TYPE=AtomicSimpleCPU

# build/ARM/gem5.opt \
# configs/example/fs.py \
# --arm-iset="aarch64" \
# --machine-type="VExpress_GEM5_V1" \
# --dtb=$M5_PATH/binaries/armv8_gem5_v1_4cpu.dtb \
# --checkpoint-dir=${CHECKPOINT_DIR} \
# --kernel=vmlinux-4.15.0-master-gem5 \
# --disk-image=hdd0531.img \
# --num-cpus=4 --cpu-clock=5.0GHz \
# --cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
# --caches --l1d_size=32kB --l1i_size=32kB \
# --l2cache --l2_size=2MB --num-l2caches=1 \
# --ruby --mem-size=16GB --mem-type=MemSubsystem \
# --channel-sizes='16GB;8GB' \
# --channel-types='PCM_DDR2_16G_400_16x4;DDR4_2400_8G_8x8' \
# --balance-interval=0us --time-warmup=100us
# exit 0


# hammer
# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=DerivO3CPU

build/ARM/gem5.opt \
--outdir=${OUT_DIR} \
configs/example/fs.py \
--script=${SCRIPT} \
--arm-iset="aarch64" \
--machine-type="VExpress_GEM5_V1" \
--dtb=$M5_PATH/binaries/armv8_gem5_v1_4cpu.dtb \
--checkpoint-restore=1 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-master-gem5 \
--disk-image=hdd0531.img \
--num-cpus=4 --cpu-clock=5.0GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=2MB --num-l2caches=1 \
--ruby --mem-size=16GB --mem-type=MemSubsystem \
--channel-sizes='16GB;8GB' \
--channel-types='PCM_DDR2_16G_400_16x4;DDR4_2400_8G_8x8' \
--balance-interval=200us --time-warmup=100ms \
--maxinsts=${MAXINSTS} \
| tee m5out/log.out.run.sh.log
exit 0


# CMP_directory
# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=DerivO3CPU

build/ARM/gem5.opt \
--outdir=${OUT_DIR} \
configs/example/fs.py \
--script=${SCRIPT} \
--arm-iset="aarch64" \
--machine-type="VExpress_GEM5_V1" \
--dtb=$M5_PATH/binaries/armv8_gem5_v1_4cpu.dtb \
--checkpoint-restore=3 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-master-gem5 \
--disk-image=hdd0531.img \
--num-cpus=4 --cpu-clock=5.0GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=8MB --num-l2caches=1 \
--ruby --mem-size=16GB --mem-type=MemSubsystem \
--channel-sizes='16GB;8GB' \
--channel-types='PCM_DDR2_16G_400_16x4;DDR4_2400_8G_8x8' \
--balance-interval=200us --time-warmup=100us \
--maxinsts=${MAXINSTS}
exit 0



################################################################################
