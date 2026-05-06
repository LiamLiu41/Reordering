// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_sink.h"
#include "sing_src.h"

using namespace std;
SingSinkPort::SingSinkPort(SingSink& sink, uint32_t port_num)
    : _sink(sink), _port_num(port_num) {
}

void SingSinkPort::setRoute(const Route& route) {
    _route = &route;
}

void SingSinkPort::receivePacket(Packet& pkt) {
    _sink.receivePacket(pkt, _port_num);
}

const string& SingSinkPort::nodename() {
    return _sink.nodename();
}

////////////////////////////////////////////////////////////////
//  UEC SINK
////////////////////////////////////////////////////////////////

SingSink::SingSink(TrafficLogger* trafficLogger, SingNIC& nic, uint32_t no_of_ports)
    : DataReceiver("uecSink"),
      _nic(nic),
      _flow(trafficLogger),
      _expected_dsn(0),
      _high_dsn(0),
      _received_bytes(0),
      _bytes_since_last_ack(0),
      _recvd_bytes(0),
      _rcv_cwnd_pen(255),
      _end_trigger(NULL),
      _dsn_rx_bitmap(0),
      _out_of_order_count(0),
      _ack_request(false),
      _entropy(0) {
    
    _no_of_ports = no_of_ports;
    _ports.resize(no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p] = new SingSinkPort(*this, p);
    }
    _stats = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    _receiver_done = false;
    _legacy_ack_policy = std::make_unique<LegacyAckPolicy>(*this);
    _reps_ackagg_policy = std::make_unique<RepsAckAggPolicy>(*this);
    _bitmap_ackagg_policy = std::make_unique<BitmapAckAggPolicy>(*this);
    setAckFeedbackMode(_ack_feedback_mode);
    _cnp_min_interval = timeFromUs((uint32_t)2);
}

void SingSink::connectPort(uint32_t port_num, SingSrc& src, const Route& route) {
    _src = &src;
    _ports[port_num]->setRoute(route);

    if (port_num == 0) {
        // Match subflow sink count to src's subflow count (set by initCcForFlow/createPathwiseSubflows)
        int num = src.numSubflows();
        createSubflowSinks(num > 0 ? num : 1);
    }
}

void SingSink::createSubflowSinks(int num_subflows) {
    _subflow_sinks.clear();
    _last_cnp_send_ts_by_subflow.clear();
    _has_sent_cnp_by_subflow.clear();
    for (int i = 0; i < num_subflows; i++) {
        _subflow_sinks.push_back(std::make_unique<SingSubflowSink>(*this, i));
        _last_cnp_send_ts_by_subflow.push_back(0);
        _has_sent_cnp_by_subflow.push_back(0);
    }
}

SingSubflowSink* SingSink::getSubflowSink(int subflow_id) {
    if (subflow_id >= 0 && subflow_id < (int)_subflow_sinks.size()) {
        return _subflow_sinks[subflow_id].get();
    }
    return nullptr;
}

void SingSink::setAckFeedbackMode(AckFeedbackMode mode) {
    _ack_feedback_mode = mode;
    switch (_ack_feedback_mode) {
    case AckFeedbackMode::LEGACY:
        _ack_policy = _legacy_ack_policy.get();
        break;
    case AckFeedbackMode::REPS_ACKAGG:
        _ack_policy = _reps_ackagg_policy.get();
        break;
    case AckFeedbackMode::BITMAP_ACKAGG:
        _ack_policy = _bitmap_ackagg_policy.get();
        break;
    default:
        abort();
    }
    assert(_ack_policy != nullptr);
}

bool SingSink::isValidSubflowId(int sf_id) const {
    return sf_id >= 0 && sf_id < (int)_subflow_sinks.size();
}

SingSubflowSink* SingSink::subflowSinkOrNull(int sf_id) {
    assert(isValidSubflowId(sf_id));
    return _subflow_sinks[sf_id].get();
}

