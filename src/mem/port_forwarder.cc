/*
 * Copyright (c) 2011-2015, 2018 ARM Limited
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
 * Copyright (c) 2006 The Regents of The University of Michigan
 * All rights reserved.
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
 * Authors: Ali Saidi
 *          Andreas Hansson
 *          William Wang
 */

/**
 * @file
 * Definition of a non-coherent crossbar object.
 */

#include "mem/port_forwarder.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/PortForwarder.hh"

PortForwarder::PortForwarder(const PortForwarderParams *p)
    : ZeroXBar(p)
{
    // create the ports based on the size of the master and slave
    // vector ports, and the ports are enumerated starting from zero
    for (int i = 0; i < p->port_master_connection_count; ++i) {
        std::string portName = csprintf("%s.master[%d]", name(), i);
        MasterPort* bp = new PortForwarderMasterPort(portName, *this, i);
        masterPorts.push_back(bp);
        reqLayers.push_back(new ReqLayer(*bp, *this,
                                         csprintf(".reqLayer%d", i)));
    }

    // create the slave ports, once again starting at zero
    for (int i = 0; i < p->port_slave_connection_count; ++i) {
        std::string portName = csprintf("%s.slave[%d]", name(), i);
        SlavePort* bp = new PortForwarderSlavePort(portName, *this, i);
        slavePorts.push_back(bp);
        respLayers.push_back(new RespLayer(*bp, *this,
                                           csprintf(".respLayer%d", i)));
    }
}

PortForwarder::~PortForwarder()
{
    for (auto l: reqLayers)
        delete l;
    for (auto l: respLayers)
        delete l;
}

bool
PortForwarder::recvTimingReq(PacketPtr pkt, PortID slave_port_id)
{
    // determine the source port based on the id
    SlavePort *src_port = slavePorts[slave_port_id];

    // we should never see express snoops on a non-coherent crossbar
    assert(!pkt->isExpressSnoop());

    // determine the destination based on the address
    AddrRange addr_range = RangeSize(pkt->getAddr(), pkt->getSize());
    PortID master_port_id = findPort(addr_range);

    // test if the layer should be considered occupied for the current
    // port
    if (!reqLayers[master_port_id]->tryTiming(src_port)) {
        DPRINTF(PortForwarder, "recvTimingReq: src %s %s 0x%x BUSY\n",
                src_port->name(), pkt->cmdString(), pkt->getAddr());
        return false;
    }

    DPRINTF(PortForwarder, "recvTimingReq: src %s %s 0x%x\n",
            src_port->name(), pkt->cmdString(), pkt->getAddr());

    // store size and command as they might be modified when
    // forwarding the packet
    unsigned int pkt_size = pkt->hasData() ? pkt->getSize() : 0;
    unsigned int pkt_cmd = pkt->cmdToIndex();

    // before forwarding the packet (and possibly altering it),
    // remember if we are expecting a response
    const bool expect_response = pkt->needsResponse() &&
        !pkt->cacheResponding();

    // since it is a normal request, attempt to send the packet
    bool success = masterPorts[master_port_id]->sendTimingReq(pkt);

    if (!success)  {
        DPRINTF(PortForwarder, "recvTimingReq: src %s %s 0x%x RETRY\n",
                src_port->name(), pkt->cmdString(), pkt->getAddr());
        reqLayers[master_port_id]->failedTiming(src_port, curTick());
        return false;
    }

    // remember where to route the response to
    if (expect_response) {
        assert(routeTo.find(pkt->req) == routeTo.end());
        routeTo[pkt->req] = slave_port_id;
    }

    reqLayers[master_port_id]->succeededTiming(curTick());

    // stats updates
    pktCount[slave_port_id][master_port_id]++;
    pktSize[slave_port_id][master_port_id] += pkt_size;
    transDist[pkt_cmd]++;

    return true;
}

