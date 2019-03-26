/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Andreas Hansson
 */

#include "mem/flex_mem.hh"

#include "sim/system.hh"

FlexMem::FlexMem(const FlexMemParams* p)
    : MemObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      memRanges(p->mem_ranges),
      channelRanges(p->channel_ranges),
      verbose(p->verbose),
      sys(p->sys),
      intlvSize(p->intlv_size),
      intlvCoverSize(0)
{
    assert(isAscending(memRanges));
    assert(isAscending(channelRanges));
    assert(totalSizeOf(memRanges));
    assert(totalSizeOf(channelRanges));
    assert(totalSizeOf(memRanges) == totalSizeOf(channelRanges));
}

void
FlexMem::init()
{
    if (!slavePort.isConnected() || !masterPort.isConnected()) {
        fatal("FlexMem is not connected on both sides.\n");
    }

    if (intlvSize) {
        assert(isPowerOf2(intlvSize));
        assert(isPowerOf2(sys->cacheLineSize()));
        assert(intlvSize >= sys->cacheLineSize());

        for (size_t i = 0; i < memRanges.size(); ++i) {
            assert((memRanges[i].start() % intlvSize) == 0);
            assert((memRanges[i].size() % intlvSize) == 0);
        }

        for (size_t i = 0; i < channelRanges.size(); ++i) {
            assert((channelRanges[i].start() % intlvSize) == 0);
            assert((channelRanges[i].size() % intlvSize) == 0);
        }

        Addr minSize = std::numeric_limits<Addr>::max();
        for (size_t i = 0; i < channelRanges.size(); ++i) {
            minSize = std::min(minSize, channelRanges[i].size());
        }
        *(const_cast<Addr*>(&intlvCoverSize)) = minSize;

        assert(intlvCoverSize);
        assert(intlvCoverSize != std::numeric_limits<Addr>::max());
        assert((intlvCoverSize % intlvSize) == 0);

        Addr remainIntlv = intlvCoverSize * channelRanges.size();
        for (size_t i = 0; i < memRanges.size(); ++i) {
            if (remainIntlv == 0) {
                Addr singleStart = memRanges[i].start();
                Addr singleEnd = singleStart + memRanges[i].size() - 1;
                AddrRange singleRange = AddrRange(singleStart, singleEnd);
                memSingleZone.push_back(singleRange);
            } else if (memRanges[i].size() > remainIntlv) {
                Addr intlvStart = memRanges[i].start();
                Addr intlvEnd = intlvStart + remainIntlv - 1;
                Addr singleStart = intlvEnd + 1;
                Addr singleEnd = intlvEnd + memRanges[i].size() - remainIntlv;
                AddrRange intlvRange = AddrRange(intlvStart, intlvEnd);
                AddrRange singleRange = AddrRange(singleStart, singleEnd);
                remainIntlv -= remainIntlv;
                memIntlvZone.push_back(intlvRange);
                memSingleZone.push_back(singleRange);
            } else if (memRanges[i].size() == remainIntlv) {
                Addr intlvStart = memRanges[i].start();
                Addr intlvEnd = intlvStart + memRanges[i].size() - 1;
                AddrRange intlvRange = AddrRange(intlvStart, intlvEnd);
                remainIntlv -= memRanges[i].size();
                memIntlvZone.push_back(intlvRange);
            } else if (memRanges[i].size() < remainIntlv) {
                Addr intlvStart = memRanges[i].start();
                Addr intlvEnd = intlvStart + memRanges[i].size() - 1;
                AddrRange intlvRange = AddrRange(intlvStart, intlvEnd);
                remainIntlv -= memRanges[i].size();
                memIntlvZone.push_back(intlvRange);
            } else {
                assert(0);
            }
        }
        assert(!remainIntlv);

        const Addr coverSize = intlvCoverSize;
        for (size_t i = 0; i < channelRanges.size(); ++i) {
            if (coverSize == 0) {
                Addr singleStart = channelRanges[i].start();
                Addr singleEnd = singleStart + channelRanges[i].size() - 1;
                AddrRange singleRange = AddrRange(singleStart, singleEnd);
                channelSingleZone.push_back(singleRange);
            } else if (channelRanges[i].size() > coverSize) {
                Addr intlvStart = channelRanges[i].start();
                Addr intlvEnd = intlvStart + coverSize - 1;
                Addr singleStart = intlvEnd + 1;
                Addr singleEnd = intlvEnd + channelRanges[i].size() - coverSize;
                AddrRange intlvRange = AddrRange(intlvStart, intlvEnd);
                AddrRange singleRange = AddrRange(singleStart, singleEnd);
                channelIntlvZone.push_back(intlvRange);
                channelSingleZone.push_back(singleRange);
            } else if (channelRanges[i].size() == coverSize) {
                Addr intlvStart = channelRanges[i].start();
                Addr intlvEnd = intlvStart + channelRanges[i].size() - 1;
                AddrRange intlvRange = AddrRange(intlvStart, intlvEnd);
                channelIntlvZone.push_back(intlvRange);
            } else if (channelRanges[i].size() < coverSize) {
                assert(0);
            } else {
                assert(0);
            }
        }

        assert(isAscending(memIntlvZone));
        assert(isAscending(channelIntlvZone));
        assert(isAscending(memSingleZone));
        assert(isAscending(channelSingleZone));
        assert(totalSizeOf(memIntlvZone) == totalSizeOf(channelIntlvZone));
        assert(totalSizeOf(memSingleZone) == totalSizeOf(channelSingleZone));
        assert(totalSizeOf(memRanges) ==
            (totalSizeOf(memIntlvZone) + totalSizeOf(memSingleZone)));
        assert(totalSizeOf(channelRanges) ==
            (totalSizeOf(channelIntlvZone) + totalSizeOf(channelSingleZone)));
    }
}