void SingSink::logRxBegin(const SingDataPacket& pkt) const {
    if (_src->debug()) {
        cout << " SingSink " << _nodename << " src " << _src->nodename()
             << " processData dsn: " << pkt.dsn() << " ssn: " << pkt.ssn()
             << " sf: " << pkt.subflow_id()
             << " time " << timeAsNs(_src->eventlist().now())
             << " when expected dsn is " << _expected_dsn << " size " << pkt.size()
             << " ooo count " << _out_of_order_count
             << " flow " << _src->flow()->str() << endl;
    }
}

void SingSink::logDuplicate(const SingDataPacket& pkt) const {
    bool debug_flow = _src->flow()->flow_id() == SingSrc::_debug_flowid;
    if (SingSrc::_debug || debug_flow) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " duplicate dsn " << pkt.dsn() << " expected " << _expected_dsn << endl;
    }
}

void SingSink::logAckDecision(const SingDataPacket& pkt, bool ecn, bool threshold_reached,
                              bool force_ack, const SingAck* ack_packet) const {
    bool debug_flow = _src->flow()->flow_id() == SingSrc::_debug_flowid;
    if (debug_flow) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " checkSack dsn: " << pkt.dsn() << " ooo_count "
             << _out_of_order_count << " ecn " << ecn
             << " thresholdReached " << threshold_reached << " forceack " << force_ack << endl;
    }

    if (ack_packet == nullptr) {
        return;
    }

    if (_src->debug()) {
        cout << " SingSink " << _nodename << " src " << _src->nodename()
             << " sendAckNow dsn: " << _expected_dsn << " acked_dsn " << pkt.dsn()
             << " ooo_count " << _out_of_order_count
             << " recvd_bytes " << _recvd_bytes << " flow " << _src->flow()->str()
             << " ecn " << ecn << " thresholdReached " << threshold_reached
             << " forceack " << force_ack << endl;
    }

    if (debug_flow) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " sendAckNow dsn: " << _expected_dsn << " acked_dsn " << pkt.dsn()
             << " next_expected_ssn " << ack_packet->next_expected_ssn()
             << " ooo_count " << _out_of_order_count
             << " recvd_bytes " << _recvd_bytes << " flow " << _src->flow()->str()
             << " ecn " << ecn << " thresholdReached " << threshold_reached
             << " forceack " << force_ack
             << " ack_path_sel_end " << (ack_packet->path_sel_end() ? "YES" : "NO")
             << endl;
    }
}

void SingSink::updateEcnStats(bool ecn, mem_b pkt_size) {
    if (ecn) {
        _stats.ecn_received++;
        _stats.ecn_bytes_received += pkt_size;
    }
}

void SingSink::accountNewDataPacket(const SingDataPacket& pkt, int sf_id) {
    if (_ack_trigger_mode == AckTriggerMode::GLOBAL) {
        _bytes_since_last_ack += pkt.size();
    }

    if (SingSubflowSink* subflow_sink = subflowSinkOrNull(sf_id)) {
        subflow_sink->processData(pkt.ssn(), pkt.size());
    }

    if (_src->msg_tracker().has_value()) {
        _src->msg_tracker().value()->addRecvd(pkt.dsn());
    }

    if (_src->flow()->flow_id() == SingSrc::_debug_flowid) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " recv dsn " << pkt.dsn() << " ssn " << pkt.ssn()
             << " sf " << pkt.subflow_id() << endl;
    }

    if (pkt.dsn() > _high_dsn) {
        _high_dsn = pkt.dsn();
    }

    // Count received payload bytes for completion tracking.
    _received_bytes += pkt.size() - SingAck::ACKSIZE;
    _nic.logReceivedData(pkt.size(), pkt.size());

    _recvd_bytes += pkt.size();
    if (_src->debug()) {
        cout << _nodename << " recvd_bytes: " << _recvd_bytes << endl;
    }

    if (_src->debug() && _received_bytes >= _src->flowsize()) {
        cout << _nodename << " received " << _received_bytes << " at "
             << timeAsUs(EventList::getTheEventList().now()) << endl;
    }
    assert(_received_bytes <= _src->flowsize());

    if (!_receiver_done && _received_bytes >= _src->flowsize()) {
        _receiver_done = true;

        if (SingSrc::_print_receiver_fct) {
            simtime_picosec start_time = _src->flowStartTime();
            simtime_picosec finish_time = _src->eventlist().now();
            simtime_picosec duration = finish_time - start_time;

            double slowdown = 1.0;
            if (_src->oneWayIdealTime() > 0) {
                slowdown = (double)duration / (double)_src->oneWayIdealTime();
            }

            cout << "receiver_fct: Flow " << _src->_name << " flowId " << _src->flowId()
                 << " " << _nodename
                 << " type " << (_src->flowType().empty() ? "default" : _src->flowType())
                 << " started at " << timeAsUs(start_time)
                 << " finished at " << timeAsUs(finish_time)
                 << " duration " << timeAsUs(duration) << " us"
                 << " one_way_ideal_time " << timeAsUs(_src->oneWayIdealTime()) << " us"
                 << " slowdown " << slowdown
                 << " flowsize " << _src->flowsize()
                 << " received_bytes " << _received_bytes
                 << endl;
        }
    }
}

