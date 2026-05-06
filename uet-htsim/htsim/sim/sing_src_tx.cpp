// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_src.h"
#include "sing_sink.h"
#include <math.h>
#include <cstdint>
#include "sing_logger.h"

using namespace std;

mem_b SingSrc::getNextPacketSize() {
    if (_default_subflow == nullptr) {
        if (_backlog == 0) {
            return 0;
        }
        return min((mem_b)_mtu, _backlog);
    }
    return getNextPacketSizeForSubflow(*_default_subflow);
}

mem_b SingSrc::getNextPacketSizeForSubflow(const SingSubflow& sf) const {
    if (sf.hasRtx()) {
        SingBasePacket::seq_t dsn = sf.peekRtxDsn();
        auto it = _tx_bitmap.find(dsn);
        assert(it != _tx_bitmap.end());
        return it->second.pkt_size;
    }

    if (_backlog == 0) {
        return 0;
    }
    return min((mem_b)_mtu, _backlog);
}

// ====== New scheduling interfaces ======

bool SingSrc::hasRtxForSubflow(const SingSubflow& sf) const {
    return sf.hasRtx();
}

bool SingSrc::hasNewDataBacklog() const {
    return _backlog > 0;
}

bool SingSrc::canSendNewData() const {
    if (_max_unack_window > 0 && _in_flight >= _max_unack_window) {
        return false;
    }
    return true;
}

BaseCC* SingSrc::cc() const {
    return _default_subflow ? _default_subflow->cc() : nullptr;
}

mem_b SingSrc::currentWindowBytes() const {
    auto* c = cc();
    if (c && c->hasWindow()) {
        return c->cwndBytes();
    }
    return 0;
}

simtime_picosec SingSrc::currentNsccAvgDelay() const {
    auto* c = cc();
    const Nscc* nscc = dynamic_cast<const Nscc*>(c);
    return nscc ? nscc->avgDelay() : 0;
}

simtime_picosec SingSrc::computeNextSendTime(mem_b pkt_size) const {
    if (_default_subflow) {
        return _default_subflow->computeNextSendTime(pkt_size);
    }
    return eventlist().now() + _base_rtt;
}

mem_b SingSrc::sendPacketFromNIC(const Route& route) {
    return sendPacketForSubflow(route, *_default_subflow);
}

mem_b SingSrc::sendPacketForSubflow(const Route& route, SingSubflow& sf) {
    mem_b sent_bytes = 0;

    if (sf.hasRtx()) {
        sent_bytes = sendRtxPacket(route, &sf);
    } else if (_backlog > 0) {
        sent_bytes = sendNewPacket(route, &sf);
    }

    if (sent_bytes > 0) {
        sf.processTx(sent_bytes);
    }

    return sent_bytes;
}

// ====== End new scheduling interfaces ======

SingSrc::PreparedDataPacket SingSrc::prepareDataPacketForSend(
    const Route& route, SingBasePacket::seq_t dsn, mem_b pkt_size,
    bool is_rtx, SingSubflow* sf, optional<SingBasePacket::seq_t> forced_ssn) {
    assert(sf != nullptr);
    uint16_t sf_id = sf->subflowId();

    SingBasePacket::seq_t ssn = forced_ssn.has_value() ? forced_ssn.value()
                                                       : sf->allocateSsn(dsn, pkt_size);
    if (forced_ssn.has_value()) {
        assert(sf->hasSsnMapping(ssn));
    }
    sf->adjustSsnInFlight(pkt_size);

    auto* p = SingDataPacket::newpkt(_flow, route, dsn, pkt_size, is_rtx, _dstaddr);
    p->set_dsn(dsn);
    p->set_ssn(ssn);
    p->set_subflow_id(sf_id);
    p->set_send_ts(eventlist().now());

    mem_b wnd = currentWindowBytes();
    uint64_t wnd_pkts = max((mem_b)_mss, wnd) / _mss;
    SingMultipath::PathSelectionResult path_choice = {0, false};
    if (_subflows.size() > 1) {
        // Pathwise mode is independent from native LB: fixed entropy and no path_sel_end.
        path_choice.path_id = sf->entropy();
        path_choice.path_sel_end = false;
    } else {
        path_choice = _mp->selectPathForPacket(pkt_size, _highest_sent, wnd_pkts);
    }
    p->set_pathid(path_choice.path_id);
    p->set_path_sel_end(path_choice.path_sel_end);
    p->flow().logTraffic(*p, *this, TrafficLogger::PKT_CREATESEND);

    return {p, ssn, sf_id, path_choice.path_id, path_choice.path_sel_end};
}

