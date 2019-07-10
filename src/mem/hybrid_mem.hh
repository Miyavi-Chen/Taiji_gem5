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

#ifndef __MEM_HYBRID_MEM_HH__
#define __MEM_HYBRID_MEM_HH__

#include <deque>
#include <vector>
#include <map>
#include <iostream>
#include <utility>

#include "mem/dram_ctrl.hh"
#include "mem/lfu.hh"
#include "mem/lru.hh"
#include "mem/mem_addr_type.hh"
#include "mem/mem_object.hh"
#include "base/statistics.hh"
#include "params/HybridMem.hh"

/**
 * An address mapper changes the packet addresses in going from the
 * slave port side of the mapper to the master port side. When the
 * slave port is queried for the address ranges, it also performs the
 * necessary range updates. Note that snoop requests that travel from
 * the master port (i.e. the memory side) to the slave port are
 * currently not modified.
 */
class DRAMCtrl;
class HybridMem : public ClockedObject
{
  friend class DRAMCtrl;
  public:

    enum WaitState {
      wNONE = 2,
      wMASTER,
      wMASTER_wSELF,
      wSELF,
      wSELF_wMASTER,
      tryMASTER,
      tryMASTER_wSELF,
      trySELF,
      trySELF_wMASTER,
    };

    enum SentState {
      sIDLE = 2,
      sMASTER,
      sSELF,
    };

    HybridMem(const HybridMemParams* params);

    virtual void init() override;

    virtual void startup() override;

    virtual void regStats() override;

    void resetStats();

    class HybridmemResetCallback : public Callback
    {
      private:
        HybridMem *hybridmem;

      public:
        HybridmemResetCallback(HybridMem *m) : hybridmem(m) {}
        virtual void process() { hybridmem->resetStats(); };
    };

    /** A function used to return the port associated with this object. */
    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

  protected:

    class BinEntry
    {
      public:
        BinEntry(uint64_t _pagesPerBin)
        : needRef(false), full(false), pagesPerBin(_pagesPerBin)
        {
          for (int i = 0; i < pagesPerBin; ++i) {
              allocatedPagesPerBin.push_back(0);
          }
        }

        bool needRefresh () {return needRef;}
        bool isFull () {return full;}

        int firstFreeFrame ()
        {
          assert(!full);
          for (int i = 0; i < allocatedPagesPerBin.size(); ++i) {
            if (!allocatedPagesPerBin[i]) { return i;}
          }
          return -1;
        }

        void binUpdate ()
        {
          full = allocatedPagesPerBin[0];
          needRef = allocatedPagesPerBin[0];
          for (int i = 1; i < allocatedPagesPerBin.size(); ++i) {
            full &= allocatedPagesPerBin[i];
            needRef |= allocatedPagesPerBin[i];
          }
        }

        void binSet (int num)
        {
          allocatedPagesPerBin[num] = true;
          binUpdate();
        }

        void binReset (int num)
        {
          allocatedPagesPerBin[num] = false;
          binUpdate();
        }

      private:
        bool needRef;
        bool full;
        uint64_t pagesPerBin;
        std::vector<bool> allocatedPagesPerBin;
    };

    class BinsOfRank
    {
      public:
        BinsOfRank(uint64_t _pagesPerBin)
        : full(false), nextFreeBin(0), pagesPerBin(_pagesPerBin)
        {
          for (int i=0; i < TOTALBINS; ++i) {
            bins.push_back(BinEntry(pagesPerBin));
          }
        }

        size_t getFreeFrame()
        {
          for (int i = nextFreeBin; i < bins.size();
                ++i, nextFreeBin = (nextFreeBin + 1) % TOTALBINS) {
            if (!bins[i].isFull()) {
              int idx = bins[i].firstFreeFrame();
              assert(idx != -1);
              // std::cout<<"BinsOfRank get bin "<<i<<" ";
              // std::cout<<"get idx "<<idx<<"\n";
              return static_cast<size_t>(idx + i * pagesPerBin);
            }
          }

          full = true;
          return std::numeric_limits<size_t>::max();
        }

        bool isFull() { return full;}

        bool needRefresh (size_t binNum)
        {
          return bins[binNum].needRefresh();
        }

        void allocFrame (size_t binNum, size_t idx)
        {
          bins[binNum].binSet(idx);
        }

        void releaseFrame (size_t binNum, size_t idx)
        {
          bins[binNum].binReset(idx);
          if (nextFreeBin > binNum) {
            std::cout<<"Free bin change from "<<nextFreeBin<<" "<<binNum<<"\n";
            nextFreeBin = binNum;
          }
        }

      private:
        std::vector<class BinEntry> bins;
        bool full;
        size_t nextFreeBin;
        uint64_t pagesPerBin;
    };