void SingSink::maybeMarkForceAckByFlags(const SingDataPacket& pkt, bool& force_ack,
                                        bool include_path_sel_end_force) {
    if (pkt.ar()) {
        // AR requests immediate ACK and one more ACK when OOO queue drains.
        force_ack = true;
        _ack_request = true;
    }

    if (include_path_sel_end_force && pkt.path_sel_end()) {
        force_ack = true;
        if (_src->flow()->flow_id() == SingSrc::_debug_flowid) {
            cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
                 << " recv path_sel_end pkt dsn " << pkt.dsn() << " forcing ACK" << endl;
        }
    }
}

void SingSink::noteAggFeedbackFromNewPacket(const SingDataPacket& pkt, bool ecn) {
    if (ecn) {
        _pending_ecn_since_last_ack = true;
        if (!_pending_first_ecn_ev_since_last_ack.has_value()) {
            _pending_first_ecn_ev_since_last_ack = static_cast<uint16_t>(pkt.path_id());
        }
        return;
    }
    _pending_non_ecn_evs_since_last_ack.push_back(static_cast<uint16_t>(pkt.path_id()));
}

void SingSink::attachAggFeedbackToAckAndFlush(SingAck* ack_packet) {
    assert(ack_packet != nullptr);

    ack_packet->set_agg_non_ecn_evs(_pending_non_ecn_evs_since_last_ack);
    ack_packet->set_ecn_echo(_pending_ecn_since_last_ack);

    if (_pending_ecn_since_last_ack) {
        uint16_t ecn_ev = ack_packet->ev();
        if (_pending_first_ecn_ev_since_last_ack.has_value()) {
            ecn_ev = _pending_first_ecn_ev_since_last_ack.value();
        }
        ack_packet->set_ev(ecn_ev);
        ack_packet->set_agg_ecn_ev(ecn_ev);
    } else {
        ack_packet->clear_agg_ecn_ev();
    }

    _pending_non_ecn_evs_since_last_ack.clear();
    _pending_ecn_since_last_ack = false;
    _pending_first_ecn_ev_since_last_ack.reset();
}

void SingSink::noteBitmapAggFeedbackFromNewPacket(const SingDataPacket& pkt, bool ecn) {
    if (!ecn) {
        return;
    }
    _pending_ecn_evs_since_last_ack.push_back(static_cast<uint16_t>(pkt.path_id()));
}

bool SingSink::shouldSendCnp(int sf_id, simtime_picosec now) const {
    if (!isValidSubflowId(sf_id)) {
        return false;
    }
    if (sf_id >= (int)_has_sent_cnp_by_subflow.size()) {
        return false;
    }
    if (_has_sent_cnp_by_subflow[sf_id] == 0) {
        return true;
    }
    return now - _last_cnp_send_ts_by_subflow[sf_id] >= _cnp_min_interval;
}

void SingSink::markCnpSent(int sf_id, simtime_picosec now) {
    assert(isValidSubflowId(sf_id));
    _last_cnp_send_ts_by_subflow[sf_id] = now;
    _has_sent_cnp_by_subflow[sf_id] = 1;
}

