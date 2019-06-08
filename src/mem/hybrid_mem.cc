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
#include "mem/hybrid_mem.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"
#include "debug/Drain.hh"

#define MISSHITRATIOPCM 2.1
#define MISSHITRATIODRAM 2.6
#define RWLATENCYRATIOPCM 4

using namespace std;
using namespace Data;

HybridMem::HybridMem(const HybridMemParams* p)
    : ClockedObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      physRanges(p->phys_ranges),
      memRanges(p->mem_ranges),
      channelRanges(p->channel_ranges),
      DramLFUDA(128, 0, 64), PcmLFUDA(128, 0, 64),
      rankingDramLRU(128), rankingPcmLRU(128),
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
      warmUpEvent([this]{processWarmUpEvent(); }, name()),
      regularBalancedEvent([this]{processRegularBalancedEvent(); }, name()),
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
      reqInDramCountPI(0), MigrationTimeStartAt(0)
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
}

void
HybridMem::startup()
{
    // schedule(warmUpEvent, timeWarmup);
    if (timeInterval == 0)
        return;
    schedule(regularBalancedEvent, curTick() + timeInterval);
}

void
HybridMem::processWarmUpEvent()
{

    std::cout<<" HybridMem Warm up at"<<curTick()<<std::endl;

    for (int i = 0; i < channelRanges.size(); ++i) {
        ctrlptrs[i]->resetAllStats();
    }

    Stats::schedStatEvent(false, true, curTick(), 0);

}

