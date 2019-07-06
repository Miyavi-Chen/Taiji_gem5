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
#include <cmath>
#include "sim/full_system.hh"
#include "mem/hybrid_mem.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"
#include "debug/Drain.hh"
#include "mem/lru.cc"
#include "mem/ruby/system/RubySystem.hh"

#define MISSHITRATIOPCM 8.1
#define MISSHITRATIODRAM 3.0
#define RWLATENCYRATIOPCM 4//for miss
#define WQABILITYRATIO 3.36

using namespace std;
using namespace Data;

HybridMem::HybridMem(const HybridMemParams* p)
    : ClockedObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      physRanges(p->phys_ranges),
      memRanges(p->mem_ranges),
      channelRanges(p->channel_ranges),
    //   DramLFUDA(128, 0, 64), PcmLFUDA(128, 0, 64),
    //   rankingDramLRU(128), rankingPcmLRU(128),
      verbose(p->verbose),
      sys(p->sys),
      cacheLineSize(sys->cacheLineSize()),
      granularity(p->granularity),
      masterId(sys->getMasterId(this)),
      headerDelay(p->header_delay),
      width(p->width),
      maxMigrationTasks(p->max_migration_tasks),
      wait(WaitState::wNONE),
      sent(SentState::sIDLE),
      waitSlaveRetry(false),
      pools(channelRanges, granularity),
      pages(pools, memRanges, granularity, channelRanges.size()),
      migrationEvent([this]{ issueTimingMigrationPkt(); }, name()),
      releaseSentStateEvent([this]{ releaseSentState(); }, name()),
      burstSize(64),
      warmUpEvent([this]{processWarmUpEvent(); }, name()),
      regularBalancedEvent([this]{processRegularBalancedEvent(); }, name()),
      MLPUpdateEvent([this]{processMLPUpdateEvent(); }, name()),
      hasWarmedUp(false),
      timeInterval(p->time_interval), timeWarmup(p->time_warmup),
      totalInterval(0), skipInterval(0),
      avgMemLatencyPCM(0), avgMemLatencyDRAM(0),
      avgRdQLenPCM(0), avgRdQLenDRAM(0),
      avgWrQLenPCM(0), avgWrQLenDRAM(0),
      avgBWPCM(0), avgBWDRAM(0), avgWrBWPCM(0), avgWrBWDRAM(0),
      avgTimeSwitchRowPCM(0),
      avgTimeSwitchRowDRAM(0), pcmScore(0), dramScore(0),
      DValueMax(1024), infDramMax(DValueMax), infPcmMax(DValueMax),
      refPagePerIntervalnum(0), refPageinDramPerIntervalnum(0),
      refPageinPcmPerIntervalnum(0), reqInDramCount(0), reqInPcmCount(0),
      reqInDramCountPI(0),
      statsStore(2048), MissThreshold(1024), numMigrate(0),
      numReadDram(0), numWriteDram(0), preNumStalls(0), preBenefit(0),
      dirMissThreshold(false),
      MigrationTimeStartAt(0), bootUpTick(0),
      prevArrival(0), reqsPI(0), totGapPI(0),
      avgGapPI(0), totBlockedreqMemAccLatPI(0),
      totBlockedreqMemAccLatWDelayPI(0), blockedreqThisInterval(0)
{
    if (physRanges.size() != memRanges.size())
        fatal("HybridMem: original and shadowed range list must "
              "be same size\n");

    for (size_t x = 0; x < physRanges.size(); x++) {
        if (physRanges[x].size() != memRanges[x].size())
            fatal("HybridMem: original and shadowed range list elements"
                  " aren't all of the same size\n");
    }

    assert(totalSizeOf(memRanges));
    assert(totalSizeOf(channelRanges));
    assert(totalSizeOf(memRanges) <= totalSizeOf(channelRanges));

    dmaDevicePtr = nullptr;
    dmaDeviceId = 0;
}

void
HybridMem::init()
{
    if (!slavePort.isConnected() || !masterPort.isConnected()) {
        fatal("HybridMem is not connected on both sides.\n");
    }

    assert(isPowerOf2(granularity));
    assert(isPowerOf2(cacheLineSize));
    assert(granularity >= cacheLineSize);

    for (size_t i = 0; i < physRanges.size(); ++i) {
        assert((physRanges[i].start() % granularity) == 0);
        assert((physRanges[i].size() % granularity) == 0);
    }

    for (size_t i = 0; i < memRanges.size(); ++i) {
        assert((memRanges[i].start() % granularity) == 0);
        assert((memRanges[i].size() % granularity) == 0);
    }

    for (size_t i = 0; i < channelRanges.size(); ++i) {
        assert((channelRanges[i].start() % granularity) == 0);
        assert((channelRanges[i].size() % granularity) == 0);
        std::string str = "system.mem_ctrls";
        str.append(std::to_string(i));
        const char *cstr = str.c_str();
        ctrlptrs.push_back(reinterpret_cast<DRAMCtrl *>(SimObject::find(cstr)));
    }

    for (size_t i = 0; i < memRanges.size(); ++i) {
        for (Addr addr = memRanges[i].start(); addr < memRanges[i].end();
                addr += granularity) {
            class Page *page = pages.pageOf(toPageAddr(addr));
            struct ChannelIdx channel_idx;
            struct FrameAddr frame_addr;
            assert(pools.tryGetAnyFreeFrame(&channel_idx, &frame_addr));
            page->allocFrame(channel_idx, frame_addr);
            page->claimChannelIsValid(channel_idx);
            if(!pools.poolOf(channel_idx)->isPoolUsed())
                pools.poolOf(channel_idx)->setPoolUsed();
        }
    }
    for (size_t ch = 0; ch < channelRanges.size(); ++ch) {
        struct ChannelIdx channel_idx = toChannelIdx(ch);
        class FramePool *pool = pools.poolOf(channel_idx);

        if (pool->isPoolUsed()) { mainMem_id.val = ch; }
        else {
            cacheMem_id.val = ch;
            if (ctrlptrs[cacheMem_id.val]->enableBinAware) {
                pool->setDRAM();
                pool->setRankBinsPtr(
                    new HybridMem::BinsInRanks(ctrlptrs[cacheMem_id.val]));
            }

            break;
        }
    }
    DRAMmemCycle2Tick = ctrlptrs[cacheMem_id.val]->tCK;

    *(const_cast<uint32_t *>(&burstSize)) = ctrlptrs[cacheMem_id.val]->burstSize;
    ctrlptrs[cacheMem_id.val]->HybridMemID = masterId;
    ctrlptrs[mainMem_id.val]->HybridMemID = masterId;
    ctrlptrs[cacheMem_id.val]->timeInterval = double(timeInterval)/1000000000000;
    ctrlptrs[mainMem_id.val]->timeInterval = double(timeInterval)/1000000000000;

    dmaDevicePtr = SimObject::find ("system.pci_ide");
    dmaDeviceId = sys->lookupMasterId(dmaDevicePtr);
    ctrlptrs[cacheMem_id.val]->DMAID = dmaDeviceId;
    ctrlptrs[mainMem_id.val]->DMAID = dmaDeviceId;

    dirCtrlPtr = reinterpret_cast<AbstractController *>(SimObject::find ("system.ruby.dir_cntrl0"));
    // dirCtrlId = sys->lookupMasterId(dirCtrlPtr);
    cacheCtrlsIDptr = &(dirCtrlPtr->params()->ruby_system->cacheCtrlsID);

    for (int i = 0; i < sys->numContexts(); ++i) {
        cpuRdReqs.push_back(0);
        cpuWrReqs.push_back(0);
    }

}

