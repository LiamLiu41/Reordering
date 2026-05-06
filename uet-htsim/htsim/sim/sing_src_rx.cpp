// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_src.h"
#include "sing_sink.h"
#include <math.h>
#include <cstdint>
#include "sing_logger.h"

using namespace std;

void SingSrc::receivePacket(Packet& pkt, uint32_t portnum) {
    switch (pkt.type()) {
        case SINGDATA: {
            _stats.bounces_received++;
            // TBD - this is likely a Back-to-sender packet
            cout << "SingSrc::receivePacket receive SINGDATA packets\n";

            abort();
        }
        case SINGACK: {
            processAck((const SingAck&)pkt);
            pkt.free();
            return;
        }
        case SINGNACK: {
            processNack((const SingNack&)pkt);
            pkt.free();
            return;
        }
        case SINGCNP: {
            processCnp((const SingCnp&)pkt);
            pkt.free();
            return;
        }
        default: {
            cout << "SingSrc::receivePacket receive default\n";

            abort();
        }
    }
}

void SingSrc::eraseSendRecord(SingDataPacket::seq_t seqno) {
    auto it = _tx_bitmap.find(seqno);
    if (it == _tx_bitmap.end()) {
        return;
    }
    _ssn_to_dsn.erase({it->second.owner_subflow_id, it->second.ssn});
    _tx_bitmap.erase(it);
    auto rtx_it = _rtx_times.find(seqno);
    if (rtx_it != _rtx_times.end()) {
        _rtx_times.erase(rtx_it);
    }
}

bool SingSrc::markDsnForRtx(SingBasePacket::seq_t seqno) {
    auto it = _tx_bitmap.find(seqno);
    if (it == _tx_bitmap.end()) {
        return false;
    }

    sendRecord& tx = it->second;
    if (tx.state == TxState::PENDING_RTX) {
        return true;
    }

    assert(tx.owner_subflow_id < _subflows.size());

    _in_flight = (_in_flight >= tx.pkt_size) ? _in_flight - tx.pkt_size : 0;
    delFromSendTimes(tx.send_time, seqno);
    if (tx.send_time == _rto_send_time) {
        recalculateRTO();
    }

    tx.state = TxState::PENDING_RTX;
    _subflows[tx.owner_subflow_id]->enqueueRtx(seqno);
    _pending_rtx_pkts++;
    _nic.rescheduleSubflow(_subflows[tx.owner_subflow_id].get(), eventlist().now());
    return true;
}


mem_b SingSrc::handleAckno(SingDataPacket::seq_t ackno) {
    auto i = _tx_bitmap.find(ackno);
    if (i == _tx_bitmap.end()) {
        return 0;
    }

    // If ackno is in tx_bitmap, it means we have recently sent out a packet
    // (new or retransmitted). The ACK confirms delivery, so remove it.
    sendRecord tx = i->second;
    mem_b pkt_size = tx.pkt_size;

    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " handleAck " << ackno << " flow " << _flow.str() << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " handleAck ackno " << ackno
             << endl;
    }

    if (_msg_tracker.has_value()) {
        _msg_tracker.value()->addSAck(ackno);
    }

    if (tx.state == TxState::IN_FLIGHT) {
        delFromSendTimes(tx.send_time, ackno);
        if (tx.send_time == _rto_send_time) {
            recalculateRTO();
        }
    } else {
        assert(tx.state == TxState::PENDING_RTX);
        assert(tx.owner_subflow_id < _subflows.size());
        bool removed = _subflows[tx.owner_subflow_id]->eraseRtxDsn(ackno);
        assert(removed);
        assert(_pending_rtx_pkts > 0);
        _pending_rtx_pkts--;
    }
    eraseSendRecord(ackno);

    return pkt_size;
}

