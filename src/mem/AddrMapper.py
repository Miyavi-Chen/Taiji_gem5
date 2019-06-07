# Copyright (c) 2012 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Andreas Hansson

from m5.params import *
from m5.proxy import *
from m5.objects.MemObject import MemObject

# An address mapper changes the packet addresses in going from the
# slave port side of the mapper to the master port side. When the
# slave port is queried for the address ranges, it also performs the
# necessary range updates. Note that snoop requests that travel from
# the master port (i.e. the memory side) to the slave port are
# currently not modified.
class AddrMapper(MemObject):
    type = 'AddrMapper'
    cxx_header = 'mem/addr_mapper.hh'
    abstract = True

    # one port in each direction
    master = MasterPort("Master port")
    slave = SlavePort("Slave port")


# Range address mapper that maps a set of original ranges to a set of
# remapped ranges, where a specific range is of the same size
# (original and remapped), only with an offset.
class RangeAddrMapper(AddrMapper):
    type = 'RangeAddrMapper'
    cxx_header = 'mem/addr_mapper.hh'

    # These two vectors should be the exact same length and each range
    # should be the exact same size. Each range in original_ranges is
    # mapped to the corresponding element in the remapped_ranges. Note
    # that the same range can occur multiple times in the remapped
    # ranges for address aliasing.
    original_ranges = VectorParam.AddrRange(
        "Ranges of memory that should me remapped")
    remapped_ranges = VectorParam.AddrRange(
        "Ranges of memory that are being mapped to")

class CowardAddrMapper(MemObject):
    type = 'CowardAddrMapper'
    cxx_header = 'mem/coward_addr_mapper.hh'

    slave = SlavePort("Slave port")
    master = MasterPort("Master port")

    original_ranges = VectorParam.AddrRange(
        "Ranges of memory that should me remapped")
    remapped_ranges = VectorParam.AddrRange(
        "Ranges of memory that are being mapped to")

    verbose = Param.Bool(False, "Print information")

class HomeAgent(MemObject):
    type = 'HomeAgent'
    cxx_header = 'mem/home_agent.hh'

    slave = SlavePort("Slave port")
    master = MasterPort("Master port")

    phys_ranges = VectorParam.AddrRange(
        "Ranges of memory that should me remapped")
    mem_ranges = VectorParam.AddrRange(
        "Ranges of memory that are being mapped to")

    verbose = Param.Bool(False, "Print information")

class FlexMem(MemObject):
    type = 'FlexMem'
    cxx_header = 'mem/flex_mem.hh'

    slave = SlavePort("Slave port")
    master = MasterPort("Master port")

    mem_ranges = VectorParam.AddrRange(
        "Ranges of memory that should me remapped")
    channel_ranges = VectorParam.AddrRange(
        "Ranges of memory that are being mapped to")

    verbose = Param.Bool(False, "Print information")
    sys = Param.System(Parent.any, "System we belong to")
    intlv_size = Param.MemorySize("Channel interleaving size")

class HybridMem(MemObject):
    type = 'HybridMem'
    cxx_header = 'mem/hybrid_mem.hh'

    slave = SlavePort("Slave port")
    master = MasterPort("Master port")

    phys_ranges = VectorParam.AddrRange(
        "Ranges of original physical address")
    mem_ranges = VectorParam.AddrRange(
        "Ranges of memory that should me remapped")
    channel_ranges = VectorParam.AddrRange(
        "Ranges of memory that are being mapped to")

    verbose = Param.Bool(False, "Print information")
    sys = Param.System(Parent.any, "System we belong to")
    granularity = Param.MemorySize('4kB', "Granular size for mapping")
    header_delay = Param.Cycles(1, "Header delay")
    width = Param.Unsigned(16, "Datapath width per port (bytes)")
    max_migration_tasks = Param.Unsigned(16,
        "Number of pages migrated at the same time")
    time_interval = Param.Latency('10us', "Balance interval")
    time_warmup = Param.Latency('10us', "Warm up interval")