void
HybridMem::startup()
{
    for (int i = 0; i < cacheCtrlsIDptr->size(); ++i) {
        cacheIDmap[cacheCtrlsIDptr->at(i)] = i;
    }


    bootUpTick = curTick();
    std::cout<<"boot up at " <<bootUpTick<<"\n";

    if (!FullSystem) {
        schedule(warmUpEvent, curTick() + timeWarmup);
        std::cout<<"Schedule warmUpEvent when start up \n";
    }

    if (!MLPUpdateEvent.scheduled()) {
        schedule(MLPUpdateEvent, curTick() + 6*DRAMmemCycle2Tick);
    } else {
        reschedule(MLPUpdateEvent, curTick() + 6*DRAMmemCycle2Tick);
    }

    if (timeInterval == 0) {return;}
    schedule(regularBalancedEvent, curTick() + timeInterval);
}

void
HybridMem::processWarmUpEvent()
{

    std::cout<<"HybridMem Warm up at "<<curTick()<<std::endl;

    for (int i = 0; i < channelRanges.size(); ++i) {
        ctrlptrs[i]->resetAllStats();
    }

    Stats::schedStatEvent(false, true, curTick(), 0);

}

void
HybridMem::statisticInfoCheck()
{
    // std::cout<< "LFUDA_PCM :";
    // PcmLFUDA.printLFU();
    // std::cout<< "LFUDA_DRAM :";
    // DramLFUDA.printLFU();
    double count = 0.0;
    for (auto &iter : migrationPagesPI) {
        // std::cout<<iter.second<<", ";
        if (iter.second > 5) count+=1;
    }
    rightRatioSum += std::isnan(count /migrationPagesPI.size())?
                        0.0 : count /migrationPagesPI.size();
    // std::cout<<"\n"<<"Right ratio: "<<count /migrationPagesPI.size()<<"\n";
    // migrationPagesPI.clear();

    if (!reqBlockedTickDiff.empty()) {
        totBlockedReqsForMigration -= reqBlockedTickDiff.size();
        blockedreqThisInterval -= reqBlockedTickDiff.size();
        reqBlockedTickDiff.clear();
    }
    avgGapPI = reqsPI == 0 ? 0 : totGapPI / reqsPI;
    if (blockedreqThisInterval) {
        std::cout<<"blockedreqThisInterval "<<blockedreqThisInterval<<"\n";
        std::cout<<"Lat_o: "<<totBlockedreqMemAccLatPI
            / blockedreqThisInterval / 1000<<" ";
        std::cout<<"Lat_w_d: "<<totBlockedreqMemAccLatWDelayPI
            / blockedreqThisInterval / 1000<<"\n";
    }

    updateStatisticInfo();
    std::cout<<"D-RQL: "<<avgRdQLenDRAM<<" D-WQL: "<<avgWrQLenDRAM<<"\n";
    std::cout<<"P-RQL: "<<avgRdQLenPCM <<" P-WQL: "<<avgWrQLenPCM <<"\n";
    std::cout<<"PCM "<<avgMemLatencyPCM/1000<<"x"<< ctrlptrs[mainMem_id.val]->readBurstsPI.value()<<" DRAM "<<avgMemLatencyDRAM/1000<<"x"<< ctrlptrs[cacheMem_id.val]->readBurstsPI.value()<<"\n";
    std::cout<<"PCM "<<avgBWPCM<<"/"<<avgWrBWPCM<<" DRAM "<<avgBWDRAM<<"/"<<avgWrBWDRAM<<"\n";

    ++totalInterval;
    ++intervalCount;
}

void
HybridMem::processRegularBalancedEvent()
{
    assert(!regularBalancedEvent.scheduled());
    // timeInterval = 1000000000;//1ms
    schedule(regularBalancedEvent, curTick() + timeInterval);
    *(const_cast<unsigned *>(&maxMigrationTasks)) = 32;

    statisticInfoCheck();

    ++skipInterval;
    if (skipInterval < 1) {
        resetPerInterval();
        return;
    }
    int64_t benefit =
        ((int64_t)numReadDram * 22 + (int64_t)numWriteDram * 172)
            - (int64_t)numMigrate * 242;//0.833*4*64+28
    std::cout<<"benefit "<< benefit<<" ";

    Counter totNumStalls = sys->getFirstCPUptr()->numSimulatedStalls();
    sys->getFirstCPUptr()->resetSimulatedStalls();
    std::cout<<"totNumStalls "<< totNumStalls<<" ";
    std::cout<<"numMigrate "<< numMigrate<<" ";


	if(totNumStalls > preNumStalls)
	{
		if(dirMissThreshold)
			incMissThres();
		else
			decMissThres();
	}
	else
	{
		if(dirMissThreshold)
			decMissThres();
		else
			incMissThres();
	}
	preNumStalls = totNumStalls;
    preBenefit = benefit;

    resetPerInterval();
    std::cout<<"MissThreshold "<< MissThreshold<<"\n";


    return;
}

void
HybridMem::getPageRanking(std::vector<Addr>& v)
{
    SortHostPage tmp;
    int write_ratio = 4;
    Ranking.clear();
    for (size_t i = 0 ; i < v.size() ; i++) {
        Addr PN = v[i];
        struct PageAddr pageAddr;
        pageAddr.val = PN;
        class Page *page = pages.pageOf(pageAddr);
        tmp.host_PN = PN;
        if (page->isInDram) {
            struct FrameAddr framAddr = page->getFrameAddr(cacheMem_id);
            tmp.dram_PN = framAddr.val;
        } else {
            tmp.dram_PN = std::numeric_limits<Addr>::max();
        }

        int sum = page->readcount + page->writecount * write_ratio;
        if (sum > DValueMax) {
            sum = DValueMax;
        }
        tmp.value = sum;
        tmp.writecount = page->writecount;
        std::vector<struct ChannelIdx> idxSet;
        page->getValidChannelIdx(&idxSet);
        tmp.isDirty = idxSet.size() == 1? true : false;
        Ranking.push_back(tmp);
    }
}

bool
HybridMem::PCMtoDRAMsort(const SortHostPage& a, const SortHostPage& b)
{
    Addr addrMax = std::numeric_limits<Addr>::max();
    if (a.dram_PN != addrMax && b.dram_PN == addrMax) {
        return true;
    } else if (a.dram_PN == addrMax && b.dram_PN != addrMax) {
        return false;
    } else if (a.value != b.value) {//value mean D-value
        return a.value > b.value;
    } else {
        return a.writecount > b.writecount;
    }

}

bool
HybridMem::DRAMtoPCMsort(const SortHostPage& a, const SortHostPage& b)
{
    if (a.isDirty != b.isDirty) {
        return a.isDirty > b.isDirty;
    } else if (a.value != b.value) {
        return a.value > b.value;
    } else {
        return a.writecount > b.writecount;
    }
}

void
HybridMem::genMigrationTasks(class Page * page, bool pcm2dram)
{
    assert(page->isValid(mainMem_id));
    if (pcm2dram) {
        if (pools.poolOf(cacheMem_id)->getFreeFrameSize() <=
                maxMigrationTasks) {
            size_t evictedPageNum = maxMigrationTasks/2;
            genDramEvictedMigrationTasks(evictedPageNum);
            std::cout<<"genDramEvictedMigrationTasks "<<evictedPageNum<<"\n";
        }

        genPcmMigrationTasks(page);
    } else {
        genDramMigrationTasks(page);
    }
}

