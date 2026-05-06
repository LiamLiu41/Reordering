// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_nic.h"
#include "sing_src.h"
#include "sing_sink.h"

using namespace std;
SingNICPort::SingNICPort(SingNIC& nic, linkspeed_bps linkspeed, EventList& eventlist)
    : EventSource(eventlist, "SingNICPort"), _nic(nic), _linkspeed(linkspeed) {
    _busy = false;
    _send_end_time = 0;
    _nodename = "SingNICPort_" + to_string(nic._src_id);
}

void SingNICPort::sendDataPacket(mem_b pkt_size) {
    assert(!_busy);  // Port must be idle
    assert(pkt_size > 0);
    
    // Mark port as busy
    _busy = true;
    
    // Calculate send completion time
    _send_end_time = eventlist().now() + 
                     (pkt_size * 8 * timeFromSec(1.0)) / _linkspeed;
    
    // Schedule send completion event
    eventlist().sourceIsPending(*this, _send_end_time);
    
    if (SingNICPort::_debug)
        cout << "Port " << this << " sending data packet, size " << pkt_size 
             << " will finish at " << timeAsUs(_send_end_time) << endl;
}

void SingNICPort::sendControlPacket(mem_b pkt_size) {
    assert(!_busy);  // Port must be idle
    assert(pkt_size > 0);
    
    // Mark port as busy
    _busy = true;
    
    // Calculate send completion time
    _send_end_time = eventlist().now() + 
                     (pkt_size * 8 * timeFromSec(1.0)) / _linkspeed;
    
    // Schedule send completion event
    eventlist().sourceIsPending(*this, _send_end_time);
    
    if (SingNICPort::_debug)
        cout << "Port " << this << " sending control packet, size " << pkt_size 
             << " will finish at " << timeAsUs(_send_end_time) << endl;
}

void SingNICPort::doNextEvent() {
    // Packet send complete
    assert(_busy);
    assert(_send_end_time == eventlist().now());
    
    // Clear busy state
    _busy = false;
    
    if (SingNICPort::_debug)
        cout << "Port " << this << " send complete at " 
             << timeAsUs(eventlist().now()) << " now free" << endl;
    
    // Notify NIC to schedule next packet
    _nic.scheduleNext();
}

////////////////////////////////////////////////////////////////
//  UEC NIC
////////////////////////////////////////////////////////////////

SingNIC::SingNIC(id_t src_num, EventList& eventList, linkspeed_bps linkspeed, uint32_t ports)
    : EventSource(eventList, "SingNIC"), NIC(src_num)  {
    _nodename = "SingNIC" + to_string(src_num);
    _linkspeed = linkspeed;
    
    // Create single port (ignore ports parameter as discussed)
    _port = make_unique<SingNICPort>(*this, linkspeed, eventList);
    
    _control_size = 0;
    
    // Initialize concurrent flow statistics
    _active_flow_count = 0;
    _max_concurrent_flows = 0;
    _max_concurrent_flows_time = 0;
    _last_change_time = 0;
    _weighted_flow_sum = 0.0;
}

void SingNIC::registerSubflowForScheduling(SingSubflow* sf, simtime_picosec next_send_time) {
    if (_scheduled_subflows.find(sf) != _scheduled_subflows.end()) {
        unregisterSubflow(sf);
    }
    
    _data_schedule_queue.insert({next_send_time, sf});
    _scheduled_subflows.insert(sf);
    
    if (!_port->isBusy()) {
        auto earliest = _data_schedule_queue.begin();
        if (earliest->second == sf) {
            if (earliest->first <= eventlist().now()) {
                scheduleNext();
            } else {
                eventlist().sourceIsPending(*this, earliest->first);
            }
        }
    }
}

void SingNIC::unregisterSubflow(SingSubflow* sf) {
    if (_scheduled_subflows.find(sf) == _scheduled_subflows.end()) {
        return;
    }
    
    for (auto it = _data_schedule_queue.begin(); it != _data_schedule_queue.end(); ) {
        if (it->second == sf) {
            it = _data_schedule_queue.erase(it);
        } else {
            ++it;
        }
    }
    
    _scheduled_subflows.erase(sf);
}

void SingNIC::rescheduleSubflow(SingSubflow* sf, simtime_picosec next_send_time) {
    registerSubflowForScheduling(sf, next_send_time);
}

void SingNIC::sendControlPacket(SingBasePacket* pkt, SingSrc* src, SingSink* sink) {
    assert((src || sink) && !(src && sink));
    
    // Add to control packet queue
    _control_size += pkt->size();
    CtrlPacket cp = {pkt, src, sink};
    _control.push_back(cp);

    if (SingNIC::_debug) {
        cout << "NIC " << this << " control packet queued, type " << pkt->str()
             << " control queue size " << _control.size() << endl;
    }

    // If port is idle, trigger scheduling immediately
    if (!_port->isBusy()) {
        scheduleNext();
    }
    // If port is busy, wait for send completion
}

