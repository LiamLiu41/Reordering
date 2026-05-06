// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_src.h"
#include "sing_sink.h"
#include <math.h>
#include <cstdint>
#include "sing_logger.h"

using namespace std;

void SingSrc::delFromSendTimes(simtime_picosec time, SingDataPacket::seq_t seq_no) {
    //cout << eventlist().now() << " flowid " << _flow.flow_id() << " _send_times.erase " << time << " for " << seq_no << endl;
    auto snd_seq_range = _send_times.equal_range(time);
    auto snd_it = snd_seq_range.first;
    while (snd_it != snd_seq_range.second) {
        if (snd_it->second == seq_no) {
            _send_times.erase(snd_it);
            break;
        } else {
            ++snd_it;
        }
    }
}

void SingSrc::startRTO(simtime_picosec send_time) {
    if (!_rtx_timeout_pending) {
        // timer is not running - start it
        _rtx_timeout_pending = true;
        _rtx_timeout = send_time + _min_rto;
        _rto_send_time = send_time;

        if (_rtx_timeout < eventlist().now())
            _rtx_timeout = eventlist().now();

        if (_debug_src)
            cout << "Start timer at " << timeAsUs(eventlist().now()) << " source " << _flow.str()
                 << " expires at " << timeAsUs(_rtx_timeout) << " flow " << _flow.str() << endl;

        _rto_timer_handle = eventlist().sourceIsPendingGetHandle(*this, _rtx_timeout);
        if (_rto_timer_handle == eventlist().nullHandle()) {
            // this happens when _rtx_timeout is past the configured simulation end time.
            _rtx_timeout_pending = false;
            if (_debug_src)
                cout << "Cancel timer because too late for flow " << _flow.str() << endl;
        }
    } else {
        // timer is already running
        if (send_time + _min_rto < _rtx_timeout) {
            // RTO needs to expire earlier than it is currently set
            cancelRTO();
            startRTO(send_time);
        }
    }
}

void SingSrc::clearRTO() {
    // clear the state
    _rto_timer_handle = eventlist().nullHandle();
    _rtx_timeout_pending = false;

    if (_debug_src)
        cout << "Clear RTO " << timeAsUs(eventlist().now()) << " would have expired at " << _rtx_timeout << " source " << _flow.str() << endl;
}

void SingSrc::cancelRTO() {
    if (_rtx_timeout_pending) {
        // cancel the timer
        eventlist().cancelPendingSourceByHandle(*this, _rto_timer_handle);
        clearRTO();
    }
}


void SingSrc::recalculateRTO() {
    // we're no longer waiting for the packet we set the timer for -
    // figure out what the timer should be now.
    cancelRTO();
    if (_send_times.empty()) {
        // nothing left that we're waiting for
        return;
    }
    auto earliest_send_time = _send_times.begin()->first;
    startRTO(earliest_send_time);
}

void SingSrc::rtxTimerExpired() {
    assert(eventlist().now() == _rtx_timeout);
    clearRTO();

    auto first_entry = _send_times.begin();
    assert(first_entry != _send_times.end());
    auto seqno = first_entry->second;

    auto send_record = _tx_bitmap.find(seqno);
    assert(send_record != _tx_bitmap.end());
    assert(send_record->second.state == TxState::IN_FLIGHT);
    mem_b pkt_size = send_record->second.pkt_size;
    uint16_t owner_subflow_id = send_record->second.owner_subflow_id;

    // Increment timeout counter
    _stats.rto_events++;

    // Trigger multipathing feedback for timeout. Unless we save EVs on the sender per packet, we will 
    // not be able to recover the original timed-out ev.
    _mp->processEv(SingMultipath::UNKNOWN_EV, SingMultipath::PATH_TIMEOUT);

    // update flightsize?

    //_send_times.erase(first_entry);
    delFromSendTimes(send_record->second.send_time,seqno);

    if (_debug_src)
        cout << _nodename << " rtx timer expired for seqno " << seqno << " flow " << _flow.str() << " packet sent at " << timeAsUs(send_record->second.send_time) << " now time is " << timeAsUs(eventlist().now()) << endl;
    
    if (_flow.flow_id() == SingSrc::_debug_flowid ) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() 
            <<" rtx timer expired for seqno " << seqno << " packet sent at " 
            << timeAsUs(send_record->second.send_time) << " now time is " << timeAsUs(eventlist().now()) 
            << " _loss_recovery_mode " << _loss_recovery_mode
            << endl;
    }

    //Yanfang: this is a hack, we remove timestamp for these seqno, 
    //I would expect that that the fast loss recovery will retransmit this packet, when the send_times record the sending timestamp for this packet
    if (_sender_based_cc && _enable_sleek) {
        if (_loss_recovery_mode) {
            if (_rtx_times[seqno] < 1) {
                recalculateRTO();
            } else {
                _highest_rtx_sent = seqno;
            }
            return; 
        }
    }

    if (_sender_based_cc) {
        auto* c = cc();
        assert(c != nullptr);
        c->onTimeout(_in_flight);
    }

    assert(owner_subflow_id < _subflows.size());
    _subflows[owner_subflow_id]->reduceSsnInFlight(pkt_size);
    bool marked = markDsnForRtx(seqno);
    assert(marked);
}