void SingSink::maybeSendCnpForNewEcnPacket(const SingDataPacket& pkt, int sf_id, bool ecn) {
    if (!ecn) {
        return;
    }
    simtime_picosec now = _src->eventlist().now();
    if (!shouldSendCnp(sf_id, now)) {
        _stats.cnp_suppressed++;
        return;
    }

    SingCnp* cnp = SingCnp::newpkt(
        _flow, nullptr, static_cast<uint16_t>(sf_id), static_cast<uint16_t>(pkt.path_id()), _srcaddr);
    _nic.sendControlPacket(cnp, NULL, this);
    markCnpSent(sf_id, now);
    _stats.cnp_sent++;
}

void SingSink::attachBitmapAggFeedbackToAckAndFlush(SingAck* ack_packet) {
    assert(ack_packet != nullptr);

    ack_packet->set_agg_ecn_evs(_pending_ecn_evs_since_last_ack);
    ack_packet->set_ecn_echo(!_pending_ecn_evs_since_last_ack.empty());

    bool debug_flow = _src->flow()->flow_id() == SingSrc::_debug_flowid;
    if (debug_flow && !_pending_ecn_evs_since_last_ack.empty()) {
        cout << timeAsUs(_src->eventlist().now()) << " flowid " << _src->flow()->flow_id()
             << " bitmap_ackagg attach ecn_evs=" << _pending_ecn_evs_since_last_ack.size()
             << " first=" << _pending_ecn_evs_since_last_ack.front()
             << " last=" << _pending_ecn_evs_since_last_ack.back() << endl;
    }

    _pending_ecn_evs_since_last_ack.clear();
}

void SingSink::updateOooState(SingBasePacket::seq_t dsn, bool& force_ack) {
    if (_src->debug()) {
        cout << _nodename << " src " << _src->nodename()
             << " >>    cumulative dsn was: " << _expected_dsn
             << " flow " << _src->flow()->str() << endl;
    }

    if (dsn == _expected_dsn) {
        while (_dsn_rx_bitmap[++_expected_dsn]) {
            _dsn_rx_bitmap[_expected_dsn] = 0;
            _out_of_order_count--;
        }
        if (_src->debug()) {
            cout << " SingSink " << _nodename << " src " << _src->nodename()
                 << " >>    cumulative dsn now: " << _expected_dsn << " ooo count "
                 << _out_of_order_count << " flow " << _src->flow()->str() << endl;
        }

        if (_out_of_order_count == 0 && _ack_request) {
            force_ack = true;
            _ack_request = false;
        }
    } else {
        _dsn_rx_bitmap[dsn] = 1;
        _out_of_order_count++;
        _stats.out_of_order++;
    }
}

bool SingSink::handleDuplicateTrimmed(const SingDataPacket& pkt, int sf_id) {
    const auto dsn = pkt.dsn();
    if (!(dsn < _expected_dsn || _dsn_rx_bitmap[dsn])) {
        return false;
    }

    if (_src->debug()) {
        cout << " SingSink processTrimmed got a packet we already have dsn: " << dsn
             << " time " << timeAsNs(_src->eventlist().now())
             << " flow" << _src->flow()->str() << endl;
    }

    SingBasePacket::seq_t next_expected_ssn = subflowSinkOrNull(sf_id)->expectedSsn();
    SingAck* ack_packet = makeAck(pkt.path_id(), sackBitmapBase(dsn), dsn, pkt.send_ts(), false,
                                  pkt.retransmitted(), next_expected_ssn, sf_id, false);
    _nic.sendControlPacket(ack_packet, NULL, this);
    return true;
}

void SingSink::dispatchTrimmedToSubflow(const SingDataPacket& pkt, int sf_id) {
    SingSubflowSink* subflow_sink = subflowSinkOrNull(sf_id);
    subflow_sink->processTrimmed(pkt.ssn());
}

void SingSink::logTrimmedRx(const SingDataPacket& pkt) const {
    if (_src->debug()) {
        cout << " SingSink processTrimmed dsn " << pkt.dsn() << " time "
             << timeAsNs(_src->eventlist().now()) << " flow" << _src->flow()->str() << endl;
    }
}