void SingNIC::scheduleNext() {
    // This function is called when port is idle, decides what to send next
    
    assert(!_port->isBusy());  // Must be idle
    
    if (SingNIC::_debug)
        cout << "NIC " << this << " scheduleNext at " << timeAsUs(eventlist().now()) << endl;
    
    // ====== Step 1: Absolute priority for control packets ======
    if (!_control.empty()) {
        CtrlPacket cp = _control.front();
        _control.pop_front();
        SingBasePacket* p = cp.pkt;
        
        _control_size -= p->size();
        
        // Set route and send
        const Route* route;
        if (cp.src)
            route = cp.src->getPortRoute(0);
        else
            route = cp.sink->getPortRoute(0);
        p->set_route(*route);
        p->sendOn();
        
        // Notify port to send
        _port->sendControlPacket(p->size());
        
        if (SingNIC::_debug)
            cout << "NIC " << this << " scheduled control packet" << endl;
        
        return;
    }
    
    // ====== Step 2: Schedule data packets ======
    while (!_data_schedule_queue.empty()) {
        auto it = _data_schedule_queue.begin();
        simtime_picosec next_send_time = it->first;
        SingSubflow* sf = it->second;
        SingSrc* src = sf->parentSrc();
        
        // Step 2.1: Check if it's time to send
        if (next_send_time > eventlist().now()) {
            eventlist().sourceIsPending(*this, next_send_time);
            return;
        }
        
        // Step 2.2: Time has arrived, remove from queue
        _data_schedule_queue.erase(it);
        _scheduled_subflows.erase(sf);
        
        // Step 2.3: Check if flow is finished
        if (src->isTotallyFinished()) {
            continue;
        }
        
        // Step 2.4: Try to send
        // Retransmissions are subflow-local; new packets are src-level backlog.
        bool has_subflow_rtx = src->hasRtxForSubflow(*sf);
        bool can_send_new_data = src->hasNewDataBacklog() && src->canSendNewData();
        if (has_subflow_rtx || can_send_new_data) {
            mem_b sent_bytes = sf->sendPacketFromNIC();
            
            if (sent_bytes > 0) {
                _port->sendDataPacket(sent_bytes);
                
                mem_b next_pkt_size = src->getNextPacketSizeForSubflow(*sf);
                simtime_picosec next_time = sf->computeNextSendTime(next_pkt_size);
                registerSubflowForScheduling(sf, next_time);
                
                if (SingNIC::_debug)
                    cout << "NIC " << this << " scheduled data packet from src " 
                         << src->nodename() << " subflow " << sf->subflowId() << endl;
                
                return;
            }
        }
        
        // Step 2.5: Cannot send, re-register with new time
        mem_b next_pkt_size = src->getNextPacketSizeForSubflow(*sf);
        simtime_picosec next_time;
        
        if (next_pkt_size == 0) {
            next_time = eventlist().now() + src->getBaseRTT();
        } else {
            next_time = sf->computeNextSendTime(next_pkt_size);
        }
        
        registerSubflowForScheduling(sf, next_time);
    }
    
    // Queue empty, port stays idle
    if (SingNIC::_debug)
        cout << "NIC " << this << " no packets to schedule, port idle" << endl;
}


void SingNIC::doNextEvent() {
    // This function is called when scheduled time arrives (not send completion)
    
    if (SingNIC::_debug)
        cout << "NIC " << this << " doNextEvent (scheduled time) at " 
             << timeAsUs(eventlist().now()) << endl;
    
    // If port is busy, do nothing
    // Port will automatically call scheduleNext() when send completes
    if (_port->isBusy()) {
        if (SingNIC::_debug)
            cout << "NIC " << this << " port busy, will schedule after send complete" << endl;
        return;
    }
    
    // Port is idle, execute scheduling
    scheduleNext();
}

void SingNIC::flowStarted() {
    simtime_picosec now = eventlist().now();
    
    // Update weighted sum with the previous state
    if (_last_change_time > 0) {
        simtime_picosec duration = now - _last_change_time;
        _weighted_flow_sum += _active_flow_count * (double)duration;
    }
    
    _active_flow_count++;
    _last_change_time = now;
    
    if (_active_flow_count > _max_concurrent_flows) {
        _max_concurrent_flows = _active_flow_count;
        _max_concurrent_flows_time = now;
    }
}

void SingNIC::flowFinished() {
    assert(_active_flow_count > 0);
    
    simtime_picosec now = eventlist().now();
    
    // Update weighted sum with the previous state
    if (_last_change_time > 0) {
        simtime_picosec duration = now - _last_change_time;
        _weighted_flow_sum += _active_flow_count * (double)duration;
    }
    
    _active_flow_count--;
    _last_change_time = now;
}

uint32_t SingNIC::getMaxConcurrentFlows() const {
    return _max_concurrent_flows;
}

simtime_picosec SingNIC::getMaxConcurrentFlowsTime() const {
    return _max_concurrent_flows_time;
}

uint32_t SingNIC::getActiveFlowCount() const {
    return _active_flow_count;
}

double SingNIC::getAverageConcurrentFlows(simtime_picosec simulation_end_time) const {
    if (simulation_end_time == 0) {
        return 0.0;
    }
    
    // Calculate the weighted sum including the final state
    double total_weighted_sum = _weighted_flow_sum;
    
    // Add the contribution of the current state (from last change to simulation end)
    if (_last_change_time > 0 && simulation_end_time > _last_change_time) {
        simtime_picosec final_duration = simulation_end_time - _last_change_time;
        total_weighted_sum += _active_flow_count * (double)final_duration;
    }
    
    // Calculate time-weighted average
    double average = total_weighted_sum / (double)simulation_end_time;
    
    return average;
}



////////////////////////////////////////////////////////////////