mem_b SingSrc::handleCumulativeAck(SingDataPacket::seq_t next_expected_dsn) {
    mem_b newly_acked = 0;

    if (_msg_tracker.has_value()) {
        _msg_tracker.value()->addCumAck(next_expected_dsn);
    }

    auto it = _tx_bitmap.begin();
    while (it != _tx_bitmap.end()) {
        auto seqno = it->first;
        // next_expected_dsn is exclusive upper bound; packets below it are acked.
        if (seqno >= next_expected_dsn) {
            // nothing else acked
            break;
        }

        sendRecord tx = it->second;
        mem_b pkt_size = tx.pkt_size;

        if (_debug_src)
            cout << _flow.str() << " " << _nodename << " handleCumAck " << seqno << " flow " << _flow.str() << endl;
        if (_flow.flow_id() == _debug_flowid) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " handleCumulativeAck seqno " << seqno << endl;
        }

        auto erase_it = it;
        ++it;
        newly_acked += pkt_size;

        if (tx.state == TxState::IN_FLIGHT) {
            delFromSendTimes(tx.send_time, seqno);
            if (tx.send_time == _rto_send_time) {
                recalculateRTO();
            }
        } else {
            assert(tx.state == TxState::PENDING_RTX);
            assert(tx.owner_subflow_id < _subflows.size());
            bool removed = _subflows[tx.owner_subflow_id]->eraseRtxDsn(seqno);
            assert(removed);
            assert(_pending_rtx_pkts > 0);
            _pending_rtx_pkts--;
        }
        _tx_bitmap.erase(erase_it);
        _ssn_to_dsn.erase({tx.owner_subflow_id, tx.ssn});
        auto rtx_time = _rtx_times.find(seqno);
        if (rtx_time != _rtx_times.end()) {
            _rtx_times.erase(rtx_time);
        }
    }
    return newly_acked;
}

void SingSrc::emitFlowCompletionLog(SingDataPacket::seq_t cum_ack, uint32_t total_messages) {
    simtime_picosec start_time = _flow_start_time;
    simtime_picosec finish_time = eventlist().now();
    simtime_picosec duration = finish_time - start_time;

    double slowdown = 1.0;
    if (_ideal_time > 0) {
        slowdown = (double)duration / (double)_ideal_time;
    }

    cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename
         << " type " << (_flow_type.empty() ? "default" : _flow_type)
         << " src " << static_cast<const NIC&>(_nic).get_id()
         << " dst " << _dstaddr
         << " started at " << timeAsUs(start_time)
         << " finished at " << timeAsUs(finish_time)
         << " duration " << timeAsUs(duration) << " us"
         << " ideal_time " << timeAsUs(_ideal_time) << " us"
         << " slowdown " << slowdown
         << " total messages " << total_messages
         << " total packets " << cum_ack
         << " flowsize " << _flow_size
         << " total bytes " << (mem_b)cum_ack * _mss
         << " in_flight now " << _in_flight
         << " timeouts " << _stats.rto_events
         << " nacks " << _stats.nacks_received
         << " last_hop_nacks " << _stats.last_hop_nacks_received
         << " fair_inc " << _nscc_overall_stats.inc_fair_bytes
         << " prop_inc " << _nscc_overall_stats.inc_prop_bytes
         << " fast_inc " << _nscc_overall_stats.inc_fast_bytes
         << " eta_inc " << _nscc_overall_stats.inc_eta_bytes
         << " multi_dec -" << _nscc_overall_stats.dec_multi_bytes
         << " quick_dec -" << _nscc_overall_stats.dec_quick_bytes
         << " nack_dec -" << _nscc_overall_stats.dec_nack_bytes
         << endl;
}

bool SingSrc::checkFinished(SingDataPacket::seq_t cum_ack) {

    if (_done_sending) {
        // assert(_backlog == 0);
        // assert(_rtx_queue.empty());
        // if (_pdc.has_value()) {
        //     assert(_pdc->checkFinished());
        // }
    } else {
        if (_msg_tracker.has_value()) {
            if (_msg_tracker.value()->checkFinished()) {
                emitFlowCompletionLog(cum_ack, _msg_tracker.value()->getMsgCompleted());
                cancelRTO();
                _done_sending = true;
                // Notify NIC that a flow has finished
                _nic.flowFinished();
            }
        } else {
            if (((int64_t)cum_ack * _mss) >= (int64_t)_flow_size) {
                emitFlowCompletionLog(cum_ack, 1);
                if (_end_trigger) {
                    _end_trigger->activate();
                }
                if (_flow_logger) {
                    _flow_logger->logEvent(_flow, *this, FlowEventLogger::FINISH, _flow_size, cum_ack);
                }
                cancelRTO();
                _done_sending = true;
                // Notify NIC that a flow has finished
                _nic.flowFinished();
            }
        }
    }

    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " checkFinished "
             << " cum_acc " << cum_ack << " mss " << _mss
             << " total bytes " << (int64_t)cum_ack * _mss
             << " flow_size " << _flow_size 
             << " backlog " << _backlog
             << " pending_rtx " << _pending_rtx_pkts
             << " done_sending " << _done_sending << endl;

    return _done_sending;
}