void
HybridMem::resetPerInterval()
{
    for (auto ctrlptr : ctrlptrs) {
        ctrlptr->resetPerInterval();
    }

    // for (int i = 0; i < cacheCtrlsIDptr->size(); ++i) {
    //     cpuRdReqs[i] = 0;
    //     cpuWrReqs[i] = 0;
    // }

    for (auto pageNum : migrationPagesPI) {
        PageAddr pageAddr = {pageNum.first};
        class Page *page = pages.pageOf(pageAddr);
        if (page->lastAccessInterval != totalInterval) {
            page->resetPageCounters();
            page->lastAccessInterval = totalInterval;
        }
        page->justMigrate = true;
    }
    numMigrate = migrationPagesPI.size();
    migrationPagesPI.clear();
    statsStore.reset();
    mapRef.clear();

    // DramLFUDA.reset();
    // PcmLFUDA.reset();
    // rankingDramLRU.reset();
    // rankingPcmLRU.reset();

    numReadDram = 0;
    numWriteDram = 0;

    avgMemLatencyPCM = 0.0;
    avgMemLatencyDRAM = 0.0;
    // avgRdQLenPCM = 0.0;
    // avgRdQLenDRAM = 0.0;

    avgTimeSwitchRowPCM = 0;
    avgTimeSwitchRowDRAM = 0;

    pcmScore = 0;
    dramScore = 0;

    refPagePerIntervalnum = 0;
    refPageinDramPerIntervalnum = 0;
    refPageinPcmPerIntervalnum = 0;

    reqInDramCount = 0;
    reqInPcmCount = 0;

    totBlockedreqMemAccLatPI = 0;
    totBlockedreqMemAccLatWDelayPI = 0;
    blockedreqThisInterval = 0;

    totGapPI = 0;
    reqsPI = 0;
}

void
HybridMem::resetPages()
{
    pages.resetPageCounters();

}

void
HybridMem::genDramEvictedMigrationTasks(size_t &halfMigrationPageNum)
{
    size_t migrationCount = 0;
    class FramePool *pool = pools.poolOf(cacheMem_id);
    for (int i = 0; i < halfMigrationPageNum; ++i) {
        struct FrameAddr frameAddr = {std::numeric_limits<Addr>::max()};
        struct PageAddr pageAddr = {std::numeric_limits<Addr>::max()};
        frameAddr = pool->pickOneEvictedFrame();
        pageAddr = {pool->getOwner(frameAddr).val};
        class Page *page = pages.pageOf(pageAddr);

        if (page->isInvalid(mainMem_id) || page->isUnused(mainMem_id)) {
            assert(tryIssueMigrationTask(page, cacheMem_id, mainMem_id));
            assert(migrationPages.find(pageAddr.val) != migrationPages.end());
            migrationPages.erase(pageAddr.val);
        } else {
            page->freeFrame(cacheMem_id);
        }

        if (migrationPagesPI.find(pageAddr.val)!=migrationPagesPI.end())
            migrationPagesPI.erase(pageAddr.val);

        page->isInDram = false;
        page->claimChannelIsValid(mainMem_id);
        ++migrationCount;
    }
    halfMigrationPageNum = migrationCount;
    // std::cout<< "Page cache count: "<< pageCacheCount<<"\n";
}

void
HybridMem::genPcmMigrationTasks(class Page * page)
{
    class FramePool *pool = pools.poolOf(cacheMem_id);
    assert(page->isValid(mainMem_id));
    migrationPages[page->getPageAddr().val] = 0;
    migrationPagesPI[page->getPageAddr().val] = 0;

    if (migrationTasks.size() == 0) {
        MigrationTimeStartAt = curTick();
    }

    if (page->isInvalid(cacheMem_id) || page->isUnused(cacheMem_id)) {
        struct FrameAddr frameAddr;
        if(!pool->tryGetAnyFreeFrame(&frameAddr)) {
            std::cout<<"DRAM EMPTY!!\n";
            exit(-1);
        }
        if (page->isInvalid(cacheMem_id))
            page->freeFrame(cacheMem_id);
        page->allocFrame(cacheMem_id, frameAddr);
        assert(tryIssueMigrationTask(page, mainMem_id, cacheMem_id));

    } else {
        //nothing to do
    }
    page->isInDram = true;
    page->claimChannelIsValid(cacheMem_id);
}

void
HybridMem::genDramMigrationTasks(class Page * page)
{
    PageAddr pageAddr = page->getPageAddr();
    assert(page->isValid(cacheMem_id));
    assert(migrationPages.find(pageAddr.val)!=migrationPages.end());
    migrationPages.erase(pageAddr.val);

    if (migrationPagesPI.find(pageAddr.val) != migrationPagesPI.end())
        migrationPagesPI.erase(pageAddr.val);

    if (page->isInvalid(mainMem_id) || page->isUnused(mainMem_id)) {
        assert(tryIssueMigrationTask(page, cacheMem_id, mainMem_id));
    } else {
        //nothing to do
    }

    page->isInDram = false;
    page->claimChannelIsValid(mainMem_id);

}

void
HybridMem::handlePFDMA(class Page * page, PacketPtr pkt)
{
    assert(pkt->masterId() == dmaDeviceId);
    if (pkt->isWrite() && !page->isDirty
        && page->readcount == 0 && page->writecount == 0) {
        updateBWInfo();
        page->isPageCache = true;

        if (WQABILITYRATIO * ctrlptrs[mainMem_id.val]->totalWriteQueueSize >=
                ctrlptrs[cacheMem_id.val]->totalWriteQueueSize) {
            //3.36* avgWrBWPCM > avgWrBWDRAM
            assert(page->isUnused(cacheMem_id));
            struct FrameAddr frameAddr;
            if (pools.poolOf(cacheMem_id)->tryGetAnyFreeFrame(&frameAddr)) {
                if (page->isInvalid(cacheMem_id)) {
                    page->freeFrame(cacheMem_id);
                }
                page->allocFrame(cacheMem_id, frameAddr);
                assert(tryIssueMigrationTask(page, mainMem_id, cacheMem_id));
                page->isInDram = true;
            } else {
                //nothing to do
            }

        }
        page->writecount = page->readcount = -64;

        if (!warmUpEvent.scheduled()) {
            schedule(warmUpEvent, curTick() + timeWarmup);
        } else {
            reschedule(warmUpEvent, curTick() + timeWarmup);
        }
    }
}


void
HybridMem::reqBlockedTickDiffUpdate()
{
    blockedreqThisInterval = avgGapPI == 0 ? 0:
                (curTick() - MigrationTimeStartAt) / avgGapPI;
    totBlockedReqsForMigration += blockedreqThisInterval;
    for (int i = 0; i < blockedreqThisInterval; ++i) {
        reqBlockedTickDiff.push_back(curTick() -
                            (MigrationTimeStartAt + (i+1)*avgGapPI));
    }
    // while (!reqBlockedAt.empty()) {
    //     reqBlockedTickDiff.push_back(curTick() - reqBlockedAt.front());
    //     reqBlockedAt.pop_front();
    // }
    // assert(reqBlockedAt.empty());

}

Port &
HybridMem::getPort(const std::string &if_name, PortID idx)
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
HybridMem::recvFunctional(PacketPtr pkt)
{
    const Addr memAddr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    if (page->inMigrating()) { flushAllMigrationPkt(); }
    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    masterPort.sendFunctional(pkt);
    pkt->setAddr(memAddr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }
}

void
HybridMem::recvFunctionalSnoop(PacketPtr pkt)
{
    const Addr memAddr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    slavePort.sendFunctionalSnoop(pkt);
    pkt->setAddr(memAddr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }
}

Tick
HybridMem::recvAtomic(PacketPtr pkt)
{
    const Addr memAddr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    if (page->inMigrating()) { flushAllMigrationPkt(); }
    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    Tick ret_tick = masterPort.sendAtomic(pkt);
    pkt->setAddr(memAddr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }

    return ret_tick;
}

Tick
HybridMem::recvAtomicSnoop(PacketPtr pkt)
{
    const Addr memAddr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    Tick ret_tick = slavePort.sendAtomicSnoop(pkt);
    pkt->setAddr(memAddr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }

    return ret_tick;
}

