/*
 * Copyright (c) 2018 ARM Limited
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
 * Authors: Matteo Andreozzi
 */

#include "debug/QOS.hh"
#include "mem/abstract_mem.hh"
#include "mem/qos/q_policy.hh"
#include "mem/qos/policy.hh"
#include "params/QoSMemCtrl.hh"
#include "sim/system.hh"

#include <unordered_map>
#include <vector>
#include <deque>

#ifndef __MEM_QOS_MEM_CTRL_HH__
#define __MEM_QOS_MEM_CTRL_HH__

namespace QoS {

/**
 * The QoS::MemCtrl is a base class for Memory objects
 * which support QoS - it provides access to a set of QoS
 * scheduling policies
 */
class MemCtrl: public AbstractMemory
{
  public:
    /** Bus Direction */
    enum BusState { READ, WRITE };

  protected:
    /** QoS Policy, assigns QoS priority to the incoming packets */
    const std::unique_ptr<Policy> policy;

    /** QoS Bus Turnaround Policy: selects the bus direction (READ/WRITE) */
    const std::unique_ptr<TurnaroundPolicy> turnPolicy;

    /** QoS Queue Policy: selects packet among same-priority queue */
    const std::unique_ptr<QueuePolicy> queuePolicy;

    /** Number of configured QoS priorities */
    const uint8_t _numPriorities;

    /** Enables QoS priority escalation */
    const bool qosPriorityEscalation;

    /**
     * Enables QoS synchronized scheduling invokes the QoS scheduler
     * on all masters, at every packet arrival.
     */
    const bool qosSyncroScheduler;

    /** Hash of master ID - master name */
    std::unordered_map<MasterID, const std::string> masters;

    /** Hash of masters - number of packets queued per priority */
    std::unordered_map<MasterID, std::vector<uint64_t> > packetPriorities;

    /** Hash of masters - address of request - queue of times of request */
    std::unordered_map<MasterID,
            std::unordered_map<uint64_t, std::deque<uint64_t>> > requestTimes;

    /**
     * Vector of QoS priorities/last service time. Refreshed at every
     * qosSchedule call.
     */
    std::vector<Tick> serviceTick;

    /** Read request packets queue length in #packets, per QoS priority */
    std::vector<uint64_t> readQueueSizes;

    /** Write request packets queue length in #packets, per QoS priority */
    std::vector<uint64_t> writeQueueSizes;

    /** Total read request packets queue length in #packets */
    uint64_t totalReadQueueSize;

    /** Total write request packets queue length in #packets */
    uint64_t totalWriteQueueSize;

    /**
     * Bus state used to control the read/write switching and drive
     * the scheduling of the next request.
     */
    BusState busState;

    /** bus state for next request event triggered */
    BusState busStateNext;

    /** per-master average QoS priority */
    Stats::VectorStandardDeviation avgPriority;
    /** per-master average QoS distance between assigned and queued values */
    Stats::VectorStandardDeviation avgPriorityDistance;

    /** per-priority minimum latency */
    Stats::Vector priorityMinLatency;
    /** per-priority maximum latency */
    Stats::Vector priorityMaxLatency;
    /** Count the number of turnarounds READ to WRITE */
    Stats::Scalar numReadWriteTurnArounds;
    /** Count the number of turnarounds WRITE to READ */
    Stats::Scalar numWriteReadTurnArounds;
    /** Count the number of times bus staying in READ state */
    Stats::Scalar numStayReadState;
    /** Count the number of times bus staying in WRITE state */
    Stats::Scalar numStayWriteState;

    /** registers statistics */
    void regStats() override;
    
    /**
     * Reset all stats
     */
    void resetAllStats();

    /**
     * Initializes dynamically counters and
     * statistics for a given Master
     *
     * @param m_id the master ID
     */
    void addMaster(const MasterID m_id);

    /**
     * Called upon receiving a request or
     * updates statistics and updates queues status
     *
     * @param dir request direction
     * @param m_id master id
     * @param qos packet qos value
     * @param addr packet address
     * @param entries number of entries to record
     */
    void logRequest(BusState dir, MasterID m_id, uint8_t qos,
                    Addr addr, uint64_t entries);

    /**
     * Called upon receiving a response,
     * updates statistics and updates queues status
     *
     * @param dir response direction
     * @param m_id master id
     * @param qos packet qos value
     * @param addr packet address
     * @param entries number of entries to record
     * @param delay response delay
     */
    void logResponse(BusState dir, MasterID m_id, uint8_t qos,
                     Addr addr, uint64_t entries, double delay);

    /**
     * Assign priority to a packet by executing
     * the configured QoS policy.
     *
     * @param queues_ptr list of pointers to packet queues
     * @param queue_entry_size size in bytes per each packet in the queue
     * @param pkt pointer to the Packet
     * @return a QoS priority value
     */
    template<typename Queues>
    uint8_t qosSchedule(std::initializer_list<Queues*> queues_ptr,
                        uint64_t queue_entry_size, const PacketPtr pkt);

