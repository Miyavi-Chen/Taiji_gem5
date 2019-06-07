#!/bin/bash

export M5_PATH=~/6TDisk/m5

OUTDIR_BASE="/home/miyavi/workspace"
CHECKPOINT_DIR="${OUTDIR_BASE}/checkpointX86_trace"

################################################################################



# CPU_TYPE=AtomicSimpleCPU

# build/X86/gem5.opt \
# configs/example/fs.py \
# --checkpoint-dir=${CHECKPOINT_DIR} \
# --kernel=vmlinux-4.15.0-nova_m-gem5 \
# --command-line-file=.vscode/miyavi_command_line \
# --disk-image=ubuntu-16.04.5-server-amd64.img \
# --num-cpus=8 --cpu-clock=3.6GHz \
# --cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
# --caches --l1d_size=32kB --l1i_size=32kB \
# --l2cache --l2_size=256kB --num-l2caches=1 \
# --ruby --mem-size=32GB --mem-type=MemSubsystem \
# --channel-sizes='32GB;4GB' \
# --channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
# --balance-interval=100us --time-warmup=100us \
# --workload_name="NA"
# exit 0



# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=TimingSimpleCPU

build/X86/gem5.opt \
configs/example/fs.py \
--checkpoint-restore=1 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-nova_m-gem5 \
--command-line-file=.vscode/miyavi_command_line \
--disk-image=ubuntu-16.04.5-server-amd64.img \
--num-cpus=8 --cpu-clock=3.6GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=256kB --num-l2caches=1 \
--ruby --mem-size=32GB --mem-type=MemSubsystem \
--channel-sizes='32GB;4GB' \
--channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
--balance-interval=100us --time-warmup=100us \
--workload_name="bfs"
exit 0



# AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
CPU_TYPE=TimingSimpleCPU

build/X86/gem5.opt \
configs/example/fs.py \
--checkpoint-restore=2 \
--checkpoint-dir=${CHECKPOINT_DIR} \
--kernel=vmlinux-4.15.0-nova_m-gem5 \
--command-line-file=.vscode/miyavi_command_line \
--disk-image=ubuntu-16.04.5-server-amd64.img \
--num-cpus=8 --cpu-clock=3.6GHz \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l1d_size=32kB --l1i_size=32kB \
--l2cache --l2_size=256kB --num-l2caches=1 \
--ruby --mem-size=32GB --mem-type=MemSubsystem \
--channel-sizes='32GB;4GB' \
--channel-types='PCM_LPDDR2_32G_400_16x4;DDR4_2400_4x16' \
--balance-interval=100us --time-warmup=100us \
--workload_name="NA"
exit 0



################################################################################