bool
HybridMem::tryIssueMigrationTask(class Page *page,
    struct ChannelIdx _from, struct ChannelIdx _to)
{
    if (migrationTasks.size() >= maxMigrationTasks) {
        return false;
    }

    class MigrationTask *task = new class MigrationTask(
        masterId, page, toPhysAddr(page),
        _from, _to, granularity, cacheLineSize);
    migrationTasks.push_back(task);
    ++(page->migrationCount);
    page->lastMigrationInterval = totalInterval;
    ++migrationPageCount;
    if (page->migrationCount > 2) {
        ++badMigrationPageCount;
    }

    if (sys->isTimingMode() && !page->isPageCache) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            if (!migrationEvent.scheduled()) {
                schedule(migrationEvent, curTick());
            }
        } else if (wait == WaitState::wMASTER) {
            if (!migrationEvent.scheduled()) {
                schedule(migrationEvent, curTick());
            }
        } else if (wait == WaitState::wSELF) {
            assert(!migrationEvent.scheduled());
        } else if (wait == WaitState::wMASTER_wSELF) {
            assert(!migrationEvent.scheduled());
        } else if (wait == WaitState::wSELF_wMASTER) {
            assert(!migrationEvent.scheduled());
        } else {
            assert(0);
        }
    } else {
        flushAllMigrationPkt();
    }

    return true;
}

void
HybridMem::flushAllMigrationPkt()
{
    for (auto it = migrationTasks.begin(); it != migrationTasks.end();) {
        class MigrationTask *task = *it;
        PacketPtr pkt;
        if (task->tryGetAnyPkt(&pkt)) {
            pkt->headerDelay =
                (clockEdge() - curTick()) + (headerDelay * clockPeriod());
            pkt->payloadDelay = (pkt->hasData()) ?
                (divCeil(pkt->getSize(), width) * clockPeriod()) : (0);
            recvFunctionalMigrationReq(task, pkt);
            recvFunctionalMigrationResp(pkt);
            it = migrationTasks.begin();
        } else {
            ++it;
        }
    }

    if (drainState() == DrainState::Draining && canBeDrained()) {
        DPRINTF(Drain, "HybridMem done draining, signaling drain manager\n");
        signalDrainDone();
    }
}

void
HybridMem::issueTimingMigrationPkt()
{
    assert(!migrationEvent.scheduled());

    for (auto it = migrationTasks.begin(); it != migrationTasks.end();) {
        class MigrationTask *task = *it;
        PacketPtr pkt;
        if (task->tryGetAnyPkt(&pkt)) {
            pkt->headerDelay =
                (clockEdge() - curTick()) + (headerDelay * clockPeriod());
            pkt->payloadDelay = (pkt->hasData()) ?
                (divCeil(pkt->getSize(), width) * clockPeriod()) : (0);
            Tick nextPktTick = pkt->headerDelay + pkt->payloadDelay;
            const bool successful = recvTimingMigrationReq(task, pkt);
            if (successful) {
                schedule(migrationEvent, curTick() + nextPktTick);
            }
            break;
        } else {
            ++it;
        }
    }

    if (drainState() == DrainState::Draining && canBeDrained()) {
        DPRINTF(Drain, "HybridMem done draining, signaling drain manager\n");
        signalDrainDone();
    }
}

bool
HybridMem::recvTimingReq(PacketPtr pkt)
{
    const Addr memAddr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    // Calc avg gap between requests
    if (prevArrival != 0) {
        totGap += curTick() - prevArrival;
        totGapPI += curTick() - prevArrival;
    }
    prevArrival = curTick();

    if (page->inMigrating()) {
        page->bookingMasterWaiting();
        // totBlockedReqsForMigration += 1;
        return false;
    }

    if (false) {
        assert(0);
    } else if (wait == WaitState::wNONE) {
        // pass
    } else if (wait == WaitState::tryMASTER) {
        // pass
    } else if (wait == WaitState::tryMASTER_wSELF) {
        // pass
        // wait = WaitState::wSELF_wMASTER;
        // return false;
    } else if (wait == WaitState::wSELF) {
        wait = WaitState::wSELF_wMASTER;
        return false;
    } else {
        assert(0);
    }

    if (waitSlaveRetry || (sent == SentState::sSELF)) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wMASTER;
            return false;
        } else if (wait == WaitState::tryMASTER) {
            wait = WaitState::wMASTER;
            return false;
        } else if (wait == WaitState::tryMASTER_wSELF) {
            // wait = WaitState::wMASTER_wSELF;
            wait = WaitState::wSELF_wMASTER;
            // totBlockedReqsForMigration += 1;
            // reqBlockedAt.push_back(curTick());
            return false;
        } else {
            assert(0);
        }
    }


    Tick releaseSentStateTick = clockEdge(Cycles(1));
    releaseSentStateTick += (pkt->hasData()) ?
        (divCeil(pkt->getSize(), width) * clockPeriod()) : (0);

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);
    const bool needsResponse = pkt->needsResponse();
    const bool cacheResponding = pkt->cacheResponding();

    if (needsResponse && !cacheResponding) {
        pkt->pushSenderState(new HybridMemSenderState(memAddr));
    }

    if (!reqBlockedTickDiff.empty() && !pkt->needAddDelay) {
        pkt->needAddDelay = true;
        pkt->delay = reqBlockedTickDiff.front();
        if (!pkt->delay) {pkt->needAddDelay = false;}
        reqBlockedTickDiff.pop_front();
    }
    pkt->setAddr(channel_addr);
    const bool successful = masterPort.sendTimingReq(pkt);
    if (!successful) {
        pkt->setAddr(memAddr);
        if (needsResponse) {
            delete pkt->popSenderState();
        }
    } else {
        auto iter = migrationPages.find(page->getPageAddr().val);
        if (iter != migrationPages.end()) {
            ++reqInDramCount;
            iter->second += 1;
        } else {
            ++reqInPcmCount;
        }

        auto iterPI = migrationPagesPI.find(page->getPageAddr().val);
        if (iterPI != migrationPagesPI.end()) {
            ++reqInDramCountPI;
            iterPI->second += 1;
        }

        memNumInc(pkt->masterId(), isRead);

        // predicRowHitOrMiss(page);
        assert(!(isRead && isWrite));
        if (isRead) {
            readReqs++;
            reqsPI++;
            page->launchReadTo(channel_idx);
            page->readreqPerInterval++;
            // reqsPageMapRd[burstAlign(memAddr)] = page;
        }
        if (isWrite) {
            writeReqs++;
            reqsPI++;
            page->launchWriteTo(channel_idx);
            page->writereqPerInterval++;
            page->isDirty = true;
            // reqsPageMapWr[burstAlign(memAddr)] = page;
        }

    }

    if (!successful) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wMASTER;
        } else if (wait == WaitState::tryMASTER) {
            wait = WaitState::wMASTER;
        } else if (wait == WaitState::tryMASTER_wSELF) {
            // wait = WaitState::wMASTER_wSELF;
            wait = WaitState::wSELF_wMASTER;
        } else {
            assert(0);
        }
        waitSlaveRetry = true;
    } else {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wNONE;
        } else if (wait == WaitState::tryMASTER) {
            wait = WaitState::wNONE;
        } else if (wait == WaitState::tryMASTER_wSELF) {
            wait = WaitState::wSELF;
        } else {
            assert(0);
        }
        sent = SentState::sMASTER;
        reschedule(releaseSentStateEvent, releaseSentStateTick, true);
    }

    if (!regularBalancedEvent.scheduled()) {
        processRegularBalancedEvent();
    }

    return successful;
}