    using SimObject::schedule;
    uint8_t schedule(MasterID m_id, uint64_t data);
    uint8_t schedule(const PacketPtr pkt);

    /**
     * Returns next bus direction (READ or WRITE)
     * based on configured policy.
     */
    BusState selectNextBusState();

    /**
     * Set current bus direction (READ or WRITE)
     * from next selected one
     */
    void setCurrentBusState() { busState = busStateNext; }

    /**
     * Record statistics on turnarounds based on
     * busStateNext and busState values
     */
    void recordTurnaroundStats();

    /**
     * Escalates/demotes priority of all packets
     * belonging to the passed master to given
     * priority value
     *
     * @param queues list of pointers to packet queues
     * @param queue_entry_size size of an entry in the queue
     * @param m_id master whose packets priority will change
     * @param tgt_prio target priority value
     */
    template<typename Queues>
    void escalate(std::initializer_list<Queues*> queues,
                  uint64_t queue_entry_size,
                  MasterID m_id, uint8_t tgt_prio);

    /**
     * Escalates/demotes priority of all packets
     * belonging to the passed master to given
     * priority value in a specified cluster of queues
     * (e.g. read queues or write queues) which is passed
     * as an argument to the function.
     * The curr_prio/tgt_prio parameters are queue selectors in the
     * queue cluster.
     *
     * @param queues reference to packet queues
     * @param queue_entry_size size of an entry in the queue
     * @param m_id master whose packets priority will change
     * @param curr_prio source queue priority value
     * @param tgt_prio target queue priority value
     */
    template<typename Queues>
    void escalateQueues(Queues& queues, uint64_t queue_entry_size,
                        MasterID m_id, uint8_t curr_prio, uint8_t tgt_prio);

  public:
    /**
     * QoS Memory base class
     *
     * @param p pointer to QoSMemCtrl parameters
     */
    MemCtrl(const QoSMemCtrlParams*);

    virtual ~MemCtrl();

    /**
     * Initializes this object
     */
    void init() override;

    /**
     * Gets the current bus state
     *
     * @return current bus state
     */
    BusState getBusState() const { return busState; }

    /**
     * Gets the next bus state
     *
     * @return next bus state
     */
    BusState getBusStateNext() const { return busStateNext; }

    /**
     * hasMaster returns true if the selected master(ID) has
     * been registered in the memory controller, which happens if
     * the memory controller has received at least a packet from
     * that master.
     *
     * @param m_id master id to lookup
     * @return true if the memory controller has received a packet
     *         from the master, false otherwise.
     */
    bool hasMaster(MasterID m_id) const
    {
        return masters.find(m_id) != masters.end();
    }

    /**
     * Gets a READ queue size
     *
     * @param prio QoS Priority of the queue
     * @return queue size in packets
     */
    uint64_t getReadQueueSize(const uint8_t prio) const
    { return readQueueSizes[prio]; }

    /**
     * Gets a WRITE queue size
     *
     * @param prio QoS Priority of the queue
     * @return queue size in packets
     */
    uint64_t getWriteQueueSize(const uint8_t prio) const
    { return writeQueueSizes[prio]; }

    /**
     * Gets the total combined READ queues size
     *
     * @return total queues size in packets
     */
    uint64_t getTotalReadQueueSize() const { return totalReadQueueSize; }

    /**
     * Gets the total combined WRITE queues size
     *
     * @return total queues size in packets
     */
    uint64_t getTotalWriteQueueSize() const { return totalWriteQueueSize; }

    /**
     * Gets the last service tick related to a QoS Priority
     *
     * @param prio QoS Priority
     * @return tick
     */
    Tick getServiceTick(const uint8_t prio) const { return serviceTick[prio]; }