uint64_t SingSrc::updateAckRecvdState(const SingAck& pkt) {
    uint64_t newly_recvd_bytes = 0;
    if (pkt.recvd_bytes() > _recvd_bytes) {
        newly_recvd_bytes = pkt.recvd_bytes() - _recvd_bytes;
        _recvd_bytes = pkt.recvd_bytes();
    }

    if (_debug_src) {
        cout << "processAck next_expected_dsn " << pkt.next_expected_dsn()
             << " next_expected_ssn " << pkt.next_expected_ssn()
             << " trigger_dsn " << pkt.trigger_dsn()
             << " recvd_bytes " << _recvd_bytes
             << " newly_recvd_bytes " << newly_recvd_bytes << endl;
    }

    _stats.acks_received++;
    // _in_flight may be corrected later by cumulative and selective ACK handling.
    _in_flight -= newly_recvd_bytes;
    return newly_recvd_bytes;
}

simtime_picosec SingSrc::computeAckRawRttSample(const SingAck& pkt) {
    simtime_picosec send_time = pkt.echo_send_ts();
    assert(send_time <= eventlist().now());
    return eventlist().now() - send_time;
}

void SingSrc::applyAckAccounting(const SingAck& pkt) {
    auto next_expected_dsn = pkt.next_expected_dsn();
    handleCumulativeAck(next_expected_dsn);

    if (_debug_src) {
        cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str()
             << " " << _nodename << " processAck next_expected_dsn: "
             << next_expected_dsn << " flow " << _flow.str() << endl;
    }

    auto sack_base_dsn = pkt.sack_base_dsn();
    uint64_t sack_bitmap = pkt.bitmap();

    if (_debug_src) {
        cout << "    sack_base_dsn: " << sack_base_dsn << " bitmap: " << sack_bitmap << endl;
    }

    while (sack_bitmap > 0) {
        if (sack_bitmap & 1) {
            if (_debug_src) {
                cout << "    Sack " << sack_base_dsn << " flow " << _flow.str() << endl;
            }

            handleAckno(sack_base_dsn);
            if (_highest_recv_seqno < sack_base_dsn) {
                _highest_recv_seqno = sack_base_dsn;
            }
        }
        sack_base_dsn++;
        sack_bitmap >>= 1;
    }
}

void SingSrc::processAckPathFeedback(const SingAck& pkt) {
    if (_ack_path_feedback_mode == AckPathFeedbackMode::REPS_ACKAGG) {
        if (pkt.ecn_echo()) {
            uint16_t ecn_ev = pkt.agg_ecn_ev_valid() ? pkt.agg_ecn_ev() : pkt.ev();
            _mp->processEv(ecn_ev, SingMultipath::PATH_ECN);
        }
        for (uint16_t ev : pkt.agg_non_ecn_evs()) {
            _mp->processEv(ev, SingMultipath::PATH_GOOD);
        }
        return;
    }

    if (_ack_path_feedback_mode == AckPathFeedbackMode::BITMAP_ACKAGG) {
        for (uint16_t ev : pkt.agg_ecn_evs()) {
            _mp->processEv(ev, SingMultipath::PATH_ECN);
        }
        return;
    }

    // For UecMpReps: only process path feedback on path_sel_end ACKs.
    // For other multipath strategies: process every ACK.
    bool should_process_ev = true;
    if (_mp->getType() == SingMultipath::REPS) {
        should_process_ev = pkt.path_sel_end();
        if (_flow.flow_id() == _debug_flowid && !should_process_ev) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " REPS: skipping processEv for non-path_sel_end ACK" << endl;
        }
    }

    if (should_process_ev) {
        _mp->processEv(pkt.ev(), pkt.ecn_echo() ? SingMultipath::PATH_ECN : SingMultipath::PATH_GOOD);
        if (_flow.flow_id() == _debug_flowid && pkt.path_sel_end()) {
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id()
                 << " processEv called for path_sel_end ACK, path_id=" << pkt.ev() << endl;
        }
    }
}

void SingSrc::dispatchAckToSubflow(const SingAck& pkt, simtime_picosec raw_rtt,
                                   uint64_t newly_recvd_bytes) {
    assert(_sender_based_cc);
    assert(_default_subflow != nullptr);
    assert(pkt.subflow_id() < _subflows.size());
    SingSubflow* sf = _subflows[pkt.subflow_id()].get();
    sf->processAck(raw_rtt, pkt.next_expected_ssn(), pkt.ecn_echo(),
                   newly_recvd_bytes);
}

