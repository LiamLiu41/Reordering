// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_NIC_H
#define SING_NIC_H

#include <memory>
#include <list>
#include <set>

#include "eventlist.h"
#include "singpacket.h"

class SingSrc;
class SingSink;
class SingSubflow;
class SingNIC;
// SingNICPort: Responsible for physical packet transmission
// Manages busy state and triggers NIC scheduling after send completion
class SingNICPort : public EventSource {
public:
    SingNICPort(SingNIC& nic, linkspeed_bps linkspeed, EventList& eventlist);
    
    // Query port status
    bool isBusy() const { return _busy; }
    simtime_picosec sendEndTime() const { return _send_end_time; }
    
    // Send data or control packet (sets busy state)
    void sendDataPacket(mem_b pkt_size);
    void sendControlPacket(mem_b pkt_size);
    
    // EventSource interface: called when packet send completes
    virtual void doNextEvent() override;
    
    virtual const string& nodename() const { return _nodename; }
    
    static bool _debug;
    
private:
    SingNIC& _nic;
    linkspeed_bps _linkspeed;
    bool _busy;
    simtime_picosec _send_end_time;
    string _nodename;
};

// SingNIC: Responsible for scheduling policy
// Decides what packet to send next based on rate-based scheduling
class SingNIC : public EventSource, public NIC {
    friend class SingNICPort;
    
    struct CtrlPacket {
        SingBasePacket* pkt;
        SingSrc* src;
        SingSink* sink;
    };
    
public:
    SingNIC(id_t src_num, EventList& eventList, linkspeed_bps linkspeed, uint32_t ports);

    // Scheduling interfaces (subflow-based)
    void registerSubflowForScheduling(SingSubflow* sf, simtime_picosec next_send_time);
    void unregisterSubflow(SingSubflow* sf);
    void rescheduleSubflow(SingSubflow* sf, simtime_picosec next_send_time);
    
    // Control packet interface (absolute priority)
    void sendControlPacket(SingBasePacket* pkt, SingSrc* src, SingSink* sink);
    
    // EventSource interface: called when scheduled time arrives
    virtual void doNextEvent() override;
    
    // Called by Port when send completes
    void scheduleNext();

    // Flow concurrency tracking
    void flowStarted();
    void flowFinished();
    uint32_t getMaxConcurrentFlows() const;
    simtime_picosec getMaxConcurrentFlowsTime() const;
    uint32_t getActiveFlowCount() const;
    double getAverageConcurrentFlows(simtime_picosec simulation_end_time) const;

    linkspeed_bps linkspeed() const {return _linkspeed;}

    int activeSources() const { return _data_schedule_queue.size(); }
    virtual const string& nodename() const {return _nodename;}
    
    static bool _debug;

private:
    // Port
    unique_ptr<SingNICPort> _port;
    
    // Data scheduling queue: (next_send_time, SingSubflow*)
    std::multimap<simtime_picosec, SingSubflow*> _data_schedule_queue;
    std::set<SingSubflow*> _scheduled_subflows;  // Fast lookup
    
    // Control packet queue (absolute priority)
    list<struct CtrlPacket> _control;
    mem_b _control_size;

    linkspeed_bps _linkspeed;

    // Concurrent flow statistics
    uint32_t _active_flow_count;
    uint32_t _max_concurrent_flows;
    simtime_picosec _max_concurrent_flows_time;
    
    // For calculating time-weighted average concurrent flows
    simtime_picosec _last_change_time;
    double _weighted_flow_sum;  // sum of (flow_count * duration)

    string _nodename;
};

#endif  // SING_NIC_H