SingNack* SingSink::buildTrimmedNack(const SingDataPacket& pkt, bool is_last_hop) {
    SingNack* nack_packet = nack(pkt.path_id(),
                                 pkt.dsn(),
                                 is_last_hop,
                                 (bool)(pkt.flags() & ECN_CE));
    nack_packet->set_subflow_id(pkt.subflow_id());
    return nack_packet;
}

void SingSink::resetAckCountersAfterSend(int sf_id) {
    _bytes_since_last_ack = 0;
    if (_ack_trigger_mode == AckTriggerMode::PER_SUBFLOW) {
        SingSubflowSink* subflow_sink = subflowSinkOrNull(sf_id);
        subflow_sink->resetBytesSinceLastAck();
    }
}

SingAck* SingSink::LegacyAckPolicy::onDuplicate(const SingDataPacket& pkt, int sf_id,
                                                bool ecn) {
    _sink.logDuplicate(pkt);
    _sink._stats.duplicates++;
    _sink._nic.logReceivedData(pkt.size(), 0);

    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(),
                         ecn ? pkt.dsn() : _sink.sackBitmapBase(pkt.dsn()),
                         pkt.dsn(),
                         pkt.send_ts(),
                         ecn,
                         pkt.retransmitted(),
                         next_expected_ssn,
                         sf_id,
                         false);
}

void SingSink::LegacyAckPolicy::onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                                            bool ecn, bool& force_ack) {
    (void)ecn;
    _sink.maybeMarkForceAckByFlags(pkt, force_ack);
    _sink.updateOooState(dsn, force_ack);
}

bool SingSink::LegacyAckPolicy::ackSendCondition(bool ecn, bool threshold_reached,
                                                 bool force_ack) const {
    return ecn || threshold_reached || force_ack;
}

SingAck* SingSink::LegacyAckPolicy::buildAck(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                                             int sf_id, bool ecn) {
    SingBasePacket::seq_t sack_base_dsn = (ecn || pkt.ar()) ? dsn : _sink.sackBitmapBase(dsn);
    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(), sack_base_dsn, dsn, pkt.send_ts(),
                         ecn,
                         pkt.retransmitted(), next_expected_ssn,
                         sf_id, pkt.path_sel_end());
}

void SingSink::LegacyAckPolicy::beforeSendAck(SingAck* ack_packet) {
    (void)ack_packet;
}

SingAck* SingSink::RepsAckAggPolicy::onDuplicate(const SingDataPacket& pkt, int sf_id,
                                                 bool ecn) {
    _sink.logDuplicate(pkt);
    _sink._stats.duplicates++;
    _sink._nic.logReceivedData(pkt.size(), 0);

    // Preserve sticky ECN even if duplicate triggered this ACK.
    if (ecn && !_sink._pending_first_ecn_ev_since_last_ack.has_value()) {
        _sink._pending_first_ecn_ev_since_last_ack = static_cast<uint16_t>(pkt.path_id());
    }
    if (ecn) {
        _sink._pending_ecn_since_last_ack = true;
    }

    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(),
                         _sink.sackBitmapBase(pkt.dsn()),
                         pkt.dsn(),
                         pkt.send_ts(),
                         false,
                         pkt.retransmitted(),
                         next_expected_ssn,
                         sf_id,
                         false);
}

void SingSink::RepsAckAggPolicy::onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                                             bool ecn, bool& force_ack) {
    // reps_ackagg disables path_sel_end-driven immediate ACK.
    _sink.maybeMarkForceAckByFlags(pkt, force_ack, false);
    _sink.updateOooState(dsn, force_ack);
    _sink.noteAggFeedbackFromNewPacket(pkt, ecn);
}

bool SingSink::RepsAckAggPolicy::ackSendCondition(bool ecn, bool threshold_reached,
                                                  bool force_ack) const {
    (void)ecn;
    // In reps_ackagg mode, ECN no longer forces immediate ACK.
    return threshold_reached || force_ack;
}