    class BinsInRanks
    {
      public:
        BinsInRanks(DRAMCtrl *_ctrlptrs)
        : ctrlptrs(_ctrlptrs),
        ranksPerChannel(ctrlptrs->ranksPerChannel),
        pagesPerBin(ctrlptrs->pagesPerBin),
        rowsPerBin(ctrlptrs->rowsPerBin), nextRank(0)
        {
          for (int i=0; i < ranksPerChannel; ++i) {
            binsInRanks.push_back(BinsOfRank(pagesPerBin));
          }
          std::cout<<"\nInit BinsInRanks\n";
        }

        size_t getFreeFrame ()
        {
          size_t pageNumOfRank = {std::numeric_limits<size_t>::max()};
          for (int cnt=0; cnt < ranksPerChannel;
                ++cnt, nextRank = (nextRank+1)% ranksPerChannel) {
            if (!binsInRanks[nextRank].isFull()) {
              pageNumOfRank = binsInRanks[nextRank].getFreeFrame();
              if (pageNumOfRank != std::numeric_limits<size_t>::max()) {
                pageNumOfRank = pageNumOfRank*ranksPerChannel + nextRank;
                break;
              }
            }
          }

          nextRank = (nextRank + 1) % ranksPerChannel;
          return pageNumOfRank;
        }

        void allocFrame (size_t frameNum)
        {
          size_t pageNumOfRank = frameNum / ranksPerChannel;
          size_t rank = frameNum % ranksPerChannel;
          size_t binNum = pageNumOfRank / pagesPerBin;
          size_t idx = pageNumOfRank % pagesPerBin;
          binsInRanks[rank].allocFrame(binNum, idx);
          updateCtrlRefTable(rank, binNum);
          // std::cout<<"BinsOfRank alloc bin "<<binNum<<" ";
          // std::cout<<"alloc idx "<<idx<<"\n";
        }

        void releaseFrame (size_t frameNum)
        {
          size_t pageNumOfRank = frameNum / ranksPerChannel;
          size_t rank = frameNum % ranksPerChannel;
          size_t binNum = pageNumOfRank / pagesPerBin;
          size_t idx = pageNumOfRank % pagesPerBin;
          binsInRanks[rank].releaseFrame(binNum, idx);
          updateCtrlRefTable(rank, binNum);
          std::cout<<"BinsOfRank free bin "<<binNum<<" ";
          std::cout<<"idx "<<idx<<"\n";
        }

        void updateCtrlRefTable (size_t rank, size_t binNum)
        {
          bool isValid = false;
          if (binsInRanks[rank].needRefresh(binNum)) {
            isValid = true;
          } else {
            //NA
          }
          ctrlptrs->updateCtrlRefTable(rank, binNum, isValid);
        }

      private:
        DRAMCtrl *ctrlptrs;
        std::vector<class BinsOfRank> binsInRanks;
        const uint32_t ranksPerChannel;
        const uint64_t pagesPerBin;
        const uint64_t rowsPerBin;
        uint32_t nextRank;

    };


    class FrameInfo
    {

      public:

        enum FrameInfoState {
          FREE = 2,
          OCCUPIED,
        };

        FrameInfo() : state(FrameInfoState::FREE)
        { owner.val = std::numeric_limits<Addr>::max(); }

        bool isFree() { return state == FrameInfoState::FREE; }

        bool isOccupied() { return state == FrameInfoState::OCCUPIED; }

        void assignOwner(struct PageAddr _owner)
        {
          assert(state == FrameInfoState::FREE);
          assert(owner.val == std::numeric_limits<Addr>::max());
          owner = _owner;
          state = FrameInfoState::OCCUPIED;
        }

        void eraseOwner(struct PageAddr _assigned)
        {
          assert(state != FrameInfoState::FREE);
          assert(owner.val == _assigned.val);
          owner.val = std::numeric_limits<Addr>::max();
          state = FrameInfoState::FREE;
        }

        struct PageAddr getOwner()
        {
          assert(state != FrameInfoState::FREE);
          return owner;
        }

      private:

        enum FrameInfoState state;
        struct PageAddr owner;

    };

    class FramePool
    {

      public:

        FramePool(AddrRange _range, Addr _frameSize)
          : RANGE(_range), FRAME_SIZE(_frameSize),
            frames(RANGE.size() / FRAME_SIZE),
            poolEmpty(false), poolUsed(false), nextTimeFrameIdx(0),
            nextEvictedFrameIdx(0),
            freeFrameSize(frames.size()), isDram(false), BinsInRanksPtr(nullptr)
        { }

        void setDRAM () {isDram = true;}
        void setRankBinsPtr (BinsInRanks *_BinsInRanksPtr) {
          BinsInRanksPtr = _BinsInRanksPtr;
        }

        bool isFull() {return poolEmpty;}

