// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SINK_H
#define SING_SINK_H

#include <memory>
#include <list>
#include <set>
#include <optional>
#include <vector>

#include "eventlist.h"
#include "trigger.h"
#include "singpacket.h"
#include "modular_vector.h"
#include "sing_subflow.h"
#include "sing_nic.h"

static const unsigned uecMaxInFlightPkts = 1 << 14;
class SingSrc;
class SingSink;
class SingSinkPort : public PacketSink {
public:
    SingSinkPort(SingSink& sink, uint32_t portnum);
    void setRoute(const Route& route);
    inline const Route* route() const {return _route;}
    virtual void receivePacket(Packet& pkt);
    virtual const string& nodename();
private:
    SingSink& _sink;
    uint8_t _port_num;
    const Route* _route;
};

class SingSink : public DataReceiver {
   public:
    struct Stats {
        uint64_t received;
        uint64_t bytes_received;
        uint64_t duplicates;
        uint64_t out_of_order;
        uint64_t trimmed;
        uint64_t ecn_received;
        uint64_t ecn_bytes_received;
        uint64_t cnp_sent;
        uint64_t cnp_suppressed;
    };

    SingSink(TrafficLogger* trafficLogger, SingNIC& nic, uint32_t no_of_ports);
    void receivePacket(Packet& pkt, uint32_t port_num);

    void processData(SingDataPacket& pkt);
    void processTrimmed(const SingDataPacket& pkt);

    void createSubflowSinks(int num_subflows);
    SingSubflowSink* getSubflowSink(int subflow_id);
    AckTriggerMode ackTriggerMode() const { return _ack_trigger_mode; }
    void setAckTriggerMode(AckTriggerMode mode) { _ack_trigger_mode = mode; }
    enum class AckFeedbackMode { LEGACY, REPS_ACKAGG, BITMAP_ACKAGG };
    AckFeedbackMode ackFeedbackMode() const { return _ack_feedback_mode; }
    void setAckFeedbackMode(AckFeedbackMode mode);
    void setCnpMinInterval(simtime_picosec interval_ps) {
        _cnp_min_interval = interval_ps > 0 ? interval_ps : timeFromUs((uint32_t)2);
    }
    simtime_picosec cnpMinInterval() const { return _cnp_min_interval; }

    virtual const string& nodename() { return _nodename; }
    virtual uint64_t cumulative_ack() { return _expected_dsn; }
    virtual uint32_t drops() { return 0; }

    inline flowid_t flowId() const { return _flow.flow_id(); }

    bool ackThresholdReached(int sf_id = -1);
    uint16_t unackedPackets();
    void setEndTrigger(Trigger& trigger);

    SingBasePacket::seq_t sackBitmapBase(SingBasePacket::seq_t dsn);
    SingBasePacket::seq_t sackBitmapBaseIdeal();
    uint64_t buildSackBitmap(SingBasePacket::seq_t sack_base_dsn);
    SingAck* makeAck(uint16_t path_id, SingBasePacket::seq_t sack_base_dsn,
                     SingBasePacket::seq_t trigger_dsn, simtime_picosec echo_send_ts,
                     bool ce, bool rtx_echo,
                     SingBasePacket::seq_t next_expected_ssn, uint16_t subflow_id,
                     bool path_sel_end = false);

    SingNack* nack(uint16_t path_id, SingBasePacket::seq_t seqno, bool last_hop, bool ecn_echo);

    const Stats& stats() const { return _stats; }
    void connectPort(uint32_t port_num, SingSrc& src, const Route& routeback);
    const Route* getPortRoute(uint32_t port_num) const {return _ports[port_num]->route();}
    SingSinkPort* getPort(uint32_t port_num) {return _ports[port_num];}
    void setSrc(uint32_t s) { _srcaddr = s; }
    inline void setFlowId(flowid_t flow_id) { _flow.set_flowid(flow_id); }

    inline SingNIC* getNIC() const { return &_nic; }

    uint16_t nextEntropy();

    SingSrc* getSrc() { return _src; }
    uint32_t getConfiguredMaxWnd();

    static mem_b _bytes_unacked_threshold;
    static int TGT_EV_SIZE;

    // for sink logger
    inline mem_b total_received() const { return _stats.bytes_received; }
    uint32_t reorder_buffer_size();  // count is in packets

   private:
    bool isValidSubflowId(int sf_id) const;
    SingSubflowSink* subflowSinkOrNull(int sf_id);

    void logRxBegin(const SingDataPacket& pkt) const;
    void logDuplicate(const SingDataPacket& pkt) const;
    void logAckDecision(const SingDataPacket& pkt, bool ecn, bool threshold_reached,
                        bool force_ack, const SingAck* ack_packet) const;

    void updateEcnStats(bool ecn, mem_b pkt_size);
    void accountNewDataPacket(const SingDataPacket& pkt, int sf_id);
    void maybeMarkForceAckByFlags(const SingDataPacket& pkt, bool& force_ack,
                                  bool include_path_sel_end_force = true);
    void noteAggFeedbackFromNewPacket(const SingDataPacket& pkt, bool ecn);
    void attachAggFeedbackToAckAndFlush(SingAck* ack_packet);
    void noteBitmapAggFeedbackFromNewPacket(const SingDataPacket& pkt, bool ecn);
    void attachBitmapAggFeedbackToAckAndFlush(SingAck* ack_packet);
    bool shouldSendCnp(int sf_id, simtime_picosec now) const;
    void markCnpSent(int sf_id, simtime_picosec now);
    void maybeSendCnpForNewEcnPacket(const SingDataPacket& pkt, int sf_id, bool ecn);
    void updateOooState(SingBasePacket::seq_t dsn, bool& force_ack);
    bool handleDuplicateTrimmed(const SingDataPacket& pkt, int sf_id);
    void dispatchTrimmedToSubflow(const SingDataPacket& pkt, int sf_id);
    void logTrimmedRx(const SingDataPacket& pkt) const;