mem_b SingSrc::sendNewPacket(const Route& route, SingSubflow* sf) {
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename
             << " sendNewPacket highest_sent " << _highest_sent << " h*m "
             << _highest_sent * _mss << " backlog " << _backlog << " flow "
             << _flow.str() << endl;
    assert(_backlog > 0);

    mem_b full_pkt_size = 0;
    if (_msg_tracker.has_value()) {
        full_pkt_size = _msg_tracker.value()->getNextPacket(_highest_sent);
    } else {
        full_pkt_size = _mtu;
        if (_backlog < _mtu) {
            full_pkt_size = _backlog;
        }
    }
    assert(full_pkt_size <= _mtu);

    _backlog -= full_pkt_size;
    assert(_backlog >= 0);
    _in_flight += full_pkt_size;

    auto dsn = _highest_sent;
    auto prepared = prepareDataPacketForSend(route, dsn, full_pkt_size,
                                             false, sf);
    auto* p = prepared.pkt;

    mem_b wnd = currentWindowBytes();
    auto* c = cc();
    if (_backlog == 0 || (_sender_based_cc && c && c->hasWindow() && (_in_flight + full_pkt_size) >= wnd))
        p->set_ar(true);

    createSendRecord(dsn, full_pkt_size, prepared.ssn, prepared.subflow_id);
    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " sending pkt " << dsn
             << " size " << full_pkt_size << " ack request " << p->ar()
             << " cwnd " << wnd << " ev " << prepared.ev << " in_flight " << _in_flight << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " sending pkt " << dsn
             << " size " << full_pkt_size << " cwnd " << wnd << " ev " << prepared.ev
             << " in_flight " << _in_flight
             << " ar " << p->ar()
             << " path_sel_end " << (prepared.path_sel_end ? "YES" : "NO")
             << endl;
    }
    p->sendOn();
    _highest_sent++;
    _stats.new_pkts_sent++;
    startRTO(eventlist().now());

    assert(full_pkt_size > 0);
    return full_pkt_size;
}

mem_b SingSrc::sendRtxPacket(const Route& route, SingSubflow* sf) {
    assert(sf != nullptr);
    if (!sf->hasRtx()) {
        return 0;
    }

    SingBasePacket::seq_t dsn = sf->popRtxDsn();
    auto it = _tx_bitmap.find(dsn);
    if (it == _tx_bitmap.end()) {
        return 0;
    }

    sendRecord& tx = it->second;
    assert(tx.state == TxState::PENDING_RTX);
    assert(tx.owner_subflow_id == sf->subflowId());
    assert(_pending_rtx_pkts > 0);
    _pending_rtx_pkts--;

    mem_b full_pkt_size = tx.pkt_size;
    _in_flight += full_pkt_size;

    auto prepared = prepareDataPacketForSend(route, dsn, full_pkt_size,
                                             true, sf, tx.ssn);
    auto* p = prepared.pkt;

    createSendRecord(dsn, full_pkt_size, prepared.ssn, prepared.subflow_id);

    if (_debug_src)
        cout << timeAsUs(eventlist().now()) << " " << _flow.str() << " " << _nodename << " sending rtx pkt " << dsn
             << " size " << full_pkt_size << " cwnd " << currentWindowBytes()
             << " in_flight " << _in_flight << endl;
    if (_flow.flow_id() == _debug_flowid) {
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " sending rtx pkt " << dsn
             << " size " << full_pkt_size << " cwnd " << currentWindowBytes()
             << " ev " << prepared.ev << " rtx_times " << _rtx_times[dsn]
             << " in_flight " << _in_flight
             << " path_sel_end " << (prepared.path_sel_end ? "YES" : "NO")
             << endl;
    }
    p->set_ar(true);
    p->sendOn();
    _stats.rtx_pkts_sent++;
    startRTO(eventlist().now());
    return full_pkt_size;
}

void SingSrc::createSendRecord(SingBasePacket::seq_t seqno,
                               mem_b full_pkt_size,
                               SingBasePacket::seq_t ssn,
                               uint16_t owner_subflow_id) {
    if (_debug_src)
        cout << _flow.str() << " " << _nodename << " createSendRecord seqno: " << seqno
             << " size " << full_pkt_size << " ssn " << ssn
             << " sf " << owner_subflow_id << endl;

    auto now = eventlist().now();
    auto it = _tx_bitmap.find(seqno);
    if (it == _tx_bitmap.end()) {
        sendRecord rec;
        rec.pkt_size = full_pkt_size;
        rec.send_time = now;
        rec.ssn = ssn;
        rec.owner_subflow_id = owner_subflow_id;
        rec.state = TxState::IN_FLIGHT;
        _tx_bitmap.emplace(seqno, rec);
    } else {
        _ssn_to_dsn.erase({it->second.owner_subflow_id, it->second.ssn});
        it->second.pkt_size = full_pkt_size;
        it->second.send_time = now;
        it->second.ssn = ssn;
        it->second.owner_subflow_id = owner_subflow_id;
        it->second.state = TxState::IN_FLIGHT;
    }

    _ssn_to_dsn[{owner_subflow_id, ssn}] = seqno;
    _send_times.emplace(now, seqno);

    if (_rtx_times.find(seqno) == _rtx_times.end()) {
        _rtx_times.emplace(seqno, 0);
    } else {
        _rtx_times[seqno] += 1;
    }
}

bool SingSrc::isPendingRtx(SingBasePacket::seq_t seqno) const {
    auto it = _tx_bitmap.find(seqno);
    if (it == _tx_bitmap.end()) {
        return false;
    }
    return it->second.state == TxState::PENDING_RTX;
}
