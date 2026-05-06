// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_subflow.h"
#include "sing_src.h"
#include <cassert>

////////////////////////////////////////////////////////////////
//  SingSubflow
////////////////////////////////////////////////////////////////

SingSubflow::SingSubflow(SingSrc& src, int subflow_id, std::unique_ptr<BaseCC> cc, uint16_t entropy)
    : _src(src), _subflow_id(subflow_id), _cc(std::move(cc)), _entropy(entropy) {}

SingBasePacket::seq_t SingSubflow::allocateSsn(SingBasePacket::seq_t dsn, mem_b pkt_size) {
    SingBasePacket::seq_t ssn = _highest_ssn_sent++;
    _ssn_info_map[ssn] = {dsn, pkt_size};
    return ssn;
}

SingBasePacket::seq_t SingSubflow::dsnForSsn(SingBasePacket::seq_t ssn) const {
    auto it = _ssn_info_map.find(ssn);
    assert(it != _ssn_info_map.end());
    return it->second.dsn;
}

bool SingSubflow::hasSsnMapping(SingBasePacket::seq_t ssn) const {
    return _ssn_info_map.find(ssn) != _ssn_info_map.end();
}

mem_b SingSubflow::cleanupSsnMap(SingBasePacket::seq_t next_expected_ssn) {
    mem_b acked_bytes = 0;
    auto it = _ssn_info_map.begin();
    while (it != _ssn_info_map.end() && it->first < next_expected_ssn) {
        acked_bytes += it->second.pkt_size;
        it = _ssn_info_map.erase(it);
    }
    return acked_bytes;
}

mem_b SingSubflow::sendPacketFromNIC() {
    const Route* route = _src.getPortRoute(0);
    return _src.sendPacketForSubflow(*route, *this);
}

simtime_picosec SingSubflow::computeNextSendTime(mem_b pkt_size) const {
    if (pkt_size == 0) {
        return _src.eventlist().now() + _src._base_rtt;
    }
    if (_src._base_rtt == 0) {
        return _src.eventlist().now() + _src._base_rtt;
    }

    double rate = 0.0;
    if (_cc && _cc->hasRate()) {
        rate = _cc->rateBytesPerPs();
    } else if (_cc && _cc->hasWindow() && _src._base_rtt > 0) {
        rate = (double)_cc->cwndBytes() / (double)_src._base_rtt;
    }

    if (rate <= 0.0) {
        return _src.eventlist().now() + _src._base_rtt;
    }

    simtime_picosec interval = (simtime_picosec)((double)pkt_size / rate);
    if (interval < 100) {
        interval = 100;
    }
    return _src.eventlist().now() + interval;
}

void SingSubflow::processAck(simtime_picosec raw_rtt,
                             SingBasePacket::seq_t next_expected_ssn, bool ecn,
                             mem_b /*newly_recvd_bytes*/) {
    // Compute ssn_acked_bytes precisely from SSN-level map cleanup
    mem_b ssn_acked_bytes = cleanupSsnMap(next_expected_ssn);

    // Reduce SSN-level in-flight by exactly the bytes just acknowledged
    _ssn_in_flight = (_ssn_in_flight >= ssn_acked_bytes) ? _ssn_in_flight - ssn_acked_bytes : 0;

    _cc->onAck(raw_rtt, ssn_acked_bytes, ecn, _ssn_in_flight);
}

void SingSubflow::processNack(mem_b pkt_size, bool last_hop) {
    reduceSsnInFlight(pkt_size);
    _cc->onNack(pkt_size, last_hop, _ssn_in_flight);
}

void SingSubflow::processTx(mem_b pkt_bytes) {
    _cc->onTx(pkt_bytes);
}

void SingSubflow::processCnp() {
    _cc->onCnp();
}

void SingSubflow::enqueueRtx(SingBasePacket::seq_t dsn) {
    if (_rtx_pending_set.insert(dsn).second) {
        _rtx_dsn_queue.push_back(dsn);
    }
}

SingBasePacket::seq_t SingSubflow::peekRtxDsn() const {
    assert(!_rtx_dsn_queue.empty());
    return _rtx_dsn_queue.front();
}

SingBasePacket::seq_t SingSubflow::popRtxDsn() {
    assert(!_rtx_dsn_queue.empty());
    SingBasePacket::seq_t dsn = _rtx_dsn_queue.front();
    _rtx_dsn_queue.pop_front();
    _rtx_pending_set.erase(dsn);
    return dsn;
}

bool SingSubflow::eraseRtxDsn(SingBasePacket::seq_t dsn) {
    auto it_set = _rtx_pending_set.find(dsn);
    if (it_set == _rtx_pending_set.end()) {
        return false;
    }
    _rtx_pending_set.erase(it_set);
    for (auto it = _rtx_dsn_queue.begin(); it != _rtx_dsn_queue.end(); ++it) {
        if (*it == dsn) {
            _rtx_dsn_queue.erase(it);
            return true;
        }
    }
    return true;
}

////////////////////////////////////////////////////////////////
//  SingSubflowSink
////////////////////////////////////////////////////////////////

SingSubflowSink::SingSubflowSink(SingSink& sink, int subflow_id)
    : _sink(sink), _subflow_id(subflow_id), _ssn_rx_bitmap(0) {}

void SingSubflowSink::processData(SingBasePacket::seq_t ssn, mem_b pkt_size) {
    _bytes_since_last_ack += pkt_size;
    _ssn_received_bytes += pkt_size;

    assert(ssn <= _expected_ssn + SSN_REORDER_WINDOW);

    if (ssn == _expected_ssn) {
        while (true) {
            _expected_ssn++;
            if (_ssn_rx_bitmap[_expected_ssn] == 0) {
                break;
            }
            _ssn_rx_bitmap[_expected_ssn] = 0;
        }
    } else if (ssn > _expected_ssn) {
        _ssn_rx_bitmap[ssn] = 1;
    }

    if (ssn > _high_ssn) {
        _high_ssn = ssn;
    }
}

void SingSubflowSink::processTrimmed(SingBasePacket::seq_t ssn) {
    if (ssn > _high_ssn) {
        _high_ssn = ssn;
    }
}