SingAck* SingSink::RepsAckAggPolicy::buildAck(const SingDataPacket& pkt,
                                              SingBasePacket::seq_t dsn, int sf_id, bool ecn) {
    (void)ecn;
    SingBasePacket::seq_t sack_base_dsn = pkt.ar() ? dsn : _sink.sackBitmapBase(dsn);
    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(), sack_base_dsn, dsn, pkt.send_ts(),
                         false,
                         pkt.retransmitted(), next_expected_ssn,
                         sf_id, false);
}

void SingSink::RepsAckAggPolicy::beforeSendAck(SingAck* ack_packet) {
    _sink.attachAggFeedbackToAckAndFlush(ack_packet);
}

SingAck* SingSink::BitmapAckAggPolicy::onDuplicate(const SingDataPacket& pkt, int sf_id,
                                                   bool ecn) {
    _sink.logDuplicate(pkt);
    _sink._stats.duplicates++;
    _sink._nic.logReceivedData(pkt.size(), 0);
    _sink.noteBitmapAggFeedbackFromNewPacket(pkt, ecn);

    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(),
                         _sink.sackBitmapBase(pkt.dsn()),
                         pkt.dsn(),
                         pkt.send_ts(),
                         false,
                         pkt.retransmitted(),
                         next_expected_ssn,
                         sf_id,
                         false);
}

void SingSink::BitmapAckAggPolicy::onNewPacket(const SingDataPacket& pkt,
                                               SingBasePacket::seq_t dsn, bool ecn,
                                               bool& force_ack) {
    // bitmap_ackagg disables path_sel_end-driven immediate ACK.
    _sink.maybeMarkForceAckByFlags(pkt, force_ack, false);
    _sink.updateOooState(dsn, force_ack);
    _sink.noteBitmapAggFeedbackFromNewPacket(pkt, ecn);
}

bool SingSink::BitmapAckAggPolicy::ackSendCondition(bool ecn, bool threshold_reached,
                                                    bool force_ack) const {
    (void)ecn;
    // In bitmap_ackagg mode, ECN no longer forces immediate ACK.
    return threshold_reached || force_ack;
}

SingAck* SingSink::BitmapAckAggPolicy::buildAck(const SingDataPacket& pkt,
                                                SingBasePacket::seq_t dsn, int sf_id, bool ecn) {
    (void)ecn;
    SingBasePacket::seq_t sack_base_dsn = pkt.ar() ? dsn : _sink.sackBitmapBase(dsn);
    SingBasePacket::seq_t next_expected_ssn = _sink.subflowSinkOrNull(sf_id)->expectedSsn();
    return _sink.makeAck(pkt.path_id(), sack_base_dsn, dsn, pkt.send_ts(),
                         false,
                         pkt.retransmitted(), next_expected_ssn,
                         sf_id, false);
}

void SingSink::BitmapAckAggPolicy::beforeSendAck(SingAck* ack_packet) {
    _sink.attachBitmapAggFeedbackToAckAndFlush(ack_packet);
}

void SingSink::processData(SingDataPacket& pkt) {
    const auto dsn = pkt.dsn();
    const int sf_id = (int)pkt.subflow_id();
    const bool ecn = (bool)(pkt.flags() & ECN_CE);
    bool force_ack = false;
    (void)subflowSinkOrNull(sf_id);
    assert(_ack_policy != nullptr);

    // Stage 1: validate receive window and classify packet.
    if (dsn > _expected_dsn + uecMaxInFlightPkts * SingSrc::_mtu) {
        abort();
    }
    logRxBegin(pkt);
    updateEcnStats(ecn, pkt.size());

    // Stage 2: duplicate handling (immediate ACK path).
    if (pkt.dsn() < _expected_dsn || _dsn_rx_bitmap[pkt.dsn()]) {
        SingAck* ack_packet = _ack_policy->onDuplicate(pkt, sf_id, ecn);
        _ack_policy->beforeSendAck(ack_packet);
        _nic.sendControlPacket(ack_packet, NULL, this);
        resetAckCountersAfterSend(sf_id);
        return;
    }

    // Stage 3: account new data and completion tracking.
    if (_received_bytes == 0) {
        force_ack = true;
    }
    accountNewDataPacket(pkt, sf_id);
    maybeSendCnpForNewEcnPacket(pkt, sf_id, ecn);

    // Stage 4: reorder update and ACK forcing hints.
    _ack_policy->onNewPacket(pkt, dsn, ecn, force_ack);

    const bool threshold_reached = ackThresholdReached(sf_id);
    logAckDecision(pkt, ecn, threshold_reached, force_ack, nullptr);
    if (!_ack_policy->ackSendCondition(ecn, threshold_reached, force_ack)) {
        return;
    }

    // Stage 5: build ACK packet and mode-specific feedback.
    SingAck* ack_packet = _ack_policy->buildAck(pkt, dsn, sf_id, ecn);
    _ack_policy->beforeSendAck(ack_packet);
    logAckDecision(pkt, ack_packet->ecn_echo(), threshold_reached, force_ack, ack_packet);

    // Stage 6: send ACK and reset the corresponding trigger counters.
    resetAckCountersAfterSend(sf_id);
    _nic.sendControlPacket(ack_packet, NULL, this);
}