    /**
     * Gets the total number of priority levels in the
     * QoS memory controller.
     *
     * @return total number of priority levels
     */
    uint8_t numPriorities() const { return _numPriorities; }
};

template<typename Queues>
void
MemCtrl::escalateQueues(Queues& queues, uint64_t queue_entry_size,
                        MasterID m_id, uint8_t curr_prio, uint8_t tgt_prio)
{
    auto it = queues[curr_prio].begin();
    while (it != queues[curr_prio].end()) {
        // No packets left to move
        if (packetPriorities[m_id][curr_prio] == 0)
            break;

        auto pkt = *it;

        DPRINTF(QOS,
                "QoSMemCtrl::escalate checking priority %d packet "
                "m_id %d address %d\n", curr_prio,
                pkt->masterId(), pkt->getAddr());

        // Found a packet to move
        if (pkt->masterId() == m_id) {

            uint64_t moved_entries = divCeil(pkt->getSize(),
                                             queue_entry_size);

            DPRINTF(QOS,
                    "QoSMemCtrl::escalate Master %s [id %d] moving "
                    "packet addr %d size %d (p size %d) from priority %d "
                    "to priority %d - "
                    "this master packets %d (entries to move %d)\n",
                    masters[m_id], m_id, pkt->getAddr(),
                    pkt->getSize(),
                    queue_entry_size, curr_prio, tgt_prio,
                    packetPriorities[m_id][curr_prio], moved_entries);


            if (pkt->isRead()) {
                panic_if(readQueueSizes[curr_prio] < moved_entries,
                         "QoSMemCtrl::escalate master %s negative READ "
                         "packets for priority %d",
                        masters[m_id], tgt_prio);
                readQueueSizes[curr_prio] -= moved_entries;
                readQueueSizes[tgt_prio] += moved_entries;
            } else if (pkt->isWrite()) {
                panic_if(writeQueueSizes[curr_prio] < moved_entries,
                         "QoSMemCtrl::escalate master %s negative WRITE "
                         "packets for priority %d",
                        masters[m_id], tgt_prio);
                writeQueueSizes[curr_prio] -= moved_entries;
                writeQueueSizes[tgt_prio] += moved_entries;
            }

            // Change QoS priority and move packet
            pkt->qosValue(tgt_prio);
            queues[tgt_prio].push_back(pkt);

            // Erase element from source packet queue, this will
            // increment the iterator
            it = queues[curr_prio].erase(it);
            panic_if(packetPriorities[m_id][curr_prio] < moved_entries,
                     "QoSMemCtrl::escalate master %s negative packets "
                     "for priority %d",
                     masters[m_id], tgt_prio);

            packetPriorities[m_id][curr_prio] -= moved_entries;
            packetPriorities[m_id][tgt_prio] += moved_entries;
        } else {
            // Increment iterator to next location in the queue
            it++;
        }
    }
}

template<typename Queues>
void
MemCtrl::escalate(std::initializer_list<Queues*> queues,
                  uint64_t queue_entry_size,
                  MasterID m_id, uint8_t tgt_prio)
{
    // If needed, initialize all counters and statistics
    // for this master
    addMaster(m_id);

    DPRINTF(QOS,
            "QoSMemCtrl::escalate Master %s [id %d] to priority "
            "%d (currently %d packets)\n",masters[m_id], m_id, tgt_prio,
            packetPriorities[m_id][tgt_prio]);

    for (uint8_t curr_prio = 0; curr_prio < numPriorities(); ++curr_prio) {
        // Skip target priority
        if (curr_prio == tgt_prio)
            continue;

        // Process other priority packet
        while (packetPriorities[m_id][curr_prio] > 0) {
            DPRINTF(QOS,
                    "QoSMemCtrl::escalate MID %d checking priority %d "
                    "(packets %d)- current packets in prio %d:  %d\n"
                    "\t(source read %d source write %d target read %d, "
                    "target write %d)\n",
                    m_id, curr_prio, packetPriorities[m_id][curr_prio],
                    tgt_prio, packetPriorities[m_id][tgt_prio],
                    readQueueSizes[curr_prio],
                    writeQueueSizes[curr_prio], readQueueSizes[tgt_prio],
                    writeQueueSizes[tgt_prio]);

            // Check both read and write queue
            for (auto q : queues) {
                escalateQueues(*q, queue_entry_size, m_id,
                               curr_prio, tgt_prio);
            }
        }
    }

    DPRINTF(QOS,
            "QoSMemCtrl::escalate Completed master %s [id %d] to priority %d "
            "(now %d packets)\n\t(total read %d, total write %d)\n",
            masters[m_id], m_id, tgt_prio, packetPriorities[m_id][tgt_prio],
            readQueueSizes[tgt_prio], writeQueueSizes[tgt_prio]);
}

template<typename Queues>
uint8_t
MemCtrl::qosSchedule(std::initializer_list<Queues*> queues,
                     const uint64_t queue_entry_size,
                     const PacketPtr pkt)
{
    // Schedule packet.
    uint8_t pkt_priority = schedule(pkt);

    assert(pkt_priority < numPriorities());

    pkt->qosValue(pkt_priority);

    if (qosSyncroScheduler) {
        // Call the scheduling function on all other masters.
        for (const auto& m : masters) {

            if (m.first == pkt->masterId())
                continue;

            uint8_t prio = schedule(m.first, 0);

            if (qosPriorityEscalation) {
                DPRINTF(QOS,
                        "QoSMemCtrl::qosSchedule: (syncro) escalating "
                        "MASTER %s to assigned priority %d\n",
                        _system->getMasterName(m.first),
                        prio);
                escalate(queues, queue_entry_size, m.first, prio);
            }
        }
    }

    if (qosPriorityEscalation) {
        DPRINTF(QOS,
                "QoSMemCtrl::qosSchedule: escalating "
                "MASTER %s to assigned priority %d\n",
                _system->getMasterName(pkt->masterId()),
                pkt_priority);
        escalate(queues, queue_entry_size, pkt->masterId(), pkt_priority);
    }

    // Update last service tick for selected priority
    serviceTick[pkt_priority] = curTick();

    return pkt_priority;
}

} // namespace QoS

#endif /* __MEM_QOS_MEM_CTRL_HH__ */
