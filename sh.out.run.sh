#!/bin/bash



COLOR_REST='\e[0m'
COLOR_RED='\e[0;31m';
COLOR_GREEN='\e[0;32m';



GEM5_ROOT=/home/peter/Desktop/gem5
export M5_PATH=${GEM5_ROOT}/full_system



################################################################################



scons --clean
rm -rf ./build
rm -f ./a.o ./liba.a ./liba.so ./a.out

exit 0



################################################################################



scons --jobs=$(($(nproc)+1)) --force-lto EXTRAS=nvmain build/X86/gem5.opt
scons --jobs=$(($(nproc)+1)) --force-lto EXTRAS=nvmain build/X86/gem5.debug

g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -c ./a.cc && \
ar rcs -o ./liba.a ./a.o && \
gcc -std=gnu99 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -static ./a.c -o ./a.out -L. -la -ldl -lstdc++

exit 0



# rm -f ./a.o ./liba.a ./liba.so ./a.out && \
# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -c ./a.cc && \
# ar rcs -o ./liba.a ./a.o && \
# gcc -std=gnu99 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -static ./a.c -o ./a.out -L. -la -ldl -lstdc++
# ./a.out



# rm -f ./a.o ./liba.a ./liba.so ./a.out && \
# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -fPIC -c ./a.cc && \
# gcc -shared -o ./liba.so ./a.o && \
# gcc -std=gnu99 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer ./a.c -o ./a.out -L. -la -ldl -lstdc++
# env LD_LIBRARY_PATH=$(pwd) ./a.out



################################################################################
###



# EXE=opt
# MEM_TYPE=NVMainMemory



# CPU_TYPE=AtomicSimpleCPU
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config
# exit 0



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=DerivO3CPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=DDR4_2400_8x8

# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config
# exit 0



EXE=opt
CPU_TYPE=DerivO3CPU
MEM_TYPE=DDR4_2400_8x8
FAST_FORWARD=100000000
MAXINSTS=200000000
M5_OUT_ROOT=/home/peter/Desktop/Dropbox/3#RUNNING/m5out
OUT_DIR=${M5_OUT_ROOT}/m5out.DDR4_2400_8x8

time \
${GEM5_ROOT}/build/X86/gem5.${EXE} \
--debug-flags=MyAddrRanges \
--outdir=${OUT_DIR} \
${GEM5_ROOT}/configs/example/se.py \
-c "/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit" \
-o "/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in" \
--fast-forward=${FAST_FORWARD} --maxinsts=${MAXINSTS} \
--num-cpus=4 \
--cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
--caches --l2cache --l3cache \
--mem-size=16GB --mem-channels=1 --mem-type=${MEM_TYPE} \
2>&1 | tee ${OUT_DIR}/log.out.run.sh.log

rm -f ${OUT_DIR}/$(basename ${0})
cp ${0} ${OUT_DIR}



# EXE=opt
# CPU_TYPE=DerivO3CPU
# MEM_TYPE=NVMainMemory
# FAST_FORWARD=100000000
# MAXINSTS=200000000
# CLK=0666
# M5_OUT_ROOT=/home/peter/Desktop/Dropbox/3#RUNNING/m5out
# OUT_DIR=${M5_OUT_ROOT}/m5out.${CLK}

# time \
# ${GEM5_ROOT}/build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# --outdir=${OUT_DIR} \
# ${GEM5_ROOT}/configs/example/se.py \
# -c "/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/mcf_base.gcc43-64bit" \
# -o "/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in;/home/peter/Desktop/cpu2006/benchspec/CPU2006/429.mcf/run/run_base_ref_gcc43-64bit.0000/inp.in" \
# --fast-forward=${FAST_FORWARD} --maxinsts=${MAXINSTS} \
# --num-cpus=4 \
# --cpu-type=${CPU_TYPE} --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-size=8GB --mem-channels=1 --mem-type=${MEM_TYPE} \
# --nvmain-config=${GEM5_ROOT}/nvmain/Config/2D_DRAM_example.${CLK}.config \
# 2>&1 | tee ${OUT_DIR}/log.out.run.sh.log

# rm -f ${OUT_DIR}/$(basename ${0})
# cp ${0} ${OUT_DIR}



# EXE=debug
# MEM_TYPE=NVMainMemory



# CPU_TYPE=AtomicSimpleCPU
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config
# exit 0



# CPU_TYPE=TimingSimpleCPU
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --checkpoint-restore=1 \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config
# exit 0



###
################################################################################



# EXE=opt
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=AtomicSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config



################################################################################



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# perf record -g --output=./perf.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf -- \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# perf script --input=./perf.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf | \
# c++filt | \
# gprof2dot --format=perf --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot



# EXE=perf
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# env CPUPROFILE=./pprof.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# google-pprof --callgrind build/X86/gem5.${EXE} ./pprof.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof > \
# ./cachegrind.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# gprof2dot --format=callgrind --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
# ./cachegrind.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind



# EXE=prof
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# gprof build/X86/gem5.${EXE} > ./gprof.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# if [ $? -e 0 ]; then
#     cat gprof.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof | \
#     gprof2dot --format=prof --strip --wrap \
#     --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
#     --output=./dot.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot
# else
#     echo -e "${COLOR_RED}[error] gprof ...${COLOR_REST}" >&2
# fi



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# valgrind --tool=callgrind \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/fs.py \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# mv ./callgrind.out.* ./cachegrind.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# gprof2dot --format=callgrind --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
# ./cachegrind.out.fs-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind



################################################################################



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# perf record -g --output=./perf.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf -- \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# perf script --input=./perf.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf | \
# c++filt | \
# gprof2dot --format=perf --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.perf.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot



# EXE=perf
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# env CPUPROFILE=./pprof.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# google-pprof --callgrind build/X86/gem5.${EXE} ./pprof.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof > \
# ./cachegrind.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# gprof2dot --format=callgrind --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
# ./cachegrind.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.pprof.cachegrind



# EXE=prof
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# gprof build/X86/gem5.${EXE} > ./gprof.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# if [ $? -e 0 ]; then
#     cat gprof.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof | \
#     gprof2dot --format=prof --strip --wrap \
#     --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
#     --output=./dot.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.gprof.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot
# else
#     echo -e "${COLOR_RED}[error] gprof ...${COLOR_REST}" >&2
# fi



# EXE=debug
# # X86KvmCPU, AtomicSimpleCPU, TimingSimpleCPU, DerivO3CPU
# CPU_TYPE=TimingSimpleCPU
# # DDR4_2400_8x8, DRAMSim2, NVMainMemory
# MEM_TYPE=NVMainMemory

# valgrind --tool=callgrind \
# build/X86/gem5.${EXE} \
# --debug-flags=MyAddrRanges \
# configs/example/se.py \
# --cmd=./a.out \
# --cpu-type=${CPU_TYPE} \
# --restore-with-cpu=${CPU_TYPE} \
# --caches --l2cache --l3cache \
# --mem-type=${MEM_TYPE} \
# --nvmain-config=nvmain/Config/Hybrid_example.config

# mv ./callgrind.out.* ./cachegrind.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind

GPROF2DOT_NODE=0
GPROF2DOT_EDGE=0
gprof2dot --format=callgrind --strip --wrap \
--node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
--output=./dot.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
./cachegrind.out.se-${EXE}-${CPU_TYPE}-${MEM_TYPE}.callgrind.cachegrind



################################################################################



# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer ./a.cc -o ./a.out
# perf record -g --output=./perf.out.a.perf -- ./a.out

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# perf script --input=./perf.out.a.perf | \
# c++filt | \
# gprof2dot --format=perf --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.a.perf.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot



# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer -L/usr/lib -Wl,--no-as-needed -lprofiler -Wl,--as-needed ./a.cc -o ./a.out
# env CPUPROFILE=./pprof.out.a.pprof ./a.out

# google-pprof --callgrind ./a.out ./pprof.out.a.pprof > \
# ./cachegrind.out.a.pprof.cachegrind

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# gprof2dot --format=callgrind --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.a.pprof.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
# ./cachegrind.out.a.pprof.cachegrind



# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -pg -fno-omit-frame-pointer ./a.cc -o ./a.out
# ./a.out
# gprof ./a.out > ./gprof.out.a.gprof

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# if [ $? -e 0 ]; then
#     cat gprof.out.a.gprof | \
#     gprof2dot --format=prof --strip --wrap \
#     --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
#     --output=./dot.out.a.gprof.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot
# else
#     echo -e "${COLOR_RED}[error] gprof ...${COLOR_REST}" >&2
# fi



# g++ -std=c++11 -m64 -Wall -Wextra -O0 -g -fno-omit-frame-pointer ./a.cc -o ./a.out
# valgrind --tool=callgrind ./a.out
# mv ./callgrind.out.* ./cachegrind.out.a.callgrind.cachegrind

# GPROF2DOT_NODE=0
# GPROF2DOT_EDGE=0
# gprof2dot --format=callgrind --strip --wrap \
# --node-thres=${GPROF2DOT_NODE} --edge-thres=${GPROF2DOT_EDGE} \
# --output=./dot.out.a.callgrind.cachegrind.n${GPROF2DOT_NODE}-e${GPROF2DOT_EDGE}.dot \
# ./cachegrind.out.a.callgrind.cachegrind