bool
HybridMem::recvTimingMigrationReq(class MigrationTask *task, PacketPtr pkt)
{
    if (false) {
        assert(0);
    } else if (wait == WaitState::wNONE) {
        // pass
    } else if (wait == WaitState::trySELF) {
        // pass
    } else if (wait == WaitState::trySELF_wMASTER) {
        // pass
    } else if (wait == WaitState::wMASTER) {
        // wait = WaitState::wMASTER_wSELF;
        wait = WaitState::wSELF_wMASTER;
        return false;
    } else {
        assert(0);
    }

    if (waitSlaveRetry || (sent == SentState::sMASTER)) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wSELF;
            return false;
        } else if (wait == WaitState::trySELF) {
            wait = WaitState::wSELF;
            return false;
        } else if (wait == WaitState::trySELF_wMASTER) {
            wait = WaitState::wSELF_wMASTER;
            return false;
        } else {
            assert(0);
        }
    }

    Tick releaseSentStateTick = clockEdge(Cycles(1));
    releaseSentStateTick += (pkt->hasData()) ?
        (divCeil(pkt->getSize(), width) * clockPeriod()) : (0);

    const bool successful = masterPort.sendTimingReq(pkt);
    if (successful) {
        task->issuePkt(pkt);
        assert(migrationReq.find(pkt->req) == migrationReq.end());
        migrationReq[pkt->req] = task;
    }

    if (!successful) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wSELF;
        } else if (wait == WaitState::trySELF) {
            wait = WaitState::wSELF;
        } else if (wait == WaitState::trySELF_wMASTER) {
            wait = WaitState::wSELF_wMASTER;
        } else {
            assert(0);
        }
        waitSlaveRetry = true;
    } else {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wNONE;
        } else if (wait == WaitState::trySELF) {
            wait = WaitState::wNONE;
        } else if (wait == WaitState::trySELF_wMASTER) {
            wait = WaitState::wMASTER;
        } else {
            assert(0);
        }
        sent = SentState::sSELF;
        reschedule(releaseSentStateEvent, releaseSentStateTick, true);
    }

    return successful;
}

void
HybridMem::recvFunctionalMigrationReq(class MigrationTask *task, PacketPtr pkt)
{
    masterPort.sendFunctional(pkt);
    task->issuePkt(pkt);
    assert(migrationReq.find(pkt->req) == migrationReq.end());
    migrationReq[pkt->req] = task;
}

bool
HybridMem::recvTimingResp(PacketPtr pkt)
{
    if (migrationReq.find(pkt->req) != migrationReq.end()) {
        return recvTimingMigrationResp(pkt);
    }

    HybridMemSenderState* receivedState =
        dynamic_cast<HybridMemSenderState*>(pkt->senderState);
    assert(receivedState != NULL);
    const Addr channel_addr = pkt->getAddr();

    pkt->senderState = receivedState->predecessor;
    pkt->setAddr(receivedState->memAddr);

    const bool successful = slavePort.sendTimingResp(pkt);

    if (!successful) {
        pkt->senderState = receivedState;
        pkt->setAddr(channel_addr);
    } else {
        delete receivedState;
    }

    return successful;
}

void
HybridMem::recvFunctionalMigrationResp(PacketPtr pkt)
{
    const auto resp = migrationReq.find(pkt->req);
    assert(resp != migrationReq.end());
    class MigrationTask *task = resp->second;
    migrationReq.erase(resp);

    task->finishPkt(pkt);

    if (task->isDone()) {
        class Page *page = task->getPage();
        if (page->masterIsWaiting()) {
            if (false) {
                assert(0);
            } else if (wait == WaitState::wNONE) {
                wait = WaitState::wMASTER;
            } else if (wait == WaitState::wSELF) {
                wait = WaitState::wSELF_wMASTER;
            } else {
                assert(0);
            }
            page->cancelMasterWaiting();
        }

        std::deque<class MigrationTask *>::iterator it;
        for (it = migrationTasks.begin(); it != migrationTasks.end(); ++it) {
            if (*it == task) { break; }
        }
        assert(it != migrationTasks.end());
        migrationTasks.erase(it);
        delete task;

        // trySendRetry();
    }
}

bool
HybridMem::recvTimingMigrationResp(PacketPtr pkt)
{
    const auto resp = migrationReq.find(pkt->req);
    assert(resp != migrationReq.end());
    class MigrationTask *task = resp->second;
    migrationReq.erase(resp);

    task->finishPkt(pkt);

    if (task->isDone()) {
        class Page *page = task->getPage();
        if (page->masterIsWaiting()) {
            if (false) {
                assert(0);
            } else if (wait == WaitState::wNONE) {
                wait = WaitState::wMASTER;
            } else if (wait == WaitState::wSELF) {
                wait = WaitState::wSELF_wMASTER;
            } else {
                assert(0);
            }
            page->cancelMasterWaiting();
        }

        std::deque<class MigrationTask *>::iterator it;
        for (it = migrationTasks.begin(); it != migrationTasks.end(); ++it) {
            if (*it == task) { break; }
        }
        assert(it != migrationTasks.end());
        migrationTasks.erase(it);

        if (task->targetChannel().val == mainMem_id.val &&
            pools.poolOf(cacheMem_id)->getFreeFrameSize() <=maxMigrationTasks) {
            page->freeFrame(cacheMem_id);
        }

        delete task;

        if (migrationTasks.end()-migrationTasks.begin() == 0) {
            totMemMigrationTime += curTick() - MigrationTimeStartAt;
            reqBlockedTickDiffUpdate();

        }


        trySendRetry();
    }

    if (false) {
        assert(0);
    } else if (wait == WaitState::wNONE) {
        if (!migrationEvent.scheduled()) {
            schedule(migrationEvent, curTick());
        }
    } else if (wait == WaitState::wMASTER) {
        if (!migrationEvent.scheduled()) {
            schedule(migrationEvent, curTick());
        }
    } else if (wait == WaitState::wSELF) {
        assert(!migrationEvent.scheduled());
    } else if (wait == WaitState::wMASTER_wSELF) {
        assert(!migrationEvent.scheduled());
    } else if (wait == WaitState::wSELF_wMASTER) {
        assert(!migrationEvent.scheduled());
    } else {
        assert(0);
    }

    return true;
}

void
HybridMem::trySendRetry()
{
    while (!waitSlaveRetry) {
        if (false) {
            assert(0);
        } else if (sent == SentState::sIDLE) {
            if (false) {
                assert(0);
            } else if (wait == WaitState::wSELF_wMASTER) {
                wait = WaitState::trySELF_wMASTER;
                // totBlockedReqsForMigration += 1;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF_wMASTER) {
                    wait = WaitState::wMASTER;
                }
            } else if (wait == WaitState::wMASTER) {
                wait = WaitState::tryMASTER;
                slavePort.sendRetryReq();
                if (wait == WaitState::tryMASTER) {
                    wait = WaitState::wNONE;
                }
            } else if (wait == WaitState::wMASTER_wSELF) {
                // wait = WaitState::tryMASTER_wSELF;
                // slavePort.sendRetryReq();
                // totBlockedReqsForMigration += 1;
                wait = WaitState::trySELF_wMASTER;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF_wMASTER) {
                    wait = WaitState::wMASTER;
                }
            } else if (wait == WaitState::wSELF) {
                wait = WaitState::trySELF;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF) {
                    wait = WaitState::wNONE;
                }
            } else if (wait == WaitState::wNONE) {
                break;
            } else {
                assert(0);
            }
        } else if (sent == SentState::sMASTER) {
            if (false) {
                assert(0);
            } else if (wait == WaitState::wMASTER) {
                wait = WaitState::tryMASTER;
                slavePort.sendRetryReq();
                if (wait == WaitState::tryMASTER) {
                    wait = WaitState::wNONE;
                }
            } else if (wait == WaitState::wMASTER_wSELF) {
                // wait = WaitState::tryMASTER_wSELF;
                // slavePort.sendRetryReq();
                // totBlockedReqsForMigration += 1;
                wait = WaitState::trySELF_wMASTER;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF_wMASTER) {
                    wait = WaitState::wMASTER;
                }
            } else if (wait == WaitState::wSELF) {
                break;
            } else if (wait == WaitState::wSELF_wMASTER) {
                break;
            } else if (wait == WaitState::wNONE) {
                break;
            } else {
                assert(0);
            }
        } else if (sent == SentState::sSELF) {
            if (false) {
                assert(0);
            } else if (wait == WaitState::wSELF_wMASTER) {
                // totBlockedReqsForMigration += 1;
                wait = WaitState::trySELF_wMASTER;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF_wMASTER) {
                    wait = WaitState::wMASTER;
                }
            } else if (wait == WaitState::wSELF) {
                wait = WaitState::trySELF;
                issueTimingMigrationPkt();
                if (wait == WaitState::trySELF) {
                    wait = WaitState::wNONE;
                }
            } else if (wait == WaitState::wMASTER) {
                break;
            } else if (wait == WaitState::wMASTER_wSELF) {
                break;
            } else if (wait == WaitState::wNONE) {
                break;
            } else {
                assert(0);
            }
        } else {
            assert(0);
        }
    }

    if (drainState() == DrainState::Draining && canBeDrained()) {
        DPRINTF(Drain, "HybridMem done draining, signaling drain manager\n");
        signalDrainDone();
    }
}