        bool tryGetAnyFreeFrame(struct FrameAddr *_frame)
        {
          _frame->val = std::numeric_limits<Addr>::max();
          if (poolEmpty) { return false; }
          //split dram/nvm here
          //dram: get free page from bins of ranks
          if (isDram) {
            size_t idx = BinsInRanksPtr->getFreeFrame();
            if (idx != std::numeric_limits<size_t>::max()) {
              assert(frames[idx].isFree());
              _frame->val = (idx * FRAME_SIZE) + RANGE.start();
              return true;
            }
          } else {
            assert(!isDram);
            assert(BinsInRanksPtr == nullptr);
            for (size_t i = 0; i < frames.size(); ++i) {
              size_t idx = ((nextTimeFrameIdx + i) % frames.size());
              if (frames[idx].isFree()) {
                _frame->val = (idx * FRAME_SIZE) + RANGE.start();
                nextTimeFrameIdx = ((idx + 1) % frames.size());
                return true;
              }
            }
          }

          poolEmpty = true;
          return false;
        }

        struct FrameAddr pickOneEvictedFrame()
        {
          struct FrameAddr _frame = {std::numeric_limits<Addr>::max()};
          for (size_t i = 0; i < frames.size(); ++i) {
            size_t idx = ((nextEvictedFrameIdx + i) % frames.size());
            if (!frames[idx].isFree()) {
              _frame.val = (idx * FRAME_SIZE) + RANGE.start();
              nextEvictedFrameIdx = ((idx + 1) % frames.size());
              return _frame;
            }
          }
          assert(_frame.val != std::numeric_limits<Addr>::max());
          return _frame;
        }

        void allocFrame(struct PageAddr _owner, struct FrameAddr _frame)
        {
          assert(RANGE.contains(_frame.val));
          size_t i = (_frame.val - RANGE.start()) / FRAME_SIZE;
          assert(i < frames.size());
          frames[i].assignOwner(_owner);
          --freeFrameSize;
          // assert(freeFrameSize >=0);
          if (isDram) { BinsInRanksPtr->allocFrame(i);}
          else {assert(!isDram); assert(BinsInRanksPtr == nullptr);}
        }
        void freeFrame(struct PageAddr _assigned, struct FrameAddr _frame)
        {
          assert(RANGE.contains(_frame.val));
          size_t i = (_frame.val - RANGE.start()) / FRAME_SIZE;
          assert(i < frames.size());
          frames[i].eraseOwner(_assigned);
          poolEmpty = false;
          ++freeFrameSize;
          if (isDram) { BinsInRanksPtr->releaseFrame(i);}
          else {assert(!isDram); assert(BinsInRanksPtr == nullptr);}
        }

        struct PageAddr getOwner(struct FrameAddr _frame)
        {
          assert(RANGE.contains(_frame.val));
          size_t i = (_frame.val - RANGE.start()) / FRAME_SIZE;
          assert(i < frames.size());
          return frames[i].getOwner();
        }

        void setPoolUsed()
        {
          assert(poolUsed == false);
          poolUsed = true;
        }

        bool isPoolUsed()
        {
          return poolUsed;
        }

        size_t getFreeFrameSize()
        {
          return freeFrameSize;
        }

      private:

        const AddrRange RANGE;
        const Addr FRAME_SIZE;
        std::vector<class FrameInfo> frames;
        bool poolEmpty;
        bool poolUsed;
        size_t nextTimeFrameIdx;
        size_t nextEvictedFrameIdx;
        size_t freeFrameSize;
        bool isDram;
        BinsInRanks *BinsInRanksPtr;

    };

    class FramePools
    {

      public:

        FramePools(std::vector<AddrRange> _ranges, Addr _frameSize)
          : RANGES(_ranges)
        {
          for (size_t i = 0; i < RANGES.size(); ++i) {
            ranges.push_back(FramePool(RANGES[i], _frameSize));
          }
        }

        bool tryGetAnyFreeFrame(struct ChannelIdx *_ch, struct FrameAddr *_fr)
        {
          _ch->val = std::numeric_limits<size_t>::max();
          _fr->val = std::numeric_limits<Addr>::max();
          for (size_t i = 0; i < ranges.size(); ++i) {
            if (ranges[i].tryGetAnyFreeFrame(_fr)) {
              _ch->val = i;
              return true;
            }
          }
          return false;
        }

        struct ChannelIdx channelIdxOf(struct FrameAddr _frame)
        {
          for (size_t i = 0; i < RANGES.size(); ++i) {
            if (RANGES[i].contains(_frame.val)) {
              struct ChannelIdx idx; idx.val = i; return idx;
            }
          }
          assert(0);
        }

        class FramePool *poolOf(struct ChannelIdx _idx)
        {
          assert(_idx.val < ranges.size());
          return &(ranges[_idx.val]);
        }

        class FramePool *poolOf(struct FrameAddr _frame)
        {
          return poolOf(channelIdxOf(_frame));
        }

      private:

        const std::vector<AddrRange> RANGES;
        std::vector<class FramePool> ranges;

    };

    class ChannelInfo
    {

      public:

        enum ChannelInfoState {
          UNUSED = 2,
          INVALID,
          VALID,
        };

