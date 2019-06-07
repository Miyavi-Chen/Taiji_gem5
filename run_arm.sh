#!/bin/bash

export M5_PATH=~/6TDisk/m5/yeah

OUTDIR_BASE="/home/miyavi/workspace"
CHECKPOINT_DIR="${OUTDIR_BASE}/checkpointARM"

################################################################################



# CPU_TYPE=AtomicSimpleCPU

# build/ARM/gem5.opt \
# configs/example/fs.py \
# --arm-iset="aarch64" \
# --machine-type="VExpress_GEM5_V1" \
# --dtb=$M5_PATH/binaries/armv8_gem5_v1_8cpu.dtb \
# --checkpoint-dir=${CHECKPOINT_DIR} \
# --kernel=vmlinux-4.15.0-master-gem5 \
# --disk-image=hdd.img \
# --num-cpus=8 --cpu-clock=5.0GHz \
# --cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
# --caches --l1d_size=32kB --l1i_size=32kB \
# --l2cache --l2_size=1MB --num-l2caches=1 \
# --ruby --mem-size=32GB --mem-type=MemSubsystem \
# --channel-sizes='32GB;4GB' \
# --channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
# --balance-interval=500us --time-warmup=100us \
# --workload_name="NA"
# exit 0



# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=TimingSimpleCPU

build/ARM/gem5.opt \
configs/example/fs.py \
--arm-iset="aarch64" \
--machine-type="VExpress_GEM5_V1" \
--dtb=$M5_PATH/binaries/armv8_gem5_v1_8cpu.dtb \
--checkpoint-restore=2 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-master-gem5 \
--disk-image=hdd.img \
--num-cpus=8 --cpu-clock=5.0GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=1MB --num-l2caches=1 \
--ruby --mem-size=32GB --mem-type=MemSubsystem \
--channel-sizes='32GB;4GB' \
--channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
--balance-interval=500us --time-warmup=100us \
# --workload_name="NA"
exit 0



# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=TimingSimpleCPU

build/ARM/gem5.opt \
configs/example/fs.py \
--arm-iset="aarch64" \
--machine-type="VExpress_GEM5_V1" \
--dtb=$M5_PATH/binaries/armv8_gem5_v1_8cpu.dtb \
--checkpoint-restore=2 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-master-gem5 \
--disk-image=hdd.img \
--num-cpus=8 --cpu-clock=5.0GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=1MB --num-l2caches=1 \
--ruby --mem-size=32GB --mem-type=MemSubsystem \
--channel-sizes='32GB;4GB' \
--channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
--balance-interval=500us --time-warmup=100us \
--workload_name="NA"
exit 0



################################################################################