    SingNack* buildTrimmedNack(const SingDataPacket& pkt, bool is_last_hop);
    void resetAckCountersAfterSend(int sf_id);

    class AckPolicy {
    public:
        explicit AckPolicy(SingSink& sink) : _sink(sink) {}
        virtual ~AckPolicy() = default;

        virtual SingAck* onDuplicate(const SingDataPacket& pkt, int sf_id, bool ecn) = 0;
        virtual void onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                                 bool ecn, bool& force_ack) = 0;
        virtual bool ackSendCondition(bool ecn, bool threshold_reached, bool force_ack) const = 0;
        virtual SingAck* buildAck(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                                  int sf_id, bool ecn) = 0;
        virtual void beforeSendAck(SingAck* ack_packet) = 0;

    protected:
        SingSink& _sink;
    };

    class LegacyAckPolicy final : public AckPolicy {
    public:
        explicit LegacyAckPolicy(SingSink& sink) : AckPolicy(sink) {}

        SingAck* onDuplicate(const SingDataPacket& pkt, int sf_id, bool ecn) override;
        void onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                         bool ecn, bool& force_ack) override;
        bool ackSendCondition(bool ecn, bool threshold_reached, bool force_ack) const override;
        SingAck* buildAck(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                          int sf_id, bool ecn) override;
        void beforeSendAck(SingAck* ack_packet) override;
    };

    class RepsAckAggPolicy final : public AckPolicy {
    public:
        explicit RepsAckAggPolicy(SingSink& sink) : AckPolicy(sink) {}

        SingAck* onDuplicate(const SingDataPacket& pkt, int sf_id, bool ecn) override;
        void onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                         bool ecn, bool& force_ack) override;
        bool ackSendCondition(bool ecn, bool threshold_reached, bool force_ack) const override;
        SingAck* buildAck(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                          int sf_id, bool ecn) override;
        void beforeSendAck(SingAck* ack_packet) override;
    };

    class BitmapAckAggPolicy final : public AckPolicy {
    public:
        explicit BitmapAckAggPolicy(SingSink& sink) : AckPolicy(sink) {}

        SingAck* onDuplicate(const SingDataPacket& pkt, int sf_id, bool ecn) override;
        void onNewPacket(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                         bool ecn, bool& force_ack) override;
        bool ackSendCondition(bool ecn, bool threshold_reached, bool force_ack) const override;
        SingAck* buildAck(const SingDataPacket& pkt, SingBasePacket::seq_t dsn,
                          int sf_id, bool ecn) override;
        void beforeSendAck(SingAck* ack_packet) override;
    };

    uint32_t _no_of_ports;
    vector <SingSinkPort*> _ports;
    uint32_t _srcaddr;
    SingNIC& _nic;
    SingSrc* _src;
    PacketFlow _flow;
    SingBasePacket::seq_t _expected_dsn;
    SingBasePacket::seq_t _high_dsn;
    SingBasePacket::seq_t
        _ref_dsn;  // used for SACK bitmap calculation in spec, unused here for NOW.
    bool _receiver_done; // flag to track if receiver completion log has been printed

    // Subflow sinks
    std::vector<std::unique_ptr<SingSubflowSink>> _subflow_sinks;
    AckTriggerMode _ack_trigger_mode = AckTriggerMode::GLOBAL;
    AckFeedbackMode _ack_feedback_mode = AckFeedbackMode::LEGACY;
    AckPolicy* _ack_policy = nullptr;
    std::unique_ptr<LegacyAckPolicy> _legacy_ack_policy;
    std::unique_ptr<RepsAckAggPolicy> _reps_ackagg_policy;
    std::unique_ptr<BitmapAckAggPolicy> _bitmap_ackagg_policy;
    std::vector<uint16_t> _pending_non_ecn_evs_since_last_ack;
    bool _pending_ecn_since_last_ack = false;
    std::optional<uint16_t> _pending_first_ecn_ev_since_last_ack;
    std::vector<uint16_t> _pending_ecn_evs_since_last_ack;
    simtime_picosec _cnp_min_interval = timeFromUs((uint32_t)2);
    std::vector<simtime_picosec> _last_cnp_send_ts_by_subflow;
    std::vector<uint8_t> _has_sent_cnp_by_subflow;

    //received payload bytes, used to decide when flow has finished.
    mem_b _received_bytes;
    mem_b _bytes_since_last_ack;

    //used to help the sender slide his window.
    uint64_t _recvd_bytes;
    //used for flow control in sender CC mode. 
    //decides whether to reduce cwnd at sender; will change dynamically based on receiver resource availability. 
    uint8_t _rcv_cwnd_pen;

    Trigger* _end_trigger;
    ModularVector<uint8_t, uecMaxInFlightPkts>
        _dsn_rx_bitmap;  // bitmap of out-of-order DSNs received above a hole

    uint32_t _out_of_order_count;
    bool _ack_request;

    uint16_t _entropy;

    Stats _stats;
    string _nodename;
};

#endif  // SING_SINK_H