        ChannelInfo() : state(ChannelInfoState::UNUSED)
        { possession.val = std::numeric_limits<Addr>::max(); }

        bool isUnused() { return state == ChannelInfoState::UNUSED; }

        bool isInvalid() { return state == ChannelInfoState::INVALID; }

        bool isValid() { return state == ChannelInfoState::VALID; }

        void assignPossession(struct FrameAddr _possession)
        {
          assert(state == ChannelInfoState::UNUSED);
          assert(possession.val == std::numeric_limits<Addr>::max());
          possession = _possession;
          state = ChannelInfoState::INVALID;
        }

        void erasePossession(struct FrameAddr _assigned)
        {
          assert(state != ChannelInfoState::UNUSED);
          assert(possession.val == _assigned.val);
          possession.val = std::numeric_limits<Addr>::max();
          state = ChannelInfoState::UNUSED;
        }

        void validate()
        {
          assert(state != ChannelInfoState::UNUSED);
          state = ChannelInfoState::VALID;
        }

        void invalidate()
        {
          assert(state != ChannelInfoState::UNUSED);
          state = ChannelInfoState::INVALID;
        }

        struct FrameAddr getPossession()
        {
          assert(state != ChannelInfoState::UNUSED);
          return possession;
        }

      private:

        enum ChannelInfoState state;
        struct FrameAddr possession;

    };

    class Page
    {

      public:

        Page(class FramePools &_pools, size_t _channels)
          : pools(_pools), channels(_channels),
            migrating(false), masterWaiting(false),
            masterWaitingTick(std::numeric_limits<Tick>::max())
        {
          pageAddr.val = std::numeric_limits<Addr>::max();

          readcount = writecount = migrationCount = 0;
          RowBufMissRd = RowBufMissWr = 0;
          wirdDirtyBit = false;
          justMigrate = isDirty = isInDram = isPageCache = false;
          migrationIntervalTotal = lastMigrationInterval = 0;
          RWScoresPerInterval = 0;
          predictRowHit = predictRowMiss = lastAccessTick = 0;
          readreqPerInterval = writereqPerInterval = 0;
          lastAccessInterval = wirdIntervalNum = expireTime = 0;
        }

        int readcount;
        int writecount;
        int RowBufMissRd;
        int RowBufMissWr;
        uint64_t wirdIntervalNum;
        uint64_t expireTime;
        bool wirdDirtyBit;
        bool justMigrate;
        bool isDirty;
        bool isInDram;
        bool isPageCache;

        //observation
        int migrationCount;
        int migrationIntervalTotal;
        int lastMigrationInterval;

        //score include read and write
        size_t RWScoresPerInterval;

        std::vector<Tick> access_tick;
        size_t predictRowHit;
        size_t predictRowMiss;
        Tick lastAccessTick;

        size_t readreqPerInterval;
        size_t writereqPerInterval;

        uint64_t lastAccessInterval;

        void setPageAddr(struct PageAddr _addr) {
          assert(pageAddr.val == std::numeric_limits<Addr>::max());
          pageAddr.val = _addr.val;
        }

        struct PageAddr getPageAddr() { return pageAddr; }

        bool inMigrating() { return migrating; }

        bool masterIsWaiting() { return masterWaiting; }

        Tick getMasterWaitingTick() { return masterWaitingTick; }

        bool isUnused(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          return channels[_idx.val].isUnused();
        }

        bool isInvalid(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          return channels[_idx.val].isInvalid();
        }

        bool isValid(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          return channels[_idx.val].isValid();
        }

        bool isCanLaunch(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          return !migrating && channels[_idx.val].isValid();
        }

