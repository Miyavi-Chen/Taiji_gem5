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

#include "mem/home_agent.hh"

HomeAgent::HomeAgent(const HomeAgentParams* p)
    : ClockedObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      physRanges(p->phys_ranges),
      memRanges(p->mem_ranges),
      verbose(p->verbose)
{
    if (physRanges.size() != memRanges.size())
        fatal("HomeAgent: original and shadowed range list must "
              "be same size\n");

    for (size_t x = 0; x < physRanges.size(); x++) {
        if (physRanges[x].size() != memRanges[x].size())
            fatal("HomeAgent: original and shadowed range list elements"
                  " aren't all of the same size\n");
    }
}

void
HomeAgent::init()
{
    if (!slavePort.isConnected() || !masterPort.isConnected()) {
        fatal("HomeAgent is not connected on both sides.\n");
    }

    if (verbose) {
        std::cout << "[cc] HomeAgent::init(): "
                  << "name=" << name() << std::endl;
        for (size_t x = 0; x < physRanges.size(); x++) {
            std::cout << "\t"
                    << physRanges[x].to_string()
                    << " -> "
                    << memRanges[x].to_string()
                    << std::endl;
        }
    }
}

Port &
HomeAgent::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "master") {
        // the master port index translates directly to the vector position
        return masterPort;
    } else if (if_name == "slave") {
        // the slave port index translates directly to the vector position
        return slavePort;
    } else {
        return ClockedObject::getPort(if_name, idx);
    }
}


void
HomeAgent::recvFunctional(PacketPtr pkt)
{
    const Addr phys_addr = pkt->getAddr();
    const Addr mem_addr = toMemAddr(phys_addr);
    pkt->setPhysAddr(phys_addr);
    pkt->setAddr(mem_addr);
    masterPort.sendFunctional(pkt);
    pkt->invalidPhysAddr();
    pkt->setAddr(phys_addr);
}

void
HomeAgent::recvFunctionalSnoop(PacketPtr pkt)
{
    const Addr phys_addr = pkt->getAddr();
    const Addr mem_addr = toMemAddr(phys_addr);
    pkt->setPhysAddr(phys_addr);
    pkt->setAddr(mem_addr);
    slavePort.sendFunctionalSnoop(pkt);
    pkt->invalidPhysAddr();
    pkt->setAddr(phys_addr);
}

Tick
HomeAgent::recvAtomic(PacketPtr pkt)
{
    const Addr phys_addr = pkt->getAddr();
    const Addr mem_addr = toMemAddr(phys_addr);
    pkt->setPhysAddr(phys_addr);
    pkt->setAddr(mem_addr);
    Tick ret_tick = masterPort.sendAtomic(pkt);
    pkt->invalidPhysAddr();
    pkt->setAddr(phys_addr);
    return ret_tick;
}

Tick
HomeAgent::recvAtomicSnoop(PacketPtr pkt)
{
    const Addr phys_addr = pkt->getAddr();
    const Addr mem_addr = toMemAddr(phys_addr);
    pkt->setPhysAddr(phys_addr);
    pkt->setAddr(mem_addr);
    Tick ret_tick = slavePort.sendAtomicSnoop(pkt);
    pkt->invalidPhysAddr();
    pkt->setAddr(phys_addr);
    return ret_tick;
}

bool
HomeAgent::recvTimingReq(PacketPtr pkt)
{
    const Addr phys_addr = pkt->getAddr();
    const Addr mem_addr = toMemAddr(phys_addr);
    bool needsResponse = pkt->needsResponse();
    bool cacheResponding = pkt->cacheResponding();

    if (needsResponse && !cacheResponding) {
        pkt->pushSenderState(new HomeAgentSenderState(phys_addr));
    }

    pkt->setPhysAddr(phys_addr);
    pkt->setAddr(mem_addr);

    // Attempt to send the packet
    bool successful = masterPort.sendTimingReq(pkt);

    // If not successful, restore the address and sender state
    if (!successful) {
        pkt->invalidPhysAddr();
        pkt->setAddr(phys_addr);
        if (needsResponse) {
            delete pkt->popSenderState();
        }
    }

    return successful;
}

bool
HomeAgent::recvTimingResp(PacketPtr pkt)
{
    HomeAgentSenderState* receivedState =
        dynamic_cast<HomeAgentSenderState*>(pkt->senderState);

    // Restore initial sender state
    if (receivedState == NULL) {
        panic("HomeAgent %s got a response without sender state\n",
              name());
    }

    const Addr phys_addr = pkt->getPhysAddr();
    const Addr mem_addr = pkt->getAddr();

    // Restore the state and address
    pkt->senderState = receivedState->predecessor;
    pkt->invalidPhysAddr();
    pkt->setAddr(receivedState->physAddr);

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
        pkt->setPhysAddr(phys_addr);
        pkt->setAddr(mem_addr);
    }
    return successful;
}

void
HomeAgent::recvTimingSnoopReq(PacketPtr pkt)
{
    slavePort.sendTimingSnoopReq(pkt);
}

bool
HomeAgent::recvTimingSnoopResp(PacketPtr pkt)
{
    return masterPort.sendTimingSnoopResp(pkt);
}

bool
HomeAgent::isSnooping() const
{
    if (slavePort.isSnooping()) {
        fatal("HomeAgent doesn't support remapping of snooping requests\n");
    }
    return false;
}

void
HomeAgent::recvReqRetry()
{
    slavePort.sendRetryReq();
}

void
HomeAgent::recvRespRetry()
{
    masterPort.sendRetryResp();
}

void
HomeAgent::recvRangeChange()
{
    slavePort.sendRangeChange();
}

AddrRangeList
HomeAgent::getAddrRanges() const
{
    // Simply return the original ranges as given by the parameters
    AddrRangeList ranges(physRanges.begin(), physRanges.end());
    return ranges;
}

Addr
HomeAgent::toMemAddr(Addr addr) const
{
    for (int i = 0; i < physRanges.size(); ++i) {
        if (physRanges[i].contains(addr)) {
            Addr offset = addr - physRanges[i].start();
            return offset + memRanges[i].start();
        }
    }

    panic("HomeAgent: address '0x%x' out of range!\n", addr);
    return addr;
}

HomeAgent*
HomeAgentParams::create()
{
    return new HomeAgent(this);
}