void SingSrc::processAck(const SingAck& pkt) {
    _nic.logReceivedCtrl(pkt.size());

    auto next_expected_dsn = pkt.next_expected_dsn();
    const uint64_t newly_recvd_bytes = updateAckRecvdState(pkt);
    simtime_picosec raw_rtt = computeAckRawRttSample(pkt);
    applyAckAccounting(pkt);
    processAckPathFeedback(pkt);

    // if (_flow.flow_id() == _debug_flowid) {
    //     cout <<  timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " track_avg_rtt " << timeAsUs(currentNsccAvgDelay())
    //         << " rtt " << timeAsUs(raw_rtt) << " skip " << pkt.ecn_echo()  << " ev " << pkt.ev()
    //         << " next_expected_dsn " << next_expected_dsn
    //         << " bitmap_base " << pkt.sack_base_dsn()
    //         << " ooo " << pkt.ooo()
    //         << " cwnd " << (currentWindowBytes() / get_avg_pktsize())
    //         << " trigger_dsn " << pkt.trigger_dsn()
    //         << endl;
    // }

    dispatchAckToSubflow(pkt, raw_rtt, newly_recvd_bytes);

    // if (_debug_src) {
    //     cout << "At " << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
    //          << " processAck next_expected_dsn: " << next_expected_dsn
    //          << " next_expected_ssn: " << pkt.next_expected_ssn()
    //          << " flow " << _flow.str() << " cwnd " << currentWindowBytes()
    //          << " flightsize " << _in_flight
    //          << " newlyrecvd " << newly_recvd_bytes << " skip " << pkt.ecn_echo()
    //          << " raw rtt " << raw_rtt << endl;
    // }

    if (checkFinished(next_expected_dsn)) {
        return;
    }

    // In new architecture, NIC scheduler handles sending automatically
    // No need to call sendIfPermitted()
}


uint16_t SingSrc::get_avg_pktsize(){
    return _mss;  // does not include header
}


void SingSrc::processNack(const SingNack& pkt) {
    _nic.logReceivedCtrl(pkt.size());
    _stats.nacks_received++;
    
    // Track last hop NACKs (caused by last hop trimming)
    if (pkt.last_hop()) {
        _stats.last_hop_nacks_received++;
    }

    auto nacked_seqno = pkt.ref_dsn();
    if (_debug_src) {
        cout << _flow.str() << " " << _nodename << " processNack nacked: " << nacked_seqno << " flow " << _flow.str()
             << endl;
    }

    uint16_t ev = pkt.ev();
    // what should we do when we get a NACK with ECN_ECHO set?  Presumably ECE is superfluous?
    // bool ecn_echo = pkt.ecn_echo();

    auto i = _tx_bitmap.find(nacked_seqno);
    if (i == _tx_bitmap.end()) {
        if (_debug_src)
            cout << _flow.str() << " " << "Didn't find NACKed packet in _active_packets flow " << _flow.str() << endl;

        // this abort is here because this is unlikely to happen in
        // simulation - when it does, it is usually due to a bug
        // elsewhere.  But if you discover a case where this happens
        // for real, remove the abort and uncomment the return below.
        //abort();
        // this can happen when the NACK arrives later than a cumulative ACK covering the NACKed
        // packet. 
        return;
    }
    if (i->second.state == TxState::PENDING_RTX) {
        return;
    }

    mem_b pkt_size = i->second.pkt_size;
    uint16_t owner_subflow_id = i->second.owner_subflow_id;

    assert(pkt_size >= _hdr_size);
    auto seqno = i->first;

    if(_flow.flow_id() == _debug_flowid){
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " ev " << ev 
            << " seqno " << seqno
            << " trimming " << endl;
    }
    bool marked = markDsnForRtx(seqno);
    assert(marked);

    if (pkt.last_hop())
        _mp->processEv(ev, pkt.ecn_echo() ? SingMultipath::PATH_ECN : SingMultipath::PATH_GOOD);
    else
        _mp->processEv(ev, SingMultipath::PATH_NACK);

    // Dispatch to subflow for CC update.
    assert(_sender_based_cc);
    assert(_default_subflow != nullptr);
    assert(owner_subflow_id < _subflows.size());
    if (pkt.subflow_id() != owner_subflow_id && _debug_src) {
        cout << "WARN: NACK subflow mismatch dsn " << seqno
             << " nack_sf " << pkt.subflow_id()
             << " owner_sf " << owner_subflow_id << endl;
    }
    SingSubflow* sf = _subflows[owner_subflow_id].get();
    sf->processNack(pkt_size, pkt.last_hop());
}

void SingSrc::processCnp(const SingCnp& pkt) {
    _nic.logReceivedCtrl(pkt.size());
    _stats.cnps_received++;
    uint16_t sf_id = pkt.subflow_id();
    if (sf_id >= _subflows.size()) {
        _stats.cnps_invalid_subflow++;
        if (_debug_src) {
            cout << _flow.str() << " " << _nodename
                 << " dropping CNP with invalid subflow_id " << sf_id
                 << " subflow_count " << _subflows.size() << endl;
        }
        return;
    }

    _subflows[sf_id]->processCnp();
}