void SingSink::processTrimmed(const SingDataPacket& pkt) {
    // Stage 1: base bookkeeping for trimmed packet.
    _nic.logReceivedTrim(pkt.size());
    _stats.trimmed++;
    const int sf_id = (int)pkt.subflow_id();
    (void)subflowSinkOrNull(sf_id);

    /* Currently, trimming support in htsim does not update DSCP code points.
     * The trim point TTL is preserved in the packet. We treat a trim as last-hop
     * when nexthop - trim_hop - 2 == 0 ("-2" is queue + pipe in htsim). */
    bool is_last_hop = (pkt.nexthop() - pkt.trim_hop() - 2) == 0;

    // Stage 2: duplicate trimmed packet => immediate ACK path.
    if (handleDuplicateTrimmed(pkt, sf_id)) {
        return;
    }

    // Stage 3: non-duplicate trimmed packet => update subflow and emit NACK.
    logTrimmedRx(pkt);
    dispatchTrimmedToSubflow(pkt, sf_id);

    SingNack* nack_packet = buildTrimmedNack(pkt, is_last_hop);
    _nic.sendControlPacket(nack_packet, NULL, this);
}

void SingSink::receivePacket(Packet& pkt, uint32_t port_num) {
    _stats.received++;
    _stats.bytes_received += pkt.size();  // should this include just the payload?

    switch (pkt.type()) {
        case SINGDATA:
            if (pkt.header_only()){
                processTrimmed((const SingDataPacket&)pkt);
                // cout << "SingSink::receivePacket receive trimmed packet\n";
                // assert(false);
            }else
                processData((SingDataPacket&)pkt);

            pkt.free();
            break;
        default:
            cout << "SingSink::receivePacket receive weird packets\n";
            abort();
    }
}

uint16_t SingSink::nextEntropy() {
    int spraymask = (1 << TGT_EV_SIZE) - 1;
    int fixedmask = ~spraymask;
    int idx = _entropy & spraymask;
    int fixed_entropy = _entropy & fixedmask;
    int ev = ++idx & spraymask;

    _entropy = fixed_entropy | ev;  // save for next pkt

    return ev;
}

bool SingSink::ackThresholdReached(int sf_id) {
    if (_ack_trigger_mode == AckTriggerMode::PER_SUBFLOW) {
        if (sf_id >= 0 && sf_id < (int)_subflow_sinks.size()) {
            return _subflow_sinks[sf_id]->shouldSack(_bytes_unacked_threshold);
        }
    }
    return _bytes_since_last_ack >= _bytes_unacked_threshold;
}

SingBasePacket::seq_t SingSink::sackBitmapBase(SingBasePacket::seq_t dsn) {
    return max((int64_t)dsn - 63, (int64_t)(_expected_dsn + 1));
}