void
HybridMem::releaseSentState()
{
    assert(!releaseSentStateEvent.scheduled());
    assert(sent != SentState::sIDLE);
    sent = SentState::sIDLE;
    trySendRetry();
}

void
HybridMem::recvReqRetry()
{
    assert(waitSlaveRetry);
    waitSlaveRetry = false;
    trySendRetry();
}

void
HybridMem::recvTimingSnoopReq(PacketPtr pkt)
{
    slavePort.sendTimingSnoopReq(pkt);
}

bool
HybridMem::recvTimingSnoopResp(PacketPtr pkt)
{
    return masterPort.sendTimingSnoopResp(pkt);
}

bool
HybridMem::isSnooping() const
{
    if (slavePort.isSnooping()) {
        fatal("HybridMem doesn't support remapping of snooping requests\n");
    }
    return false;
}

void
HybridMem::recvRespRetry()
{
    masterPort.sendRetryResp();
}

void
HybridMem::recvRangeChange()
{
    slavePort.sendRangeChange();
}

AddrRangeList
HybridMem::getAddrRanges() const
{
    // Simply return the original ranges as given by the parameters
    AddrRangeList ranges(memRanges.begin(), memRanges.end());
    return ranges;
}

Addr
HybridMem::toChannelAddr(class Page *page, struct ChannelIdx idx, PacketPtr pkt)
{
    return page->getFrameAddr(idx).val + (pkt->getAddr() % granularity);
}

ChannelIdx
HybridMem::selectChannelIdx(class Page *page)
{
    std::vector<struct ChannelIdx> channelIdxSet;
    page->getCanLaunchChannelIdx(&channelIdxSet);
    assert(channelIdxSet.size());
    size_t select = 0;
    if (channelIdxSet.size() > 1) {
        if (page->isInDram)
            select = cacheMem_id.val;
        else
            select = mainMem_id.val;
    }
    return channelIdxSet[select];
}

ChannelIdx
HybridMem::selectChannelIdxFun(class Page *page)
{
    std::vector<struct ChannelIdx> channelIdxSet;
    page->getCanLaunchChannelIdxFun(&channelIdxSet);
    assert(channelIdxSet.size());
    size_t select = 0;
    if (channelIdxSet.size() > 1) {
        if (page->isInDram)
            select = cacheMem_id.val;
        else
            select = mainMem_id.val;
    }
    return channelIdxSet[select];
}

ChannelIdx
HybridMem::toChannelIdx(size_t i) const
{
    struct ChannelIdx ret;
    ret.val = i;
    return ret;
}

PhysAddr
HybridMem::toPhysAddr(struct PageAddr addr) const
{
    struct PhysAddr ret;
    ret.val = std::numeric_limits<Addr>::max();
    for (size_t i = 0; i < memRanges.size(); ++i) {
        if (memRanges[i].contains(addr.val)) {
            Addr offset = addr.val - memRanges[i].start();
            ret.val = offset + physRanges[i].start();
            return ret;
        }
    }

    panic("HybridMem: address '0x%x' out of range!\n", addr.val);
    return ret;
}

PageAddr
HybridMem::toMemAddr(Addr addr) const
{
    struct PageAddr pageaddr;
    for (int i = 0; i < physRanges.size(); ++i) {
        if (physRanges[i].contains(addr)) {
            Addr offset = addr - physRanges[i].start();
            pageaddr.val = offset + memRanges[i].start();
            return pageaddr;
        }
    }

    panic("HybridMem: address '0x%x' out of range!\n", addr);
    return pageaddr;
}

PhysAddr
HybridMem::toPhysAddr(class Page *page) const
{
    return toPhysAddr(page->getPageAddr());
}

PageAddr
HybridMem::toPageAddr(Addr addr) const
{
    assert((addr % granularity) == 0);
    struct PageAddr ret;
    ret.val = addr;
    return ret;
}

PageAddr
HybridMem::toPageAddr(PacketPtr pkt) const
{
    const Addr begin_addr = pkt->getAddr();
    const unsigned size = pkt->getSize();
    assert(size);
    const Addr end = begin_addr + size - 1;
    const Addr addr = roundDown(begin_addr, granularity);
    assert(addr == roundDown(end, granularity));
    return toPageAddr(addr);
}

FrameAddr
HybridMem::toFrameAddrMemSide(Addr _addr) const
{
    const Addr begin_addr = _addr;
    const Addr addr = roundDown(begin_addr, granularity);
    assert((addr % granularity) == 0);
    return toFrameAddr(addr);
}

PageAddr
HybridMem::toPageAddrMemSide(Addr _addr) const
{
    const Addr begin_addr = _addr;
    const Addr addr = roundDown(begin_addr, granularity);
    assert((addr % granularity) == 0);
    return toPageAddr(addr);
}

FrameAddr
HybridMem::toFrameAddr(Addr addr) const
{
    assert((addr % granularity) == 0);
    struct FrameAddr ret;
    ret.val = addr;
    return ret;
}

FrameAddr
HybridMem::toFrameAddr(PacketPtr pkt) const
{
    const Addr begin_addr = pkt->getAddr();
    const unsigned size = pkt->getSize();
    assert(size);
    const Addr end = begin_addr + size - 1;
    const Addr addr = roundDown(begin_addr, granularity);
    assert(addr == roundDown(end, granularity));
    return toFrameAddr(addr);
}

bool
HybridMem::isAscending(const std::vector<AddrRange> &ranges) const
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
HybridMem::totalSizeOf(const std::vector<AddrRange> &ranges) const
{
    Addr sum = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        sum += ranges[i].size();
    }
    return sum;
}

bool
HybridMem::canBeDrained()
{
    return (wait == WaitState::wNONE) && !waitSlaveRetry &&
           (sent == SentState::sIDLE) && migrationTasks.empty();
}

DrainState
HybridMem::drain()
{
    warn_once("Current HybridMem can't serialize the modified mapping table. "
        "If restored, the table will be the same as the initialization.\n");

    if (!canBeDrained()) {
        DPRINTF(Drain, "HybridMem not drained\n");
        return DrainState::Draining;
    } else {
        return DrainState::Drained;
    }
}