bool
PortForwarder::recvTimingResp(PacketPtr pkt, PortID master_port_id)
{
    // determine the source port based on the id
    MasterPort *src_port = masterPorts[master_port_id];

    // determine the destination
    const auto route_lookup = routeTo.find(pkt->req);
    assert(route_lookup != routeTo.end());
    const PortID slave_port_id = route_lookup->second;
    assert(slave_port_id != InvalidPortID);
    assert(slave_port_id < respLayers.size());

    // test if the layer should be considered occupied for the current
    // port
    if (!respLayers[slave_port_id]->tryTiming(src_port)) {
        DPRINTF(PortForwarder, "recvTimingResp: src %s %s 0x%x BUSY\n",
                src_port->name(), pkt->cmdString(), pkt->getAddr());
        return false;
    }

    DPRINTF(PortForwarder, "recvTimingResp: src %s %s 0x%x\n",
            src_port->name(), pkt->cmdString(), pkt->getAddr());

    // store size and command as they might be modified when
    // forwarding the packet
    unsigned int pkt_size = pkt->hasData() ? pkt->getSize() : 0;
    unsigned int pkt_cmd = pkt->cmdToIndex();

    // send the packet through the destination slave port
    bool success = slavePorts[slave_port_id]->sendTimingResp(pkt);

    if (!success)  {
        DPRINTF(PortForwarder, "recvTimingResp: src %s %s 0x%x RETRY\n",
                src_port->name(), pkt->cmdString(), pkt->getAddr());
        respLayers[slave_port_id]->failedTiming(src_port, curTick());
        return false;
    }

    // remove the request from the routing table
    routeTo.erase(route_lookup);

    respLayers[slave_port_id]->succeededTiming(curTick());

    // stats updates
    pktCount[slave_port_id][master_port_id]++;
    pktSize[slave_port_id][master_port_id] += pkt_size;
    transDist[pkt_cmd]++;

    return true;
}

void
PortForwarder::recvReqRetry(PortID master_port_id)
{
    // responses never block on forwarding them, so the retry will
    // always be coming from a port to which we tried to forward a
    // request
    reqLayers[master_port_id]->recvRetry();
}

void
PortForwarder::recvRespRetry(PortID slave_port_id)
{
    // responses never block on forwarding them, so the retry will
    // always be coming from a port to which we tried to forward a
    // request
    respLayers[slave_port_id]->recvRetry();
}

Tick
PortForwarder::recvAtomic(PacketPtr pkt, PortID slave_port_id)
{
    DPRINTF(PortForwarder, "recvAtomic: packet src %s addr 0x%x cmd %s\n",
            slavePorts[slave_port_id]->name(), pkt->getAddr(),
            pkt->cmdString());

    unsigned int pkt_size = pkt->hasData() ? pkt->getSize() : 0;
    unsigned int pkt_cmd = pkt->cmdToIndex();

    // determine the destination port
    AddrRange addr_range = RangeSize(pkt->getAddr(), pkt->getSize());
    PortID master_port_id = findPort(addr_range);

    // stats updates for the request
    pktCount[slave_port_id][master_port_id]++;
    pktSize[slave_port_id][master_port_id] += pkt_size;
    transDist[pkt_cmd]++;

    // forward the request to the appropriate destination
    Tick response_latency = masterPorts[master_port_id]->sendAtomic(pkt);

    // add the response data
    if (pkt->isResponse()) {
        pkt_size = pkt->hasData() ? pkt->getSize() : 0;
        pkt_cmd = pkt->cmdToIndex();

        // stats updates
        pktCount[slave_port_id][master_port_id]++;
        pktSize[slave_port_id][master_port_id] += pkt_size;
        transDist[pkt_cmd]++;
    }

    return response_latency;
}

void
PortForwarder::recvFunctional(PacketPtr pkt, PortID slave_port_id)
{
    if (!pkt->isPrint()) {
        // don't do DPRINTFs on PrintReq as it clutters up the output
        DPRINTF(PortForwarder,
                "recvFunctional: packet src %s addr 0x%x cmd %s\n",
                slavePorts[slave_port_id]->name(), pkt->getAddr(),
                pkt->cmdString());
    }

    // determine the destination port
    AddrRange addr_range = RangeSize(pkt->getAddr(), pkt->getSize());
    PortID dest_id = findPort(addr_range);

    // forward the request to the appropriate destination
    masterPorts[dest_id]->sendFunctional(pkt);
}

PortForwarder*
PortForwarderParams::create()
{
    return new PortForwarder(this);
}

void
PortForwarder::regStats()
{
    // register the stats of the base class and our layers
    ZeroXBar::regStats();
    for (auto l: reqLayers)
        l->regStats();
    for (auto l: respLayers)
        l->regStats();
}