SingBasePacket::seq_t SingSink::sackBitmapBaseIdeal() {
    uint8_t lowest_value = UINT8_MAX;
    SingBasePacket::seq_t lowest_position = 0;

    // find the lowest non-zero value in the sack bitmap; that is the candidate for the base, since
    // it is the oldest packet that we are yet to sack. on sack bitmap construction that covers a
    // given seqno, the value is incremented.
    for (SingBasePacket::seq_t crt = _expected_dsn; crt <= _high_dsn; crt++)
        if (_dsn_rx_bitmap[crt] && _dsn_rx_bitmap[crt] < lowest_value) {
            lowest_value = _dsn_rx_bitmap[crt];
            lowest_position = crt;
        }

    if (lowest_position + 64 > _high_dsn)
        lowest_position = _high_dsn - 64;

    if (lowest_position <= _expected_dsn)
        lowest_position = _expected_dsn + 1;

    return lowest_position;
}

uint64_t SingSink::buildSackBitmap(SingBasePacket::seq_t sack_base_dsn) {
    // take the next 64 entries from sack_base_dsn and create a SACK bitmap with them
    if (_src->debug())
        cout << " SingSink: building sack for sack_base_dsn " << sack_base_dsn << endl;
    uint64_t bitmap = (uint64_t)(_dsn_rx_bitmap[sack_base_dsn] != 0) << 63;

    for (int i = 1; i < 64; i++) {
        bitmap = bitmap >> 1 | (uint64_t)(_dsn_rx_bitmap[sack_base_dsn + i] != 0) << 63;
        if (_src->debug() && (_dsn_rx_bitmap[sack_base_dsn + i] != 0))
            cout << "     Sack: " << sack_base_dsn + i << endl;

        if (_dsn_rx_bitmap[sack_base_dsn + i]) {
            // remember that we sacked this packet
            if (_dsn_rx_bitmap[sack_base_dsn + i] < UINT8_MAX)
                _dsn_rx_bitmap[sack_base_dsn + i]++;
        }
    }
    if (_src->debug())
        cout << "       bitmap is: " << bitmap << endl;
    return bitmap;
}

SingAck* SingSink::makeAck(uint16_t path_id, SingBasePacket::seq_t sack_base_dsn,
                           SingBasePacket::seq_t trigger_dsn, simtime_picosec echo_send_ts,
                           bool ce, bool rtx_echo,
                           SingBasePacket::seq_t next_expected_ssn, uint16_t subflow_id,
                           bool path_sel_end) {
    uint64_t bitmap = buildSackBitmap(sack_base_dsn);
    SingAck* pkt = SingAck::newpkt(_flow, NULL, next_expected_ssn, _expected_dsn, subflow_id,
                                   sack_base_dsn, trigger_dsn, echo_send_ts, path_id, ce, _recvd_bytes,
                                   _rcv_cwnd_pen, _srcaddr);
    pkt->set_bitmap(bitmap);
    pkt->set_ooo(_out_of_order_count);
    pkt->set_rtx_echo(rtx_echo);
    pkt->set_path_sel_end(path_sel_end);
    return pkt;
}

SingNack* SingSink::nack(uint16_t path_id, SingBasePacket::seq_t seqno,bool last_hop, bool ecn_echo) {
    SingNack* pkt = SingNack::newpkt(_flow, NULL, seqno, path_id,  _recvd_bytes,_rcv_cwnd_pen,_srcaddr);
    pkt->set_last_hop(last_hop);
    pkt->set_ecn_echo(ecn_echo);
    pkt->set_subflow_id(0);
    return pkt;
}

void SingSink::setEndTrigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
};


/*static unsigned pktByteTimes(unsigned size) {
    // IPG (96 bit times) + preamble + SFD + ether header + FCS = 38B
    return max(size, 46u) + 38;
}*/

uint32_t SingSink::reorder_buffer_size() {
    uint32_t count = 0;
    // it's not very efficient to count each time, but if we only do
    // this occasionally when the sink logger runs, it should be OK.
    for (uint32_t i = 0; i < uecMaxInFlightPkts; i++) {
        if (_dsn_rx_bitmap[i])
            count++;
    }
    return count;
}

uint32_t SingSink::getConfiguredMaxWnd() {
    return _src->configuredMaxWnd();
}