        void getUnusedChannelIdx(std::vector<struct ChannelIdx> *_idxSet)
        {
          _idxSet->clear();
          for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].isUnused()) {
              struct ChannelIdx idx; idx.val = i; _idxSet->push_back(idx);
            }
          }
        }

        void getInvalidChannelIdx(std::vector<struct ChannelIdx> *_idxSet)
        {
          _idxSet->clear();
          for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].isInvalid()) {
              struct ChannelIdx idx; idx.val = i; _idxSet->push_back(idx);
            }
          }
        }

        void getValidChannelIdx(std::vector<struct ChannelIdx> *_idxSet)
        {
          _idxSet->clear();
          for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i].isValid()) {
              struct ChannelIdx idx; idx.val = i; _idxSet->push_back(idx);
            }
          }
        }

        void getCanLaunchChannelIdx(std::vector<struct ChannelIdx> *_idxSet)
        {
          _idxSet->clear();
          if (!migrating) {
            getValidChannelIdx(_idxSet);
          }
        }

        void getCanLaunchChannelIdxFun(std::vector<struct ChannelIdx> *_idxSet)
        {
          _idxSet->clear();
            getValidChannelIdx(_idxSet);
        }

        void startMigration() {
          assert(!migrating);
          assert(!masterWaiting);
          migrating = true;
        }

        void bookingMasterWaiting() {
          assert(migrating);
          assert(!masterWaiting);
          masterWaiting = true;
          assert(masterWaitingTick == std::numeric_limits<Tick>::max());
          masterWaitingTick = curTick();
        }

        void cancelMasterWaiting() {
          assert(migrating);
          assert(masterWaiting);
          masterWaiting = false;
          assert(masterWaitingTick != std::numeric_limits<Tick>::max());
          masterWaitingTick = std::numeric_limits<Tick>::max();
        }

        void finishMigration() {
          assert(migrating);
          assert(!masterWaiting);
          migrating = false;
        }

        void allocFrame(struct ChannelIdx _idx, struct FrameAddr _frame)
        {
          assert(_idx.val < channels.size());
          pools.poolOf(_idx)->allocFrame(pageAddr, _frame);
          channels[_idx.val].assignPossession(_frame);
        }

        void freeFrame(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          struct FrameAddr frame = channels[_idx.val].getPossession();
          pools.poolOf(_idx)->freeFrame(pageAddr, frame);
          channels[_idx.val].erasePossession(frame);
        }

        void claimChannelIsValid(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          channels[_idx.val].validate();
        }

        void launchReadTo(struct ChannelIdx _idx)
        {
          assert(!migrating);
          assert(_idx.val < channels.size());
          assert(channels[_idx.val].isValid());
        }

        void launchWriteTo(struct ChannelIdx _idx)
        {
          assert(!migrating);
          assert(_idx.val < channels.size());
          assert(channels[_idx.val].isValid());
          for (size_t i = 0; i < channels.size(); ++i) {
            if (i == _idx.val) { continue; }
            if (channels[i].isValid()) { channels[i].invalidate(); }
          }
        }

        struct FrameAddr getFrameAddr(struct ChannelIdx _idx)
        {
          assert(_idx.val < channels.size());
          return channels[_idx.val].getPossession();
        }

        void resetPageCounters()
        {
          if (isPageCache) {readcount = -64; writecount = -64;}
          else {readcount = 0; writecount = 0;}
          //observation
          // migrationCount = 0;
          // migrationIntervalTotal = 0;
          // lastMigrationInterval = 0;

          //score include read and write
          RWScoresPerInterval = 0;

          justMigrate = false;
          RowBufMissRd = 0;
          RowBufMissWr = 0;

          predictRowHit = 0;
          predictRowMiss = 0;

          readreqPerInterval = 0;
          writereqPerInterval = 0;

          lastAccessTick = 0;
        }

      private:

        class FramePools &pools;
        struct PageAddr pageAddr;
        std::vector<class ChannelInfo> channels;
        bool migrating;
        bool masterWaiting;
        Tick masterWaitingTick;

    };

    class MemRange
    {

      public:

        MemRange(class FramePools &_pools, AddrRange _range,
                 Addr _pageSize, size_t _channels)
          : RANGE(_range), PAGE_SIZE(_pageSize),
            pages(RANGE.size() / PAGE_SIZE, {_pools, _channels})
        {
          for (size_t i = 0; i < pages.size(); ++i) {
            struct PageAddr pageAddr;
            pageAddr.val = (i * PAGE_SIZE) + RANGE.start();
            pages[i].setPageAddr(pageAddr);
          }
        }

        class Page *pageOf(struct PageAddr _pageAddr)
        {
          assert(RANGE.contains(_pageAddr.val));
          size_t i = (_pageAddr.val - RANGE.start()) / PAGE_SIZE;
          assert(i < pages.size());
          return &(pages[i]);
        }

        void resetPageCounters()
        {
          for (size_t i = 0; i < pages.size(); ++i) {
            pages[i].resetPageCounters();
          }
        }

      private:

        const AddrRange RANGE;
        const Addr PAGE_SIZE;
        std::vector<class Page> pages;

    };

    class MemRanges
    {

      public:

        MemRanges(class FramePools &_pools,
                  std::vector<AddrRange> _ranges,
                  Addr _pageSize, size_t _channels)
          : RANGES(_ranges)
        {
          for (size_t i = 0; i < RANGES.size(); ++i) {
            ranges.push_back(MemRange(_pools, RANGES[i], _pageSize, _channels));
          }
        }

        class Page *pageOf(struct PageAddr _pageAddr)
        {
          for (size_t i = 0; i < RANGES.size(); ++i) {
            if (RANGES[i].contains(_pageAddr.val)) {
              return ranges[i].pageOf(_pageAddr);
            }
          }
          assert(0);
        }

        void resetPageCounters()
        {
          for (size_t i = 0; i < ranges.size(); ++i) {
            ranges[i].resetPageCounters();
          }
        }

      private:

        const std::vector<AddrRange> RANGES;
        std::vector<class MemRange> ranges;

    };

    class MigrationTask
    {

      public:

        MigrationTask(MasterID _masterId, class Page *_page,
                      struct PhysAddr _physAddrBase,
                      struct ChannelIdx _from, struct ChannelIdx _to,
                      Addr _granularity, Addr _pktSize)
          : page(_page), issuedTick(curTick()), from(_from), to(_to),
            ongoingRd(_granularity / _pktSize), ongoingWr(ongoingRd)
        {
          assert(_granularity >= _pktSize);
          assert((_granularity % _pktSize) == 0);
          assert(from.val != to.val);
          assert(page->isValid(from));
          assert(!page->isUnused(to));
          assert(!page->inMigrating());
          page->startMigration();
          struct FrameAddr from_addr = page->getFrameAddr(from);
          struct FrameAddr to_addr = page->getFrameAddr(to);
          for (Addr shift = 0; shift < _granularity; shift += _pktSize) {
            const Addr phys_addr = _physAddrBase.val + shift;
            RequestPtr read_req = std::make_shared<Request>(
                from_addr.val + shift, _pktSize, 0, _masterId);
            PacketPtr read_pkt = Packet::createRead(read_req);
            read_pkt->setPhysAddr(phys_addr);
            read_pkt->dataDynamic(new uint8_t[_pktSize]);
            readPkt[phys_addr] = read_pkt;
            RequestPtr write_req = std::make_shared<Request>(
                to_addr.val + shift, _pktSize, 0, _masterId);
            PacketPtr write_pkt = Packet::createWrite(write_req);
            write_pkt->setPhysAddr(phys_addr);
            write_pkt->allocate();
            pendingWr[phys_addr] = write_pkt;
          }
          assert(readPkt.size() == ongoingRd);
          assert(pendingWr.size() == ongoingWr);
        }

        class Page *getPage() { return page; }

        Tick getIssuedTick() { return issuedTick; }

        bool tryGetReadPkt(PacketPtr *_pkt) {
          *_pkt = nullptr;
          if (readPkt.empty()) {
            return false;
          } else {
            *_pkt = readPkt.begin()->second;
            return true;
          }
        }

        bool tryGetWritePkt(PacketPtr *_pkt) {
          *_pkt = nullptr;
          if (writePkt.empty()) {
            return false;
          } else {
            *_pkt = writePkt.begin()->second;
            return true;
          }
        }

        bool tryGetAnyPkt(PacketPtr *_pkt) {
          *_pkt = nullptr;
          if (tryGetWritePkt(_pkt)) { return true; }
          if (tryGetReadPkt(_pkt)) { return true; }
          return false;
        }

        bool isDone()
        {
          return readPkt.empty() && issuedRd.empty() &&
                 pendingWr.empty() && writePkt.empty() && issuedWr.empty();
        }

        void issuePkt(PacketPtr pkt)
        {
          std::map<Addr, PacketPtr>::iterator issue;
          const Addr phys_addr = pkt->getPhysAddr();
          if ((issue = writePkt.find(phys_addr)) != writePkt.end()) {
            assert(issuedWr.find(phys_addr) == issuedWr.end());
            issuedWr[phys_addr] = issue->second;
            writePkt.erase(issue);
          } else if ((issue = readPkt.find(phys_addr)) != readPkt.end()) {
            assert(issuedRd.find(phys_addr) == issuedRd.end());
            issuedRd[phys_addr] = issue->second;
            readPkt.erase(issue);
          } else {
            assert(0);
          }
        }

        void finishPkt(PacketPtr pkt)
        {
          std::map<Addr, PacketPtr>::iterator issued;
          const Addr phys_addr = pkt->getPhysAddr();
          if ((issued = issuedWr.find(phys_addr)) != issuedWr.end()) {
            delete issued->second;
            issuedWr.erase(issued);
            assert(ongoingWr);
            --ongoingWr;
          } else if ((issued = issuedRd.find(phys_addr)) != issuedRd.end()) {
            std::map<Addr, PacketPtr>::iterator pending;
            assert((pending = pendingWr.find(phys_addr)) != pendingWr.end());
            pending->second->setData(issued->second->getConstPtr<uint8_t>());
            assert(writePkt.find(phys_addr) == writePkt.end());
            writePkt[phys_addr] = pending->second;
            pendingWr.erase(pending);
            delete issued->second;
            issuedRd.erase(issued);
            assert(ongoingRd);
            --ongoingRd;
          } else {
            assert(0);
          }
        }

        struct ChannelIdx targetChannel()
        {
          return to;
        }

        ~MigrationTask()
        {
          assert(!ongoingRd);
          assert(!ongoingWr);
          assert(isDone());
          assert(page->inMigrating());
          page->claimChannelIsValid(to);
          page->finishMigration();
        }

      private:

        class Page *page;
        const Tick issuedTick;
        struct ChannelIdx from;
        struct ChannelIdx to;
        size_t ongoingRd;
        size_t ongoingWr;
        std::map<Addr, PacketPtr> readPkt;
        std::map<Addr, PacketPtr> issuedRd;
        std::map<Addr, PacketPtr> pendingWr;
        std::map<Addr, PacketPtr> writePkt;
        std::map<Addr, PacketPtr> issuedWr;

    };

    class WIRD
		{
			private:
				int rollingIdx;
				int threshold;
        uint64_t reqsCounter;
        uint64_t intervalNum;

				class storage
				{
					public:
						uint64_t expireTime;
						class Page *page;

				};
				std::vector<storage> list;

			public:
				WIRD();
				class Page *getEvicPage();
				void putPage(class Page *);
        void popPage(class Page *);
        void setExpitreTime(class Page *);
        void intervalUpdatePCM();
        uint64_t getIntervalNum();
        int getThreshold();
        void reqsCounterUpdate();
		};
		WIRD* wird;

    class HybridMemSenderState : public Packet::SenderState
    {

      public:

        /**
         * Construct a new sender state to remember the memory address.
         *
         * @param _memAddr Address before remapping
         */
        HybridMemSenderState(Addr _memAddr) : memAddr(_memAddr)
        { }

        /** Destructor */
        ~HybridMemSenderState() { }

        /** The memory address the packet was destined for */
        Addr memAddr;

    };

    class MapperSlavePort : public SlavePort
    {

      public:

        MapperSlavePort(const std::string& _name, HybridMem& _mapper)
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

        HybridMem& mapper;

    };

    /** Instance of slave port, i.e. on the CPU side */
    MapperSlavePort slavePort;

    class MapperMasterPort : public MasterPort
    {

      public:

        MapperMasterPort(const std::string& _name, HybridMem& _mapper)
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

        HybridMem& mapper;

    };

    /** Instance of master port, facing the memory side */
    MapperMasterPort masterPort;

    std::vector<AddrRange> physRanges;

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

    // LFUDA DramLFUDA;
    // LFUDA PcmLFUDA;
    // LFU DramLFU;
    // LFU PcmLFU;

    std::unordered_set<Addr> mapRef;
    // LRU rankingDramLRU;
    // LRU rankingPcmLRU;

    class SortHostPage
    {
      public:
        Addr host_PN;
        Addr dram_PN;
        int value;
        int writecount;
        bool isDirty;

    };
    std::vector<SortHostPage> Ranking;

    class SortMigration
    {
      public:
        double avg_migration_interval;
        int host_PN;
        int migration_count;
    };

    const bool verbose;

    System *sys;

    const Addr cacheLineSize;

    const Addr granularity;

    const MasterID masterId;

    const Cycles headerDelay;

    const unsigned width;

    const unsigned maxMigrationTasks;

    WaitState wait;

    SentState sent;

    bool waitSlaveRetry;

    class FramePools pools;

    class MemRanges pages;

    EventFunctionWrapper migrationEvent;

    EventFunctionWrapper releaseSentStateEvent;

    std::deque<class MigrationTask *> migrationTasks;

    std::map<RequestPtr, class MigrationTask *> migrationReq;

    std::map<Addr, int> migrationPages;
    std::map<Addr, int> migrationPagesPI;

    void recvFunctional(PacketPtr pkt);

    void recvFunctionalSnoop(PacketPtr pkt);

    Tick recvAtomic(PacketPtr pkt);

    Tick recvAtomicSnoop(PacketPtr pkt);

    bool tryIssueMigrationTask(class Page *page,
          struct ChannelIdx _from, struct ChannelIdx _to);

    void flushAllMigrationPkt();

    void issueTimingMigrationPkt();

    bool recvTimingReq(PacketPtr pkt);

    bool recvTimingMigrationReq(class MigrationTask *task, PacketPtr pkt);

    void recvFunctionalMigrationReq(class MigrationTask *task, PacketPtr pkt);

    bool recvTimingResp(PacketPtr pkt);

    void recvFunctionalMigrationResp(PacketPtr pkt);

    bool recvTimingMigrationResp(PacketPtr pkt);

    void trySendRetry();

    void releaseSentState();

    void recvReqRetry();

    void recvTimingSnoopReq(PacketPtr pkt);

    bool recvTimingSnoopResp(PacketPtr pkt);

    bool isSnooping() const;

    void recvRespRetry();

    void recvRangeChange();

    AddrRangeList getAddrRanges() const;

    Addr toChannelAddr(class Page *page, struct ChannelIdx idx, PacketPtr pkt);

    struct ChannelIdx selectChannelIdx(class Page *page);

    struct ChannelIdx selectChannelIdxFun(class Page *page);

    struct ChannelIdx toChannelIdx(size_t i) const;

    struct PhysAddr toPhysAddr(struct PageAddr addr) const;

    struct PhysAddr toPhysAddr(class Page *page) const;

    struct PageAddr toPageAddr(Addr addr) const;

    struct PageAddr toMemAddr(Addr addr) const;

    struct PageAddr toPageAddr(PacketPtr pkt) const;

    struct FrameAddr toFrameAddrMemSide(Addr addr) const;

    struct PageAddr toPageAddrMemSide(Addr addr) const;

    struct FrameAddr toFrameAddr(Addr addr) const;
    struct FrameAddr toFrameAddr(PacketPtr pkt) const;

    bool isAscending(const std::vector<AddrRange> &ranges) const;

    Addr totalSizeOf(const std::vector<AddrRange> &ranges) const;

    bool canBeDrained();

    DrainState drain() override;

    void CountScoreinc(struct PageAddr, bool isRead, bool hit, uint64_t qlen);

    void ReadCountinc(class Page *page, uint64_t qlen);

    void WriteCountinc(class Page *page, uint64_t qlen);

    size_t addScoreToPage(class Page *page, size_t score);

    void getPageRanking(std::vector<Addr>& v);

    static bool PCMtoDRAMsort(const SortHostPage& a, const SortHostPage& b);
    static bool DRAMtoPCMsort(const SortHostPage& a, const SortHostPage& b);

    void genMigrationTasks(class Page * page, bool pcm2dram);
    void genDramEvictedMigrationTasks(size_t &halfMigrationPageNum);
    void genPcmMigrationTasks(class Page * page);
    void genDramMigrationTasks(class Page * page);
    void handlePFDMA(class Page * page, PacketPtr pkt);

    void predicRowHitOrMiss(class Page * page);

    void updateStatisticInfo();
    void updateBWInfo();
    void resetPerInterval();
    void resetPages();

    void statisticInfoCheck();
    void reqBlockedTickDiffUpdate();

    void incMissThres();
    void decMissThres();
    void incRowBufferMissCount(struct PageAddr pageaddr, size_t value, bool isRead);

    void processWarmUpEvent();
    EventFunctionWrapper warmUpEvent;

    void processRegularBalancedEvent();
    EventFunctionWrapper regularBalancedEvent;
    bool hasWarmedUp;

    std::vector<DRAMCtrl *> ctrlptrs;

    MasterID dmaDeviceId;
    class SimObject *dmaDevicePtr;

    MasterID dirCtrlId;
    class AbstractController *dirCtrlPtr;

    Tick timeInterval;
    Tick timeWarmup;

    uint64_t totalInterval;
    uint64_t skipInterval;


    struct ChannelIdx mainMem_id;
    struct ChannelIdx cacheMem_id;

    double avgMemLatencyPCM;
    double avgMemLatencyDRAM;
    double avgRdQLenPCM;
    double avgRdQLenDRAM;
    double avgWrQLenPCM;
    double avgWrQLenDRAM;
    double avgBWPCM;
    double avgBWDRAM;
    double avgWrBWPCM;
    double avgWrBWDRAM;

    Tick avgTimeSwitchRowPCM;
    Tick avgTimeSwitchRowDRAM;

    uint64_t pcmScore;
    uint64_t dramScore;

    const int DValueMax;
    const int infDramMax;
    const int infPcmMax;

    uint32_t refPagePerIntervalnum;
    uint32_t refPageinDramPerIntervalnum;
    uint32_t refPageinPcmPerIntervalnum;

    uint64_t reqInDramCount;
    uint64_t reqInPcmCount;
    uint64_t reqInDramCountPI;

		LRU statsStore;
    int MissThreshold;
		int numMigrate;
		int numReadDram;
		int numWriteDram;
		int64_t preBenefit;
		bool dirMissThreshold;


    // All statistics that the model needs to capture
    Stats::Scalar readReqs;
    Stats::Scalar writeReqs;
    Stats::Scalar intervalCount;
    Stats::Scalar balanceCount;
    Stats::Scalar unbalanceCount;
    Stats::Scalar rightRatioSum;

    Stats::Formula rightRatio;

    Tick MigrationTimeStartAt;
    Stats::Scalar totMemMigrationTime;
    Stats::Scalar totBlockedReqsForMigration;

    Tick bootUpTick;
    Stats::Scalar lastWarmupAt;
    Stats::Scalar badMigrationPageCount;
    Stats::Scalar migrationPageCount;
    Stats::Scalar migrationPageCount2DRAM;
    Stats::Scalar migrationPageCount2PCM;
    Stats::Scalar totBlockedreqMemAccLat;
    Stats::Scalar totBlockedreqMemAccLatWDelay;

    Tick prevArrival;
    Tick reqsPI;
    Tick totGapPI;
    Tick avgGapPI;
    Stats::Scalar totGap;
    Stats::Formula avgGap;
    Stats::Formula avgBlockedreqMemAccLat;
    Stats::Formula avgBlockedreqMemAccLatWDelay;

    Tick totBlockedreqMemAccLatPI;
    Tick totBlockedreqMemAccLatWDelayPI;
    size_t blockedreqThisInterval;
    std::deque<Tick> reqBlockedTickDiff;


};
#endif
