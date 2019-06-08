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

#include "mem/coward_addr_mapper.hh"

CowardAddrMapper::CowardAddrMapper(const CowardAddrMapperParams* p)
    : ClockedObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      originalRanges(p->original_ranges),
      remappedRanges(p->remapped_ranges),
      verbose(p->verbose)
{
    if (originalRanges.size() != remappedRanges.size())
        fatal("CowardAddrMapper: original and shadowed range list must "
              "be same size\n");

    for (size_t x = 0; x < originalRanges.size(); x++) {
        if (originalRanges[x].size() != remappedRanges[x].size())
            fatal("CowardAddrMapper: original and shadowed range list elements"
                  " aren't all of the same size\n");
    }
}

void
CowardAddrMapper::init()
{
    if (!slavePort.isConnected() || !masterPort.isConnected()) {
        fatal("Address mapper is not connected on both sides.\n");
    }

    if (verbose) {
        std::cout << "[cc] CowardAddrMapper::init(): "
                  << "name=" << name() << std::endl;
        for (size_t x = 0; x < originalRanges.size(); x++) {
            std::cout << "\t"
                    << originalRanges[x].to_string()
                    << " -> "
                    << remappedRanges[x].to_string()
                    << std::endl;
        }
    }
}

Port &
CowardAddrMapper::getPort(const std::string &if_name, PortID idx)
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
CowardAddrMapper::recvFunctional(PacketPtr pkt)
{
    Addr orig_addr = pkt->getAddr();
    pkt->setAddr(remapAddr(orig_addr));
    masterPort.sendFunctional(pkt);
    pkt->setAddr(orig_addr);
}

void
CowardAddrMapper::recvFunctionalSnoop(PacketPtr pkt)
{
    Addr orig_addr = pkt->getAddr();
    pkt->setAddr(remapAddr(orig_addr));
    slavePort.sendFunctionalSnoop(pkt);
    pkt->setAddr(orig_addr);
}

Tick
CowardAddrMapper::recvAtomic(PacketPtr pkt)
{
    Addr orig_addr = pkt->getAddr();
    pkt->setAddr(remapAddr(orig_addr));
    Tick ret_tick = masterPort.sendAtomic(pkt);
    pkt->setAddr(orig_addr);
    return ret_tick;
}

Tick
CowardAddrMapper::recvAtomicSnoop(PacketPtr pkt)
{
    Addr orig_addr = pkt->getAddr();
    pkt->setAddr(remapAddr(orig_addr));
    Tick ret_tick = slavePort.sendAtomicSnoop(pkt);
    pkt->setAddr(orig_addr);
    return ret_tick;
}

bool
CowardAddrMapper::recvTimingReq(PacketPtr pkt)
{
    Addr orig_addr = pkt->getAddr();
    bool needsResponse = pkt->needsResponse();
    bool cacheResponding = pkt->cacheResponding();

    if (needsResponse && !cacheResponding) {
        pkt->pushSenderState(new CowardAddrMapperSenderState(orig_addr));
    }

    pkt->setAddr(remapAddr(orig_addr));

    // Attempt to send the packet
    bool successful = masterPort.sendTimingReq(pkt);

    // If not successful, restore the address and sender state
    if (!successful) {
        pkt->setAddr(orig_addr);

        if (needsResponse) {
            delete pkt->popSenderState();
        }
    }

    return successful;
}

bool
CowardAddrMapper::recvTimingResp(PacketPtr pkt)
{
    CowardAddrMapperSenderState* receivedState =
        dynamic_cast<CowardAddrMapperSenderState*>(pkt->senderState);

    // Restore initial sender state
    if (receivedState == NULL)
        panic("CowardAddrMapper %s got a response without sender state\n",
              name());

    Addr remapped_addr = pkt->getAddr();

    // Restore the state and address
    pkt->senderState = receivedState->predecessor;
    pkt->setAddr(receivedState->origAddr);

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
        pkt->setAddr(remapped_addr);
    }
    return successful;
}

void
CowardAddrMapper::recvTimingSnoopReq(PacketPtr pkt)
{
    slavePort.sendTimingSnoopReq(pkt);
}

bool
CowardAddrMapper::recvTimingSnoopResp(PacketPtr pkt)
{
    return masterPort.sendTimingSnoopResp(pkt);
}

bool
CowardAddrMapper::isSnooping() const
{
    if (slavePort.isSnooping())
        fatal("CowardAddrMapper doesn't support remapping of snooping requests\n");
    return false;
}

void
CowardAddrMapper::recvReqRetry()
{
    slavePort.sendRetryReq();
}

void
CowardAddrMapper::recvRespRetry()
{
    masterPort.sendRetryResp();
}

void
CowardAddrMapper::recvRangeChange()
{
    slavePort.sendRangeChange();
}

AddrRangeList
CowardAddrMapper::getAddrRanges() const
{
    // Simply return the original ranges as given by the parameters
    AddrRangeList ranges(originalRanges.begin(), originalRanges.end());
    return ranges;
}

Addr
CowardAddrMapper::remapAddr(Addr addr) const
{
    for (int i = 0; i < originalRanges.size(); ++i) {
        if (originalRanges[i].contains(addr)) {
            Addr offset = addr - originalRanges[i].start();
            return offset + remappedRanges[i].start();
        }
    }

    panic("CowardAddrMapper: address '0x%x' out of range!\n", addr);
    return addr;
}

CowardAddrMapper*
CowardAddrMapperParams::create()
{
    return new CowardAddrMapper(this);
}
