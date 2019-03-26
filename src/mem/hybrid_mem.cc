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

#include "mem/hybrid_mem.hh"

#include "sim/system.hh"
#include "debug/Drain.hh"

HybridMem::HybridMem(const HybridMemParams* p)
    : MemObject(p),
      slavePort(name() + ".slave", *this),
      masterPort(name() + ".master", *this),
      physRanges(p->phys_ranges),
      memRanges(p->mem_ranges),
      channelRanges(p->channel_ranges),
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
      releaseSentStateEvent([this]{ releaseSentState(); }, name())
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
        }
    }
}

BaseSlavePort&
HybridMem::getSlavePort(const std::string& if_name, PortID idx)
{
    if (if_name == "slave") {
        return slavePort;
    } else {
        return MemObject::getSlavePort(if_name, idx);
    }
}

BaseMasterPort&
HybridMem::getMasterPort(const std::string& if_name, PortID idx)
{
    if (if_name == "master") {
        return masterPort;
    } else {
        return MemObject::getMasterPort(if_name, idx);
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
            wait = WaitState::wMASTER_wSELF;
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
        assert(!(isRead && isWrite));
        if (isRead) { page->launchReadTo(channel_idx); }
        if (isWrite) { page->launchWriteTo(channel_idx); }
    }

    if (!successful) {
        if (false) {
            assert(0);
        } else if (wait == WaitState::wNONE) {
            wait = WaitState::wMASTER;
        } else if (wait == WaitState::tryMASTER) {
            wait = WaitState::wMASTER;
        } else if (wait == WaitState::tryMASTER_wSELF) {
            wait = WaitState::wMASTER_wSELF;
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
        wait = WaitState::wMASTER_wSELF;
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
                wait = WaitState::tryMASTER_wSELF;
                slavePort.sendRetryReq();
                if (wait == WaitState::tryMASTER_wSELF) {
                    wait = WaitState::wSELF;
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
                wait = WaitState::tryMASTER_wSELF;
                slavePort.sendRetryReq();
                if (wait == WaitState::tryMASTER_wSELF) {
                    wait = WaitState::wSELF;
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
        // do something
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

HybridMem*
HybridMemParams::create()
{
    return new HybridMem(this);
}
