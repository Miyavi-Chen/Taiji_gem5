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

#ifndef __MEM_FLEX_MEM_HH__
#define __MEM_FLEX_MEM_HH__

#include "mem/mem_object.hh"
#include "params/FlexMem.hh"

/**
 * An address mapper changes the packet addresses in going from the
 * slave port side of the mapper to the master port side. When the
 * slave port is queried for the address ranges, it also performs the
 * necessary range updates. Note that snoop requests that travel from
 * the master port (i.e. the memory side) to the slave port are
 * currently not modified.
 */

class FlexMem : public MemObject
{

  public:

    FlexMem(const FlexMemParams* params);

    virtual ~FlexMem() { }

    virtual BaseSlavePort& getSlavePort(const std::string& if_name,
                                        PortID idx = InvalidPortID);

    virtual BaseMasterPort& getMasterPort(const std::string& if_name,
                                          PortID idx = InvalidPortID);

    virtual void init();

  protected:

    class FlexMemSenderState : public Packet::SenderState
    {

      public:

        /**
         * Construct a new sender state to remember the memory address.
         *
         * @param _memAddr Address before remapping
         */
        FlexMemSenderState(Addr _memAddr) : memAddr(_memAddr)
        { }

        /** Destructor */
        ~FlexMemSenderState() { }

        /** The memory address the packet was destined for */
        Addr memAddr;

    };

    class MapperSlavePort : public SlavePort
    {

      public:

        MapperSlavePort(const std::string& _name, FlexMem& _mapper)
            : SlavePort(_name, &_mapper), mapper(_mapper)
        { }

      protected:

        void recvFunctional(PacketPtr pkt)
        {
            mapper.recvFunctional(pkt);
        }

        Tick recvAtomic(PacketPtr pkt)
        {
            return mapper.recvAtomic(pkt);
        }

        bool recvTimingReq(PacketPtr pkt)
        {
            return mapper.recvTimingReq(pkt);
        }

        bool recvTimingSnoopResp(PacketPtr pkt)
        {
            return mapper.recvTimingSnoopResp(pkt);
        }

        AddrRangeList getAddrRanges() const
        {
            return mapper.getAddrRanges();
        }

        void recvRespRetry()
        {
            mapper.recvRespRetry();
        }

      private:

        FlexMem& mapper;

    };

    /** Instance of slave port, i.e. on the CPU side */
    MapperSlavePort slavePort;

    class MapperMasterPort : public MasterPort
    {

      public:

        MapperMasterPort(const std::string& _name, FlexMem& _mapper)
            : MasterPort(_name, &_mapper), mapper(_mapper)
        { }

      protected:

        void recvFunctionalSnoop(PacketPtr pkt)
        {
            mapper.recvFunctionalSnoop(pkt);
        }

        Tick recvAtomicSnoop(PacketPtr pkt)
        {
            return mapper.recvAtomicSnoop(pkt);
        }

        bool recvTimingResp(PacketPtr pkt)
        {
            return mapper.recvTimingResp(pkt);
        }

        void recvTimingSnoopReq(PacketPtr pkt)
        {
            mapper.recvTimingSnoopReq(pkt);
        }

        void recvRangeChange()
        {
            mapper.recvRangeChange();
        }

        bool isSnooping() const
        {
            return mapper.isSnooping();
        }

        void recvReqRetry()
        {
            mapper.recvReqRetry();
        }

      private:

        FlexMem& mapper;

    };

    /** Instance of master port, facing the memory side */
    MapperMasterPort masterPort;

    /**
     * This contains a list of ranges the should be remapped. It must
     * be the exact same length as channelRanges which describes what
     * manipulation should be done to each range.
     */
    std::vector<AddrRange> memRanges;

    /**
     * This contains a list of ranges that addresses should be
     * remapped to. See the description for memRanges above
     */
    std::vector<AddrRange> channelRanges;

    const bool verbose;

    System *const sys;

    const Addr intlvSize;

    const Addr intlvCoverSize;

    std::vector<AddrRange> memIntlvZone;

    std::vector<AddrRange> channelIntlvZone;

    std::vector<AddrRange> memSingleZone;

    std::vector<AddrRange> channelSingleZone;

    void recvFunctional(PacketPtr pkt);

    void recvFunctionalSnoop(PacketPtr pkt);

    Tick recvAtomic(PacketPtr pkt);

    Tick recvAtomicSnoop(PacketPtr pkt);

    bool recvTimingReq(PacketPtr pkt);

    bool recvTimingResp(PacketPtr pkt);

    void recvTimingSnoopReq(PacketPtr pkt);

    bool recvTimingSnoopResp(PacketPtr pkt);

    bool isSnooping() const;

    void recvReqRetry();

    void recvRespRetry();

    void recvRangeChange();

    AddrRangeList getAddrRanges() const;

    Addr toChannelAddr(const Addr addr, const unsigned int size) const;

    bool isAscending(const std::vector<AddrRange> &ranges) const;

    Addr totalSizeOf(const std::vector<AddrRange> &ranges) const;
};

#endif //__MEM_FLEX_MEM_HH__