BaseSlavePort&
FlexMem::getSlavePort(const std::string& if_name, PortID idx)
{
    if (if_name == "slave") {
        return slavePort;
    } else {
        return MemObject::getSlavePort(if_name, idx);
    }
}

BaseMasterPort&
FlexMem::getMasterPort(const std::string& if_name, PortID idx)
{
    if (if_name == "master") {
        return masterPort;
    } else {
        return MemObject::getMasterPort(if_name, idx);
    }
}

void
FlexMem::recvFunctional(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const Addr channel_addr = toChannelAddr(mem_addr, pkt->getSize());
    pkt->setAddr(channel_addr);
    masterPort.sendFunctional(pkt);
    pkt->setAddr(mem_addr);
}

void
FlexMem::recvFunctionalSnoop(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const Addr channel_addr = toChannelAddr(mem_addr, pkt->getSize());
    pkt->setAddr(channel_addr);
    slavePort.sendFunctionalSnoop(pkt);
    pkt->setAddr(mem_addr);
}

Tick
FlexMem::recvAtomic(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const Addr channel_addr = toChannelAddr(mem_addr, pkt->getSize());
    pkt->setAddr(channel_addr);
    Tick ret_tick = masterPort.sendAtomic(pkt);
    pkt->setAddr(mem_addr);
    return ret_tick;
}

Tick
FlexMem::recvAtomicSnoop(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const Addr channel_addr = toChannelAddr(mem_addr, pkt->getSize());
    pkt->setAddr(channel_addr);
    Tick ret_tick = slavePort.sendAtomicSnoop(pkt);
    pkt->setAddr(mem_addr);
    return ret_tick;
}

bool
FlexMem::recvTimingReq(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const Addr channel_addr = toChannelAddr(mem_addr, pkt->getSize());
    bool needsResponse = pkt->needsResponse();
    bool cacheResponding = pkt->cacheResponding();

    if (needsResponse && !cacheResponding) {
        pkt->pushSenderState(new FlexMemSenderState(mem_addr));
    }

    pkt->setAddr(channel_addr);

    // Attempt to send the packet
    bool successful = masterPort.sendTimingReq(pkt);

    // If not successful, restore the address and sender state
    if (!successful) {
        pkt->setAddr(mem_addr);
        if (needsResponse) {
            delete pkt->popSenderState();
        }
    }

    return successful;
}

