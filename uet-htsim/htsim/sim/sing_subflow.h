// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SUBFLOW_H
#define SING_SUBFLOW_H

#include <memory>
#include <map>
#include <cstdint>
#include <deque>
#include <unordered_set>

#include "config.h"
#include "singpacket.h"
#include "sing_cc.h"
#include "modular_vector.h"

class SingSrc;
class SingSink;

class SingSubflow {
public:
    SingSubflow(SingSrc& src, int subflow_id, std::unique_ptr<BaseCC> cc, uint16_t entropy);

    // NIC scheduling interface
    mem_b sendPacketFromNIC();
    simtime_picosec computeNextSendTime(mem_b pkt_size) const;

    // Called by SingSrc after connection-level processing
    void processAck(simtime_picosec raw_rtt,
                    SingBasePacket::seq_t next_expected_ssn, bool ecn,
                    mem_b newly_recvd_bytes);
    void processNack(mem_b pkt_size, bool last_hop);
    void processTx(mem_b pkt_bytes);
    void processCnp();

    SingSrc* parentSrc() { return &_src; }
    int subflowId() const { return _subflow_id; }
    BaseCC* cc() const { return _cc.get(); }
    uint16_t entropy() const { return _entropy; }
    mem_b ssnInFlight() const { return _ssn_in_flight; }
    SingBasePacket::seq_t highestSsnSent() const { return _highest_ssn_sent; }

    // SSN -> DSN lookup
    SingBasePacket::seq_t dsnForSsn(SingBasePacket::seq_t ssn) const;
    bool hasSsnMapping(SingBasePacket::seq_t ssn) const;

    // Allocate an SSN for a given DSN and record the mapping (with packet size)
    SingBasePacket::seq_t allocateSsn(SingBasePacket::seq_t dsn, mem_b pkt_size);

    // Remove SSN mapping entries below next_expected_ssn; returns total bytes acked
    mem_b cleanupSsnMap(SingBasePacket::seq_t next_expected_ssn);

    void adjustSsnInFlight(mem_b delta) { _ssn_in_flight += delta; }
    void reduceSsnInFlight(mem_b delta) {
        _ssn_in_flight = (_ssn_in_flight >= delta) ? _ssn_in_flight - delta : 0;
    }

    void enqueueRtx(SingBasePacket::seq_t dsn);
    bool hasRtx() const { return !_rtx_dsn_queue.empty(); }
    SingBasePacket::seq_t peekRtxDsn() const;
    SingBasePacket::seq_t popRtxDsn();
    bool eraseRtxDsn(SingBasePacket::seq_t dsn);

private:
    SingSrc& _src;
    int _subflow_id;
    std::unique_ptr<BaseCC> _cc;
    uint16_t _entropy;

    SingBasePacket::seq_t _highest_ssn_sent = 0;
    mem_b _ssn_in_flight = 0;

    struct SsnInfo {
        SingBasePacket::seq_t dsn;
        mem_b pkt_size;
    };
    // SSN -> {DSN, pkt_size} mapping; entries removed as SSN cumack advances
    std::map<SingBasePacket::seq_t, SsnInfo> _ssn_info_map;

    std::deque<SingBasePacket::seq_t> _rtx_dsn_queue;
    std::unordered_set<SingBasePacket::seq_t> _rtx_pending_set;
};

enum class AckTriggerMode { PER_SUBFLOW, GLOBAL };

class SingSubflowSink {
public:
    SingSubflowSink(SingSink& sink, int subflow_id);

    // Called by SingSink after dispatching by subflow_id
    void processData(SingBasePacket::seq_t ssn, mem_b pkt_size);
    void processTrimmed(SingBasePacket::seq_t ssn);

    int subflowId() const { return _subflow_id; }
    SingBasePacket::seq_t expectedSsn() const { return _expected_ssn; }
    mem_b ssnReceivedBytes() const { return _ssn_received_bytes; }
    mem_b bytesSinceLastAck() const { return _bytes_since_last_ack; }
    void resetBytesSinceLastAck() { _bytes_since_last_ack = 0; }

    bool shouldSack(mem_b threshold) const {
        return _bytes_since_last_ack >= threshold;
    }

private:
    static constexpr unsigned SSN_REORDER_WINDOW = 1 << 14;

    SingSink& _sink;
    int _subflow_id;

    SingBasePacket::seq_t _expected_ssn = 0;
    SingBasePacket::seq_t _high_ssn = 0;
    mem_b _bytes_since_last_ack = 0;
    mem_b _ssn_received_bytes = 0;
    ModularVector<uint8_t, SSN_REORDER_WINDOW> _ssn_rx_bitmap;
};

#endif // SING_SUBFLOW_H