void
HybridMem::predicRowHitOrMiss(class Page * page)
{
    if (page->lastAccessTick == 0) {
        page->predictRowMiss++;
        page->lastAccessTick = curTick();
        return;
    }

    if (page->isInDram) {//remove "!" after finish
        //predic pcm
        Tick threshold = avgTimeSwitchRowPCM;
        Tick now_time_stamp = curTick();
        if (now_time_stamp < page->lastAccessTick) {
            now_time_stamp = curTick() + 12700000;
        }

        if ((now_time_stamp - page->lastAccessTick) < threshold) {
            page->predictRowHit++;
        } else {
            page->predictRowMiss++;
            page->lastAccessTick = curTick();
        }
    } else {
        //predic dram
        Tick threshold = avgTimeSwitchRowDRAM;
        Tick now_time_stamp = curTick();
        if (now_time_stamp < page->lastAccessTick) {
            now_time_stamp = curTick() + 12700000;
        }

        if ((now_time_stamp - page->lastAccessTick) < threshold) {
            page->predictRowHit++;
        } else {
            page->predictRowMiss++;
            page->lastAccessTick = curTick();
        }
    }

}

void
HybridMem::updateStatisticInfo()
{
    Stats::VResult vr;
    Stats::Result result;

    updateBWInfo();

    ctrlptrs[mainMem_id.val]->avgMemAccLatPI.result(vr);
    avgMemLatencyPCM = std::isnan(vr.at(0))? 0.0 : vr.at(0);
    avgMemLatencyPCM = std::isinf(vr.at(0))? 0.0 : avgMemLatencyPCM;
    ctrlptrs[cacheMem_id.val]->avgMemAccLatPI.result(vr);
    avgMemLatencyDRAM = std::isnan(vr.at(0))? 0.0 : vr.at(0);
    avgMemLatencyDRAM = std::isinf(vr.at(0))? 0.0 : avgMemLatencyDRAM;

    avgTimeSwitchRowPCM = ctrlptrs[mainMem_id.val]->avgTimeSwitchRow();
    avgTimeSwitchRowDRAM = ctrlptrs[cacheMem_id.val]->avgTimeSwitchRow();

    ctrlptrs[mainMem_id.val]->avgRdQLenPI.prepare();
    result = ctrlptrs[mainMem_id.val]->avgRdQLenPI.result();
    avgRdQLenPCM = std::max(result, Stats::Result(0));
    ctrlptrs[cacheMem_id.val]->avgRdQLenPI.prepare();
    result = ctrlptrs[cacheMem_id.val]->avgRdQLenPI.result();
    avgRdQLenDRAM = std::max(result, Stats::Result(0));

    ctrlptrs[mainMem_id.val]->avgWrQLenPI.prepare();
    result = ctrlptrs[mainMem_id.val]->avgWrQLenPI.result();
    avgWrQLenPCM = std::max(result, Stats::Result(0));
    ctrlptrs[cacheMem_id.val]->avgWrQLenPI.prepare();
    result = ctrlptrs[cacheMem_id.val]->avgWrQLenPI.result();
    avgWrQLenDRAM = std::max(result, Stats::Result(0));

}

void
HybridMem::updateBWInfo()
{
    Stats::VResult vr;

    ctrlptrs[mainMem_id.val]->avgRdBWPI.result(vr);
    avgBWPCM = vr.at(0);
    ctrlptrs[mainMem_id.val]->avgWrBWPI.result(vr);
    avgWrBWPCM = std::isnan(vr.at(0))? 0.0 : vr.at(0);
    avgWrBWPCM = std::isinf(vr.at(0))? 0.0 : avgWrBWPCM;
    avgWrBWPCM = vr.at(0);
    avgBWPCM += vr.at(0);
    ctrlptrs[cacheMem_id.val]->avgRdBWPI.result(vr);
    avgBWDRAM = vr.at(0);
    ctrlptrs[cacheMem_id.val]->avgWrBWPI.result(vr);
    avgWrBWDRAM = std::isnan(vr.at(0))? 0.0 : vr.at(0);
    avgWrBWDRAM = std::isinf(vr.at(0))? 0.0 : avgWrBWDRAM;
    avgWrBWDRAM = vr.at(0);
    avgBWDRAM += vr.at(0);
}

void
HybridMem::CountScoreinc(struct PageAddr pageaddr, bool isRead, bool hit, MasterID id)
{
    class Page *page = pages.pageOf(pageaddr);
    bool indram = page->isInDram;

    if (page->lastAccessInterval != totalInterval) {
        page->resetPageCounters();
        page->lastAccessInterval = totalInterval;
    }

    if (isRead) {
        ReadCountinc(page);
    } else {
        WriteCountinc(page);
    }

    if (indram) {

    } else {
        if (!hit) {
            if (isRead) {
                incRowBufferMissCount(pageaddr, 1, isRead);
            } else {
                incRowBufferMissCount(pageaddr, 1, isRead);
            }
        }
    }
}

void
HybridMem::ReadCountinc(class Page *const page)
{
    Addr pageAddr = page->getPageAddr().val;
    auto iterBool = mapRef.insert(pageAddr);

    if (iterBool.second == true) {
        refPagePerIntervalnum++;
        if (page->isInDram) {
            // ref_pages_in_DRAM_queue.push_back(pageAddr);
            refPageinDramPerIntervalnum++;
        } else {
            // ref_pages_in_PCM_queue.push_back(pageAddr);
            refPageinPcmPerIntervalnum++;
        }
    }

    int preReadcount = page->readcount++;
    assert(preReadcount < page->readcount);

    if (page->isInDram && page->justMigrate) {
        ++numReadDram;
    }
}

void
HybridMem::WriteCountinc(class Page *const page)
{
    Addr pageAddr = page->getPageAddr().val;
    auto iterBool = mapRef.insert(pageAddr);

    if (iterBool.second == true) {
        refPagePerIntervalnum++;
        if (page->isInDram) {
            // ref_pages_in_DRAM_queue.push_back(host_PN);
            refPageinDramPerIntervalnum++;
        } else {
            // ref_pages_in_PCM_queue.push_back(host_PN);
            refPageinPcmPerIntervalnum++;
        }
    }

    int preWritecount = page->writecount++;
    assert(preWritecount < page->writecount);

    if (page->isInDram && page->justMigrate) {
        ++numWriteDram;
    }

    if (!page->isInDram && page->isValid(cacheMem_id)) {
        page->freeFrame(cacheMem_id);
    }
}

size_t
HybridMem::addScoreToPage(class Page *page, size_t score)
{
    size_t pre = page->RWScoresPerInterval;

    size_t infMax;
    if (page->isInDram) {
        infMax = infDramMax;
    } else {
        infMax = infPcmMax;
    }

    page->RWScoresPerInterval += score;
    assert(page->RWScoresPerInterval > pre);

    if (page->RWScoresPerInterval > infMax) {
        page->RWScoresPerInterval = infMax;
    }
    assert(page->RWScoresPerInterval >= pre);

    return (page->RWScoresPerInterval - pre);
}

void
HybridMem::incRowBufferMissCount(struct PageAddr pageaddr, size_t value, bool isRead)
{
    class Page *page = pages.pageOf(pageaddr);
	if (page->isInDram) {
		std::cout<<"Pages in DRAM inc RowBufferCount!\n";
		exit(-1);
	}
    Addr pageAddr = page->getPageAddr().val;
    if (isRead) {
        page->RowBufMissRd += value;
    } else {
        page->RowBufMissWr += value;
    }

    MLPAvgCal(page);
    double utility =
        page->RowBufMissRd * 22 * page->avgReadMLP +
        page->RowBufMissWr * 172 * page->avgWriteMLP;
	if (utility > MissThreshold) {
		statsStore.erase(pageAddr);
		page->resetPageCounters();
        // page->RowBufMissWr = 0;
        // page->justMigrate = true;

		//migrate
        class FramePool *pool = pools.poolOf(cacheMem_id);
		if (pool->isFull()) {
			std::cout<<"DRAM EMPTY!!\n";
			exit(-1);
		} else {
			genMigrationTasks(page, true);
		}
	} else {
        struct PageAddr evictPN;
		evictPN = statsStore.put(pageAddr);
		if (evictPN.val != std::numeric_limits<Addr>::max()) {
            class Page *_page = pages.pageOf(evictPN);
			_page->resetPageCounters();
		}
	}
}