void
HybridMem::rightRatioCheck()
{
    double count = 0.0;
    std::cout<< "LFUDA_PCM :";
    PcmLFUDA.printLFU();
    std::cout<< "LFUDA_DRAM :";
    DramLFUDA.printLFU();

    for (auto &iter : migrationPagesPI) {
        std::cout<<iter.second<<", ";
        if (iter.second > 5) count+=1;
    }
    rightRatioSum += std::isnan(count /migrationPagesPI.size())?
                        0.0 : count /migrationPagesPI.size();
    std::cout<<"\n"<<"Right ratio: "<<count /migrationPagesPI.size()<<"\n";
    migrationPagesPI.clear();

    updateStatisticInfo();
    std::cout<<"D-RQL: "<<avgRdQLenDRAM<<" D-WQL: "<<avgWrQLenDRAM<<"\n";
    std::cout<<"P-RQL: "<<avgRdQLenPCM <<" P-WQL: "<<avgWrQLenPCM <<"\n";
    std::cout<<"PCM "<<avgMemLatencyPCM<<"x"<< ctrlptrs[mainMem_id.val]->readBurstsPI.value()<<" DRAM "<<avgMemLatencyDRAM<<"x"<< ctrlptrs[cacheMem_id.val]->readBurstsPI.value()<<"\n";
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

    rightRatioCheck();

    ++skipInterval;
    if (skipInterval < 1) {
        resetPerInterval();
        return;
    }

    size_t migrationPageNum;

    double LatencyXTickPCM = avgMemLatencyPCM * avgRdQLenPCM;
    double LatencyXTickDRAM = avgMemLatencyDRAM * avgRdQLenDRAM;
    double targetLatencyXTick = (LatencyXTickPCM + LatencyXTickDRAM) / 2;
    if (std::abs(LatencyXTickPCM - LatencyXTickDRAM) < targetLatencyXTick*0.125) {
        ++balanceCount;
        std::cout<<"Balanced"<<"\n";
        resetPerInterval();
        if (!hasWarmedUp) {
            hasWarmedUp = true;
            schedule(warmUpEvent, curTick() + timeWarmup);
        }
        return;
    }
    ++unbalanceCount;
    MigrationTimeStartAt = curTick();
    // pendingReqsPriorMigration =
    //             dirCtrlPtr->memoryPort.reqQueue.transmitList.size();
    if (LatencyXTickDRAM < LatencyXTickPCM) {
        // std::vector<Addr> tmpHostPages;
        // rankingPcmLRU.getAllHostPages(tmpHostPages);
        // getPageRanking(tmpHostPages);
        // sort(Ranking.begin(), Ranking.end(), PCMtoDRAMsort);

        // getMigrationPageNum(migrationPageNum, avgBWDRAM, avgBWPCM);
        getMigrationPageNum(migrationPageNum, LatencyXTickDRAM, LatencyXTickPCM);
        // migrationPageNum = PcmLFUDA.capacity();

        genMigrationTasks(migrationPageNum, true);

    } else {
        if (DramLFUDA.isEmpty()) {
            resetPerInterval();
            return;
        }
        // std::vector<Addr> tmpHostPages;
        // rankingDramLRU.getAllHostPages(tmpHostPages);
        // getPageRanking(tmpHostPages);
        // sort(Ranking.begin(), Ranking.end(), DRAMtoPCMsort);

        // getMigrationPageNum(migrationPageNum, avgBWDRAM, avgBWPCM);
        getMigrationPageNum(migrationPageNum, LatencyXTickDRAM, LatencyXTickPCM);

        genMigrationTasks(migrationPageNum, false);

    }

    resetPerInterval();

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
HybridMem::getMigrationPageNum(size_t& migrationPageNum,
                                double DRAMLatency, double PCMLatency)
{
    pcmScore = std::max(pcmScore, uint64_t(1));
    dramScore = std::max(dramScore, uint64_t(1));

    double pcmLatencyPerScore = double(PCMLatency) / pcmScore;
    double dramLatencyPerScore = double(DRAMLatency) / dramScore;
    size_t needMove = 0;
    size_t needDramPage = 0;
    double targetLatency = double(DRAMLatency + PCMLatency) / 2;
    // double dramAvgQLen = avgRdQLenDRAM + 1;
    // double pcmAvgQLen = avgRdQLenPCM + 1;

    if (DRAMLatency <= PCMLatency) {
        if (PCMLatency > 0 && DRAMLatency > 0) {
            // if (waiting_by_dram + waiting_by_pcm > 5) {
            //     dram_avg_req = 5;
            //     pcm_avg_req = 5;
            // }
            int cur = PcmLFUDA.head;
            size_t freeFrames = pools.poolOf(cacheMem_id)->getFreeFrameSize();
            for (unsigned long int i = 0; i < PcmLFUDA.capacity() && DRAMLatency < targetLatency && PCMLatency > targetLatency ; i++, cur = PcmLFUDA.lfuNodes[cur].next) {
                Addr host_PN = PcmLFUDA.lfuNodes[cur].hostAddr;
                size_t rowHit;
                size_t rowMiss;
                struct PageAddr pageAddr = {host_PN};
                class Page *page = pages.pageOf(pageAddr);
                struct FrameAddr dram_PN = {std::numeric_limits<Addr>::max()};
                if (page->isValid(cacheMem_id))
                    dram_PN = page->getFrameAddr(cacheMem_id);

                if (dram_PN.val == std::numeric_limits<Addr>::max()) {
                    if (needDramPage < freeFrames) {
                        needDramPage++;
                    } else if (freeFrames == 0) {
                        printf("DRAM is empty!\n");
                        exit(-1);
                    } else {
                        break;
                    }
                }

                rowHit = page->predictRowHit;
                rowMiss = page->predictRowMiss;

                double infDram =
                        (double)(rowHit + rowMiss * MISSHITRATIODRAM);
                double infPcm = (double)page->RWScoresPerInterval;

                assert(infPcm <= infPcmMax);
                if (infDram > infDramMax) {
                    infDram = infDramMax;
                }

                // DRAMLatency += infDram * dramLatencyPerScore *avgRdQLenDRAM;
                // PCMLatency -= infPcm * pcmLatencyPerScore*avgWrQLenPCM;
                DRAMLatency += infDram * dramLatencyPerScore;
                PCMLatency -= infPcm * pcmLatencyPerScore;
                needMove++;
            }

            migrationPageNum = needMove;
        } else {
            migrationPageNum = (refPageinPcmPerIntervalnum * 0.20);
        }
    } else {
        if (PCMLatency > 0 && DRAMLatency > 0) {
            // if (waiting_by_dram + waiting_by_pcm > 5) {
            //     dram_avg_req = 5;
            //     pcm_avg_req = 5;
            // }
            int cur = DramLFUDA.tail;
            for (unsigned long int i = 0; i < DramLFUDA.capacity() && DRAMLatency > targetLatency && PCMLatency < targetLatency ; i--, cur = DramLFUDA.lfuNodes[cur].pre) {
                Addr host_PN = DramLFUDA.lfuNodes[cur].hostAddr;
                size_t rowHit;
                size_t rowMiss;
                //int pcm_PN = page->pcm_PN;
                struct PageAddr pageAddr = {host_PN};
                class Page *page = pages.pageOf(pageAddr);

                rowHit = page->predictRowHit;
                rowMiss = page->predictRowMiss;

                double infDram = (double)page->RWScoresPerInterval;
                double infPcm = (double)(rowHit + rowMiss * MISSHITRATIOPCM);

                assert(infDram <= infDramMax);
                if (infPcm > infPcmMax) {
                    infPcm = infPcmMax;
                }

                // DRAMLatency -= infDram * dramLatencyPerScore*avgWrQLenDRAM;
                // PCMLatency += infPcm * pcmLatencyPerScore *avgWrQLenPCM;
                DRAMLatency -= infDram * dramLatencyPerScore;
                PCMLatency += infPcm * pcmLatencyPerScore;
                needMove++;
            }

            migrationPageNum = needMove;
        } else {
            migrationPageNum = (refPageinDramPerIntervalnum * 0.05);
            // migrationPageNum = std::max(migrationPageNum, size_t(1));
            /*migrationPageNum =
                            std::min(migrationPageNum, max_page_migration_from_dram_to_pcm);*/
        }
    }

    if (migrationPageNum > maxMigrationTasks)
        migrationPageNum = maxMigrationTasks;
}

void
HybridMem::genMigrationTasks(size_t &migrationPageNum, bool pcm2dram)
{
    assert(migrationPageNum <= maxMigrationTasks);
    if (pcm2dram) {
        if (pools.poolOf(cacheMem_id)->getFreeFrameSize() <
                migrationPageNum) {
            migrationPageNum /= 2;
            genDramMigrationTasks(migrationPageNum);
            std::cout<<"genDramEvictedMigrationTasks "<<migrationPageNum<<"\n";
        }

        genPcmMigrationTasks(migrationPageNum);
        std::cout<<"genPcmMigrationTasks "<<migrationPageNum<<"\n";
    } else {
        genDramMigrationTasks(migrationPageNum);
        std::cout<<"genDramMigrationTasks "<<migrationPageNum<<"\n";
    }
}

void
HybridMem::resetPerInterval()
{
    for (auto ctrlptr : ctrlptrs) {
        ctrlptr->resetPerInterval();
    }

    mapRef.clear();

    DramLFUDA.reset();
    PcmLFUDA.reset();
    rankingDramLRU.reset();
    rankingPcmLRU.reset();

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

}

void
HybridMem::resetPages()
{
    pages.resetPageCounters();

}

void
HybridMem::genDramEvictedMigrationTasks(size_t &halfMigrationPageNum)
{
    if (DramLFUDA.capacity() == 0) {
        std::cout<<"DramLFUDA is empty\n";
        halfMigrationPageNum = 0;
        return;
    }
    int pageCacheCount = 0;
    size_t migrationCount = 0;
    for (int i = Ranking.size() - 1;
        i >=int(Ranking.size() - halfMigrationPageNum); --i) {
        migrationCount++;
        Addr host_PN = Ranking[i].host_PN;
        struct PageAddr pageAddr = {host_PN};
        class Page *page = pages.pageOf(pageAddr);
        assert(page->isValid(cacheMem_id));
        assert(migrationPages.find(host_PN)!=migrationPages.end());
        migrationPages.erase(host_PN);
        if (page->isPageCache) {++pageCacheCount;}

        if (migrationPagesPI.find(host_PN)!=migrationPagesPI.end())
            migrationPagesPI.erase(host_PN);
        // struct FrameAddr frame_addr = page->getPossession();

        if (page->isInvalid(mainMem_id) || page->isUnused(mainMem_id)) {
            assert(tryIssueMigrationTask(page, cacheMem_id, mainMem_id));
        } else{
            //nothing to do
        }

        page->freeFrame(cacheMem_id);
        page->isInDram = false;
        page->claimChannelIsValid(mainMem_id);
    }
    halfMigrationPageNum = migrationCount;
    std::cout<< "Page cache count: "<< pageCacheCount<<"\n";
}

void
HybridMem::genPcmMigrationTasks(size_t &migrationPageNum)
{
    if (PcmLFUDA.capacity() == 0) {
        std::cout<<"PcmLFUDA is empty\n";
        migrationPageNum = 0;
        return;
    }
    int cur = PcmLFUDA.head;
    int pageCacheCount = 0;
    size_t migrationCount = 0;
    for (size_t i = 0; i < migrationPageNum && i < PcmLFUDA.capacity(); ++i) {
        // Addr host_PN = Ranking[i].host_PN;
        if (migrationTasks.size() >= maxMigrationTasks) {
            break;
        }
        ++migrationCount;
        Addr host_PN = PcmLFUDA.lfuNodes[cur].hostAddr;
        assert(cur != -1);
        cur = PcmLFUDA.lfuNodes[cur].next;
        struct PageAddr pageAddr = {host_PN};
        class Page *page = pages.pageOf(pageAddr);
        assert(page->isValid(mainMem_id));
        migrationPages[host_PN] = 0;
        migrationPagesPI[host_PN] = 0;
        if (page->isPageCache) {++pageCacheCount;}

        if (page->isInvalid(cacheMem_id) || page->isUnused(cacheMem_id)) {
            struct FrameAddr frame_addr;
            assert(pools.poolOf(cacheMem_id)->tryGetAnyFreeFrame(&frame_addr));
            if (page->isInvalid(cacheMem_id))
                page->freeFrame(cacheMem_id);
            page->allocFrame(cacheMem_id, frame_addr);
            assert(tryIssueMigrationTask(page, mainMem_id, cacheMem_id));

        } else {
            //nothing to do
        }
        page->isInDram = true;
        page->claimChannelIsValid(cacheMem_id);
    }
    migrationPageNum = migrationCount;
    std::cout<< "Page cache count: "<< pageCacheCount<<"\n";
}

void
HybridMem::genDramMigrationTasks(size_t &migrationPageNum)
{
    if (DramLFUDA.capacity() == 0) {
        std::cout<<"DramLFUDA is empty\n";
        migrationPageNum = 0;
        return;
    }

    int cur = DramLFUDA.tail;
    int pageCacheCount = 0;
    size_t migrationCount = 0;
    for (int i = DramLFUDA.capacity() - 1;
        i >= int(DramLFUDA.capacity() - migrationPageNum) && i >= 0 ; --i) {
        // Addr host_PN = Ranking[i].host_PN;
        if (migrationTasks.size() >= maxMigrationTasks) {
            break;
        }
        ++migrationCount;
        Addr host_PN = DramLFUDA.lfuNodes[cur].hostAddr;
        assert(cur != -1);
        cur = DramLFUDA.lfuNodes[cur].pre;
        struct PageAddr pageAddr = {host_PN};
        class Page *page = pages.pageOf(pageAddr);
        assert(page->isValid(cacheMem_id));
        assert(migrationPages.find(host_PN)!=migrationPages.end());
        migrationPages.erase(host_PN);
        if (page->isPageCache) {++pageCacheCount;}

        if (migrationPagesPI.find(host_PN)!=migrationPagesPI.end())
            migrationPagesPI.erase(host_PN);

        if (page->isInvalid(mainMem_id) || page->isUnused(mainMem_id)) {
            assert(tryIssueMigrationTask(page, cacheMem_id, mainMem_id));
        } else {
            //nothing to do
        }

        if (pools.poolOf(cacheMem_id)->getFreeFrameSize() < maxMigrationTasks)
            page->freeFrame(cacheMem_id);

        page->isInDram = false;
        page->claimChannelIsValid(mainMem_id);
    }
    migrationPageNum = migrationCount;
    std::cout<< "Page cache count: "<< pageCacheCount<<"\n";
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
    const Addr mem_addr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    if (page->inMigrating()) { flushAllMigrationPkt(); }
    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    masterPort.sendFunctional(pkt);
    pkt->setAddr(mem_addr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }
}

void
HybridMem::recvFunctionalSnoop(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    slavePort.sendFunctionalSnoop(pkt);
    pkt->setAddr(mem_addr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }
}

Tick
HybridMem::recvAtomic(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    if (page->inMigrating()) { flushAllMigrationPkt(); }
    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    Tick ret_tick = masterPort.sendAtomic(pkt);
    pkt->setAddr(mem_addr);

    assert(!(isRead && isWrite));
    if (isRead) { page->launchReadTo(channel_idx); }
    if (isWrite) { page->launchWriteTo(channel_idx); }

    return ret_tick;
}

Tick
HybridMem::recvAtomicSnoop(PacketPtr pkt)
{
    const Addr mem_addr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    assert(!page->inMigrating());

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);

    pkt->setAddr(channel_addr);
    Tick ret_tick = slavePort.sendAtomicSnoop(pkt);
    pkt->setAddr(mem_addr);

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
    if (page->migrationCount > 1) {
        ++badMigrationPageCount;
    }

    if (sys->isTimingMode()) {
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
    const Addr mem_addr = pkt->getAddr();
    const bool isRead = pkt->isRead();
    const bool isWrite = pkt->isWrite();
    class Page *page = pages.pageOf(toPageAddr(pkt));

    if (page->inMigrating()) {
        page->bookingMasterWaiting();
        totBlockedReqsForMigration += 1;
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
            totBlockedReqsForMigration += 1;
            return false;
        } else {
            assert(0);
        }
    }

    if (pkt->masterId() == dmaDeviceId && isWrite && !page->isDirty
        && page->readcount == 0 && page->writecount == 0) {
        updateBWInfo();

        // if (3.36*ctrlptrs[mainMem_id.val]->totalWriteQueueSize >= ctrlptrs[cacheMem_id.val]->totalWriteQueueSize) {
        //     //3.36* avgWrBWPCM > avgWrBWDRAM
        //     if (page->isUnused(cacheMem_id)) {
        //         struct FrameAddr frame_addr;
        //         assert(pools.poolOf(cacheMem_id)->tryGetAnyFreeFrame(&frame_addr));
        //         if (page->isInvalid(cacheMem_id))
        //             page->freeFrame(cacheMem_id);
        //         page->allocFrame(cacheMem_id, frame_addr);
        //         // assert(tryIssueMigrationTask(page, mainMem_id, cacheMem_id));

        //     } else {
        //         //nothing to do
        //     }
        //     page->isInDram = true;
        //     page->claimChannelIsValid(cacheMem_id);
        // }
        if (!warmUpEvent.scheduled()) {
            schedule(warmUpEvent, curTick() + 500000000);
        } else {
            reschedule(warmUpEvent, curTick() + 500000000);
        }
        page->isPageCache = true;
        page->writecount = page->readcount = -64;
    }

    Tick releaseSentStateTick = clockEdge(Cycles(1));
    releaseSentStateTick += (pkt->hasData()) ?
        (divCeil(pkt->getSize(), width) * clockPeriod()) : (0);

    struct ChannelIdx channel_idx = selectChannelIdx(page);
    const Addr channel_addr = toChannelAddr(page, channel_idx, pkt);
    const bool needsResponse = pkt->needsResponse();
    const bool cacheResponding = pkt->cacheResponding();

    if (needsResponse && !cacheResponding) {
        pkt->pushSenderState(new HybridMemSenderState(mem_addr));
    }
    pkt->setAddr(channel_addr);
    const bool successful = masterPort.sendTimingReq(pkt);
    if (!successful) {
        pkt->setAddr(mem_addr);
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

        predicRowHitOrMiss(page);
        assert(!(isRead && isWrite));
        if (isRead) {
            page->launchReadTo(channel_idx);
            page->readreqPerInterval++;
        }
        if (isWrite) {
            page->launchWriteTo(channel_idx);
            page->writereqPerInterval++;
            page->isDirty = true;
        }

        // /*
        // //Invalid page of bin to DRAM
        // // For simplicity, requests are assumed to be 8 byte-sized
        // */
        // PacketPtr req = genFunCmd(MemCmd::InvalidPage, master_port_id);
        // success = masterPorts[master_port_id]->sendTimingReq(req);
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

        trySendRetry();
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
        delete task;
        // std::cout<<"migrationTasks size: "<<migrationTasks.end()-migrationTasks.begin()<<"\n";
        if (migrationTasks.end()-migrationTasks.begin() == 0) {
            totMemMigrationTime += curTick() - MigrationTimeStartAt;
            // totBlockedReqsForMigration +=
            //     dirCtrlPtr->memoryPort.reqQueue.transmitList.size() -
            //                                         pendingReqsPriorMigration;
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
                totBlockedReqsForMigration += 1;
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
                totBlockedReqsForMigration += 1;
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
                totBlockedReqsForMigration += 1;
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
                totBlockedReqsForMigration += 1;
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

struct HybridMem::ChannelIdx
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

struct HybridMem::ChannelIdx
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

struct HybridMem::ChannelIdx
HybridMem::toChannelIdx(size_t i) const
{
    struct ChannelIdx ret;
    ret.val = i;
    return ret;
}

struct HybridMem::PhysAddr
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

struct HybridMem::PageAddr
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

struct HybridMem::PhysAddr
HybridMem::toPhysAddr(class Page *page) const
{
    return toPhysAddr(page->getPageAddr());
}

struct HybridMem::PageAddr
HybridMem::toPageAddr(Addr addr) const
{
    assert((addr % granularity) == 0);
    struct PageAddr ret;
    ret.val = addr;
    return ret;
}

struct HybridMem::PageAddr
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

struct HybridMem::FrameAddr
HybridMem::toFrameAddrMemSide(Addr _addr) const
{
    const Addr begin_addr = _addr;
    const Addr addr = roundDown(begin_addr, granularity);
    assert((addr % granularity) == 0);
    return toFrameAddr(addr);
}

struct HybridMem::PageAddr
HybridMem::toPageAddrMemSide(Addr _addr) const
{
    const Addr begin_addr = _addr;
    const Addr addr = roundDown(begin_addr, granularity);
    assert((addr % granularity) == 0);
    return toPageAddr(addr);
}

struct HybridMem::FrameAddr
HybridMem::toFrameAddr(Addr addr) const
{
    assert((addr % granularity) == 0);
    struct FrameAddr ret;
    ret.val = addr;
    return ret;
}

struct HybridMem::FrameAddr
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
HybridMem::CountScoreinc(struct PageAddr pageaddr, bool isRead, bool hit, uint64_t qlen)
{
    class Page *page = pages.pageOf(pageaddr);
    bool indram = page->isInDram;

    if (page->lastAccessInterval != totalInterval) {
        page->resetPageCounters();
        page->lastAccessInterval = totalInterval;
    }

    if (isRead) {
        ReadCountinc(page, qlen);
        if (page->isPageCache && page->readcount < 0) {
            return;
        }
    } else {
        WriteCountinc(page, qlen);
        if (page->isPageCache && page->writecount < 0) {
            return;
        }
    }

    if (indram) {
        if (hit)
                dramScore += addScoreToPage(page, 1);
            else
                dramScore += addScoreToPage(page, 3);
    } else {
        if (hit)
                pcmScore += addScoreToPage(page, 1);
            else
                pcmScore += addScoreToPage(page, 2);
    }
}

void
HybridMem::ReadCountinc(class Page *page, uint64_t qlen)
{
    Addr pageAddr = page->getPageAddr().val;
    auto iter = mapRef.insert(pageAddr);

    if (iter.second == true) {
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

    if (page->isPageCache && (page->readcount < 0 || page->writecount < 0)) {
        return;
    }
    if (page->isInDram) {
        if ((double)qlen > avgRdQLenDRAM) {return;}
    } else {
        if ((double)qlen < avgRdQLenPCM/2) {return;}
    }


    struct PageAddr evictPN;
    if (page->isInDram) {
        evictPN = DramLFUDA.refer(pageAddr, 1);
        // evictPN = DramLFUDA.refer(pageAddr, 1);
        // evict_PN = rankingDramLRU.put(pageAddr);
        //check_DRAM_score();
    } else {
        evictPN = PcmLFUDA.refer(pageAddr, 1);
        // evictPN = PcmLFUDA.refer(pageAddr, 1);
        // evict_PN = rankingPcmLRU.put(pageAddr);
        //check_PCM_score();
    }

    if (evictPN.val != std::numeric_limits<Addr>::max()) {
        class Page *_page = pages.pageOf(evictPN);
        if (_page->isInDram) {
            dramScore -= _page->RWScoresPerInterval;
        } else {
            pcmScore -= _page->RWScoresPerInterval;
        }
        _page->resetPageCounters();
    }
}

void
HybridMem::WriteCountinc(class Page *page, uint64_t qlen)
{
    Addr pageAddr = page->getPageAddr().val;
    auto iter = mapRef.insert(pageAddr);

    if (iter.second == true) {
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
    if (page->isPageCache && (page->readcount < 0 || page->writecount < 0)) {
        return;
    }

    if (page->isInDram) {
        if (qlen > 32) {return;}
    } else {
        if (qlen <= 32) {return;}
    }

    struct PageAddr evictPN;
    if (page->isInDram) {
        evictPN = DramLFUDA.refer(pageAddr, 1);
        // evictPN = DramLFUDA.refer(pageAddr, 1);
        // evictPN = rankingDramLRU.put(pageAddr);
        //check_DRAM_score();
    } else {
        evictPN = PcmLFUDA.refer(pageAddr, RWLATENCYRATIOPCM);
        // evictPN = PcmLFUDA.refer(pageAddr, 1);
        // evictPN = rankingPcmLRU.put(pageAddr);
        //check_PCM_score();
    }

    if (evictPN.val != std::numeric_limits<Addr>::max()) {
        class Page *_page = pages.pageOf(evictPN);
        if (_page->isInDram) {
            dramScore -= _page->RWScoresPerInterval;
        } else {
            pcmScore -= _page->RWScoresPerInterval;
        }
        // std::cout<<"Evict D-value"<<_page->readcount+_page->writecount*4<<"\n";
        _page->resetPageCounters();
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

HybridMem::LRU::LRU(int length)
{
    head = -1;
    tail = -1;
    size = 0;
    max_size = length;

    member = new node[length];
    for (int i = 0 ; i < length ; i++) {
        member[i].hostAddr = std::numeric_limits<Addr>::max();
        member[i].next = -1;
        member[i].pre = -1;
    }

    exist = new bool[length]();
    for (int i = 0 ; i < length ; i++) {
        exist[i] = false;
    }
}

void
HybridMem::LRU::reset()
{
    head = -1;
    tail = -1;
    size = 0;

    for (int i = 0 ; i < max_size ; i++) {
        exist[i] = false;
    }

    map_index.clear();
}


bool
HybridMem::LRU::isEmpty()
{
    bool rv = true;
    for (int i = 0 ; i < max_size ; i++) {
        if (exist[i]) {
           rv = false; return rv;
        }

    }

    return rv;
}

struct HybridMem::PageAddr
HybridMem::LRU::put(Addr hostAddr)
{
    struct PageAddr pageAddr;
    if (size == 0)
        return firstPut(hostAddr);

    auto iter = map_index.find(hostAddr);

    if (iter != map_index.end()) {
        //find
        int __index = iter->second;
        return findInLRU(__index);
    } else {
        //not find
        if (size == max_size) {
            Addr evict_Addr = member[tail].hostAddr;
            int pre = member[tail].pre;
            member[pre].next = -1;
            member[tail].pre = -1;
            int n = map_index.erase(evict_Addr);
            if (n != 1) {
                printf("LRU error!1\n");
                exit(-1);
            }

            member[tail].hostAddr = hostAddr;
            member[tail].next = head;
            member[head].pre = tail;
            map_index[hostAddr] = tail;

            head = tail;
            tail = pre;
            pageAddr.val = evict_Addr;
            return pageAddr;
        } else {
            int __index;
            for (__index = 0 ; __index < max_size ; __index++) {
                if (exist[__index] == false) {
                    exist[__index] = true;
                    break;
                }
            }
            member[__index].hostAddr = hostAddr;
            map_index[hostAddr] = __index;
            member[head].pre = __index;
            member[__index].next = head;
            member[__index].pre = -1;
            head = __index;
            size++;
            pageAddr.val = std::numeric_limits<Addr>::max();
            return pageAddr;
        }
    }
}

struct HybridMem::PageAddr
HybridMem::LRU::firstPut(Addr hostAddr)
{
    assert(size == 0);

    struct PageAddr pageAddr;
    member[size].hostAddr = hostAddr;
    member[size].pre = -1;
    member[size].next = -1;
    map_index[hostAddr] = size;
    head = size;
    tail = size;
    exist[size] = true;
    size++;
    pageAddr.val = std::numeric_limits<Addr>::max();
    return pageAddr;
}

struct HybridMem::PageAddr
HybridMem::LRU::findInLRU(int index)
{
    struct PageAddr pageAddr;
    if (index != head) {
        if (index != tail) {
            int next = member[index].next;
            int pre = member[index].pre;
            member[pre].next = next;
            member[next].pre = pre;
            member[index].pre = -1;
            member[index].next = head;
            member[head].pre = index;
            head = index;
        } else {
            int pre = member[index].pre;
            member[pre].next = -1;
            member[index].pre = -1;
            member[index].next = head;
            member[head].pre = index;
            head = index;
            tail = pre;
        }
    }

    pageAddr.val = std::numeric_limits<Addr>::max();
    return pageAddr;
}

void
HybridMem::LRU::getAllHostPages(std::vector<Addr>& v)
{
    for (int i = 0 ; i < max_size ; i++) {
        if (exist[i] == true) {
            v.push_back(member[i].hostAddr);
        }
    }
    assert(v.size() == (size_t)size);
}

void
HybridMem::resetStats() {
    std::cout<< "HybridMem resetstates\n";
    intervalCount = 0;
    balanceCount = 0;
    unbalanceCount = 0;
    rightRatioSum = 0;
    totMemMigrationTime = 0;
    lastWarmupAt =curTick();
}

void
HybridMem::regStats()
{
    using namespace Stats;
    ClockedObject::regStats();

    registerResetCallback(new HybridmemResetCallback(this));

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
}

HybridMem*
HybridMemParams::create()
{
    return new HybridMem(this);
}