bool
FlexMem::recvTimingResp(PacketPtr pkt)
{
    FlexMemSenderState* receivedState =
        dynamic_cast<FlexMemSenderState*>(pkt->senderState);

    // Restore initial sender state
    if (receivedState == NULL) {
        panic("FlexMem %s got a response without sender state\n",
              name());
    }

    const Addr channel_addr = pkt->getAddr();

    // Restore the state and address
    pkt->senderState = receivedState->predecessor;
    pkt->setAddr(receivedState->memAddr);

    // Attempt to send the packet
    bool successful = slavePort.sendTimingResp(pkt);

    // If packet successfully sent, delete the sender state, otherwise
    // restore state
    if (successful) {
        delete receivedState;
    } else {
        // Don't delete anything and let the packet look like we did
        // not touch it
        pkt->senderState = receivedState;
        pkt->setAddr(channel_addr);
    }
    return successful;
}

void
FlexMem::recvTimingSnoopReq(PacketPtr pkt)
{
    slavePort.sendTimingSnoopReq(pkt);
}

bool
FlexMem::recvTimingSnoopResp(PacketPtr pkt)
{
    return masterPort.sendTimingSnoopResp(pkt);
}

bool
FlexMem::isSnooping() const
{
    if (slavePort.isSnooping()) {
        fatal("FlexMem doesn't support remapping of snooping requests\n");
    }
    return false;
}

void
FlexMem::recvReqRetry()
{
    slavePort.sendRetryReq();
}

void
FlexMem::recvRespRetry()
{
    masterPort.sendRetryResp();
}

void
FlexMem::recvRangeChange()
{
    slavePort.sendRangeChange();
}

AddrRangeList
FlexMem::getAddrRanges() const
{
    // Simply return the original ranges as given by the parameters
    AddrRangeList ranges(memRanges.begin(), memRanges.end());
    return ranges;
}

Addr
FlexMem::toChannelAddr(const Addr addr, const unsigned int size) const
{
    if (!intlvSize) {
        return addr;
    }

    assert(size);

    Addr intlvBlkIdx = 0;
    for (size_t i = 0; i < memIntlvZone.size(); ++i) {
        if (memIntlvZone[i].contains(addr)) {
            const Addr end = addr + size - 1;
            assert(memIntlvZone[i].contains(end));
            assert((addr / intlvSize) == (end / intlvSize));
            intlvBlkIdx += (addr - memIntlvZone[i].start()) / intlvSize;
            const int channelIdx = intlvBlkIdx % channelIntlvZone.size();
            const Addr shiftBlks = intlvBlkIdx / channelIntlvZone.size();
            const Addr rgStart = channelIntlvZone[channelIdx].start();
            const Addr blkAddr = rgStart + (shiftBlks * intlvSize);
            const Addr newAddr = blkAddr + (addr % intlvSize);
            assert(channelIntlvZone[channelIdx].contains(newAddr));
            assert(channelIntlvZone[channelIdx].contains(newAddr + size - 1));
            assert((addr % intlvSize) == (newAddr % intlvSize));
            return newAddr;
        } else {
            intlvBlkIdx += (memIntlvZone[i].size() / intlvSize);
        }
    }

    Addr singleMemAddr = 0;
    for (size_t i = 0; i < memSingleZone.size(); ++i) {
        if (memSingleZone[i].contains(addr)) {
            const Addr end = addr + size - 1;
            assert(memSingleZone[i].contains(end));
            singleMemAddr += (addr - memSingleZone[i].start());
            for (size_t c = 0; c < channelSingleZone.size(); ++c) {
                if (singleMemAddr < channelSingleZone[c].size()) {
                    const Addr newAddr =
                            channelSingleZone[c].start() + singleMemAddr;
                    assert(channelSingleZone[c].contains(newAddr));
                    assert(channelSingleZone[c].contains(newAddr + size - 1));
                    return newAddr;
                } else {
                    singleMemAddr -= channelSingleZone[c].size();
                }
            }
        } else {
            singleMemAddr += memSingleZone[i].size();
        }
    }

    panic("FlexMem: address '0x%x' interleaving failure!\n", addr);
    return addr;
}

bool
FlexMem::isAscending(const std::vector<AddrRange> &ranges) const
{
    Addr bound = std::numeric_limits<Addr>::max();
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i == 0) {
            bound = ranges[i].end();
        } else if (ranges[i].start() > bound) {
            bound = ranges[i].end();
        } else {
            return false;
        }
    }
    return true;
}

Addr
FlexMem::totalSizeOf(const std::vector<AddrRange> &ranges) const
{
    Addr sum = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        sum += ranges[i].size();
    }
    return sum;
}

FlexMem*
FlexMemParams::create()
{
    return new FlexMem(this);
}