void
HybridMem::incMissThres()
{
	int pre = MissThreshold++;
	if(pre >= MissThreshold)
	{
		printf("MissThres too big!\n");
		exit(-1);
	}
	dirMissThreshold = true;
}

void
HybridMem::decMissThres()
{
	int pre = MissThreshold--;
    if(pre <= MissThreshold)
	{
		printf("MissThres too big!\n");
		exit(-1);
	}
	if(MissThreshold < 0)
		MissThreshold = 0;
	dirMissThreshold = false;
}

void
HybridMem::memNumInc(MasterID id, bool isRead)
{
    if (id == masterId) {return;}

    std::unordered_map<MasterID, int>::iterator iter;
    assert((iter = cacheIDmap.find(id)) != cacheIDmap.end());
    if (isRead) {
        int64_t preCnt = cpuRdReqs[iter->second]++;
        assert(preCnt < cpuRdReqs[iter->second]);
    } else {
        int64_t preCnt = cpuWrReqs[iter->second]++;
        assert(preCnt < cpuWrReqs[iter->second]);
    }
}

void
HybridMem::memNumDec(MasterID id, bool isRead)
{
    if (id == masterId) {return;}

    std::unordered_map<MasterID, int>::iterator iter;
    assert((iter = cacheIDmap.find(id)) != cacheIDmap.end());
    if (isRead) {
        int64_t preCnt = cpuRdReqs[iter->second]--;
        assert(preCnt > cpuRdReqs[iter->second]);
    } else {
        int64_t preCnt = cpuWrReqs[iter->second]--;
        assert(preCnt > cpuWrReqs[iter->second]);
    }
}

void
HybridMem::memReqAdd(Addr hostAddr, MasterID id, bool isRead)
{
    PageAddr pageAddr = {hostAddr};
    class Page *page = pages.pageOf(pageAddr);
    if (id == masterId) {return;}
    if (isRead) {
        page->pid = cacheIDmap.find(id)->second;
        reqsPageMapRd[burstAlign(hostAddr)] = page;
    } else {
        page->pid = cacheIDmap.find(id)->second;
        reqsPageMapWr[burstAlign(hostAddr)] = page;
    }
}

void
HybridMem::memReqPop(Addr hostAddr, MasterID id, bool isRead)
{
    if (id == masterId) {return;}
    if (isRead) {
        assert(reqsPageMapRd.find(hostAddr) != reqsPageMapRd.end());
        reqsPageMapRd.erase(hostAddr);
    } else {
        assert(reqsPageMapWr.find(hostAddr) != reqsPageMapWr.end());
        reqsPageMapWr.erase(hostAddr);
    }
}

void
HybridMem::MLPCalRd()
{
    for (auto &iter : reqsPageMapRd) {
        assert((cpuRdReqs[iter.second->pid] != 0));
        iter.second->ReadMLPAcc += 1 / (double)cpuRdReqs[iter.second->pid];
        ++iter.second->ReadMLPTimes;
    }
}

void
HybridMem::MLPCalWr()
{
    for (auto &iter : reqsPageMapWr) {
        assert((cpuWrReqs[iter.second->pid] != 0));
        iter.second->WriteMLPAcc += 1 / (double)cpuWrReqs[iter.second->pid];
        ++iter.second->WriteMLPTimes;
    }
}

void
HybridMem::MLPAvgCal(class Page * page)
{
    if (page->ReadMLPTimes != 0)
        page->avgReadMLP = page->ReadMLPAcc / (double)page->ReadMLPTimes;
    else
        page->avgReadMLP = 0;

    if (page->WriteMLPTimes != 0)
        page->avgWriteMLP = page->WriteMLPAcc / (double)page->WriteMLPTimes;
    else
        page->avgWriteMLP = 0;
}

void
HybridMem::processMLPUpdateEvent()
{
    MLPCalRd();
    MLPCalWr();

    if (!MLPUpdateEvent.scheduled()) {
        schedule(MLPUpdateEvent, curTick() + 6*DRAMmemCycle2Tick);
    } else {
        reschedule(MLPUpdateEvent, curTick() + 6*DRAMmemCycle2Tick);
    }

}

void
HybridMem::resetStats() {
    std::cout<< "HybridMem resetstates\n";
    intervalCount = 0;
    balanceCount = 0;
    unbalanceCount = 0;
    rightRatioSum = 0;
    totMemMigrationTime = 0;
    lastWarmupAt =curTick() - bootUpTick;
}

void
HybridMem::regStats()
{
    using namespace Stats;
    ClockedObject::regStats();

    registerResetCallback(new HybridmemResetCallback(this));

    readReqs
        .name(name() + ".readReqs")
        .desc("Number of read requests accepted");

    writeReqs
        .name(name() + ".writeReqs")
        .desc("Number of write requests accepted");

    intervalCount
        .name(name() + ".intervalCount")
        .desc("Total number of interval");

    balanceCount
        .name(name() + ".balanceCount")
        .desc("Total number of balance interval");

    unbalanceCount
        .name(name() + ".unbalanceCount")
        .desc("Total number of unbalance interval");

    rightRatioSum
        .name(name() + ".rightRatioSum")
        .desc("Sum of each right ratio");

    rightRatio
        .name(name() + ".rightRatio")
        .desc("right ratio unit: percent")
        .precision(2);

    rightRatio = (rightRatioSum *100) / intervalCount;

    totMemMigrationTime
        .name(name() + ".totMemMigrationTime")
        .desc("Total ticks spent from issue first migration pkt until  "
              " all migration done");

    totBlockedReqsForMigration
        .name(name() + ".totBlockedReqsForMigration")
        .desc("Total number of reqs blocked for migration");

    lastWarmupAt
        .name(name() + ".lastWarmupAt")
        .desc("last warmup/reset tick");

    badMigrationPageCount
        .name(name() + ".badMigrationPageCount")
        .desc("The count of page migrated to and back PCM/DRAM");

    migrationPageCount
        .name(name() + ".migrationPageCount")
        .desc("The count of page migrated to or back PCM/DRAM");

    totBlockedreqMemAccLat
        .name(name() + ".totBlockedreqMemAccLat")
        .desc("Total ticks of blocked req spent from burst creation "
              "until serviced by the Ctrl");

    totBlockedreqMemAccLatWDelay
        .name(name() + ".totBlockedreqMemAccLatWDelay")
        .desc("Total ticks of blocked req with delay spent from burst creation "
              "until serviced by the Ctrl");

    avgBlockedreqMemAccLat
        .name(name() + ".avgBlockedreqMemAccLat")
        .desc("Average memory access latency per blocked burst")
        .precision(2);

    avgBlockedreqMemAccLatWDelay
        .name(name() + ".avgBlockedreqMemAccLatWDelay")
        .desc("Average memory access latency per blocked burst with delay")
        .precision(2);

    avgBlockedreqMemAccLat =
        totBlockedreqMemAccLat / totBlockedReqsForMigration;
    avgBlockedreqMemAccLatWDelay =
        totBlockedreqMemAccLatWDelay / totBlockedReqsForMigration;

    totGap
        .name(name() + ".totGap")
        .desc("Total gap between requests");

    avgGap
        .name(name() + ".avgGap")
        .desc("Average gap between requests")
        .precision(2);

    avgGap = totGap / (readReqs + writeReqs);
}

HybridMem*
HybridMemParams::create()
{
    return new HybridMem(this);
}
