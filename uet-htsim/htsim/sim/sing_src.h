// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SRC_H
#define SING_SRC_H

#include <memory>
#include <list>
#include <set>
#include <optional>
#include <map>
#include <utility>

#include "sing_base.h"
#include "eventlist.h"
#include "trigger.h"
#include "singpacket.h"
#include "circular_buffer.h"
#include "sing_mp.h"
#include "sing_cc.h"
#include "sing_subflow.h"
#include "sing_nic.h"

#define timeInf 0
// min RTO bound in us
//  *** don't change this default - override it by calling SingSrc::setMinRTO()
#define DEFAULT_UEC_RTO_MIN 100

class SingSrc;
class SingSink;
class SingLogger;
// Packets are received on ports, but then passed to the Src for handling
class SingSrcPort : public PacketSink {
public:
    SingSrcPort(SingSrc& src, uint32_t portnum);
    void setRoute(const Route& route);
    inline const Route* route() const {return _route;}
    virtual void receivePacket(Packet& pkt);
    virtual const string& nodename();
private:
    SingSrc& _src;
    uint8_t _port_num;
    const Route* _route;  // we're only going to support ECMP_HOST for now.
};

class SingSrc : public EventSource, public TriggerTarget, public SingTransportConnection {
    friend class SingSubflow;
public:
    struct Stats {
        /* all must be non-negative, but we'll make them signed so we
           can do maths with them without concern about underflow */
        int32_t new_pkts_sent;
        int32_t rtx_pkts_sent;
        int32_t rto_events;
        int32_t acks_received;
        int32_t nacks_received;
        int32_t last_hop_nacks_received;  // NACKs caused by last hop trimming
        int32_t cnps_received;
        int32_t cnps_invalid_subflow;
        int32_t bounces_received;
        int32_t _sleek_counter;
    };
    struct NsccStats {
        mem_b inc_fair_bytes;
        mem_b inc_prop_bytes;
        mem_b inc_fast_bytes;
        mem_b inc_eta_bytes;
        mem_b dec_multi_bytes;
        mem_b dec_quick_bytes;
        mem_b dec_nack_bytes;
    };
    SingSrc(TrafficLogger* trafficLogger, 
           EventList& eventList, 
           unique_ptr<SingMultipath> mp,
           SingNIC& nic, 
           uint32_t no_of_ports);
    void delFromSendTimes(simtime_picosec time, SingDataPacket::seq_t seq_no);
    // Initialize global sender-based CC defaults.
    static void initCcGlobalDefaults(simtime_picosec network_rtt, mem_b network_bdp,
                                     linkspeed_bps linkspeed, bool trimming_enabled);
    // Initialize per-connection sender-based CC parameters.
    // In Pathwise mode (_num_pathwise_subflows > 1), skips subflow creation;
    // caller must follow up with createPathwiseSubflows().
    void initCcForFlow(const FlowBasicParams& params);
    void logFlowEvents(FlowEventLogger& flow_logger) { _flow_logger = &flow_logger; }
    virtual void connectPort(uint32_t portnum, Route& routeout, Route& routeback, SingSink& sink, simtime_picosec start);
    const Route* getPortRoute(uint32_t port_num) const {return _ports[port_num]->route();}
    SingSrcPort* getPort(uint32_t port_num) {return _ports[port_num];}
    void receivePacket(Packet& pkt, uint32_t portnum);
    void doNextEvent();
    uint32_t dst() { return _dstaddr; }
    void setDst(uint32_t dst) { _dstaddr = dst; }
    
    // New scheduling interfaces
    bool hasRtxForSubflow(const SingSubflow& sf) const;
    bool hasNewDataBacklog() const;
    bool canSendNewData() const;
    simtime_picosec computeNextSendTime(mem_b pkt_size) const;
    mem_b sendPacketFromNIC(const Route& route);
    simtime_picosec getBaseRTT() const { return _base_rtt; }
    BaseCC* cc() const;
    SingSubflow* defaultSubflow() const { return _default_subflow; }
    const std::vector<std::unique_ptr<SingSubflow>>& subflows() const { return _subflows; }
    int numSubflows() const { return (int)_subflows.size(); }
    void createPathwiseSubflows(int num_subflows, const FlowBasicParams& params);
    mem_b currentWindowBytes() const;
    simtime_picosec currentNsccAvgDelay() const;

    static int _num_pathwise_subflows;

    // Functions from SingTransportConnection
    virtual void continueConnection() override;
    virtual void startConnection() override;
    virtual bool hasStarted() override;
    virtual bool isActivelySending() override;
    virtual void makeReusable(SingMsgTracker* conn_reuse_tracker) override { _msg_tracker.emplace(conn_reuse_tracker); };
    virtual void addToBacklog(mem_b size) override;

    static void setMinRTO(uint32_t min_rto_in_us) {
        _min_rto = timeFromUs((uint32_t)min_rto_in_us);
    }
    void setMaxWnd(mem_b maxwnd) {
        //_maxwnd = cwnd;
        _maxwnd = maxwnd;
    }

    void setConfiguredMaxWnd(mem_b wnd){
        _configured_maxwnd = wnd;
    }

    mem_b configuredMaxWnd() const { return _configured_maxwnd; }
    static void setDefaultMaxUnackWindow(mem_b max_unack_window_bytes) {
        _default_max_unack_window = max_unack_window_bytes;
    }
    /*
     If a PDC is used, call the same function there. Checks if _all_ msgs are done,
     include the ones in the various backlog queues.
     Otherwise, it just returns the status of _done_sending.
    */
    bool isTotallyFinished();

    const Stats& stats() const { return _stats; }

    void setEndTrigger(Trigger& trigger);
    // called from a trigger to start the flow.
    virtual void activate();
    static int _global_node_count;
    static simtime_picosec _min_rto;
    static uint16_t _hdr_size;
    static uint16_t _mss;  // does not include header
    static uint16_t _mtu;  // does include header

    static bool _sender_based_cc;

    using Sender_CC = CcAlgoId;
    static constexpr Sender_CC DCTCP = CcAlgoId::DCTCP;
    static constexpr Sender_CC NSCC = CcAlgoId::NSCC;
    static constexpr Sender_CC CONSTANT = CcAlgoId::CONSTANT;
    static constexpr Sender_CC SWIFT = CcAlgoId::SWIFT;
    static constexpr Sender_CC BARRE = CcAlgoId::BARRE;
    static Sender_CC _sender_cc_algo;
    void setFlowCcSelection(Sender_CC algo, std::optional<CcProfile> profile = std::nullopt);

    static bool _enable_sleek;
    static bool _dump_cc_params;

    virtual const string& nodename() { return _nodename; }
    virtual void setName(const string& name) override { _name=name; _mp->set_debug_tag(name); }
    inline void setFlowId(flowid_t flow_id) { _flow.set_flowid(flow_id); }
    void setFlowsize(uint64_t flow_size_in_bytes);
    mem_b flowsize() { return _flow_size; }
    inline void setIdealTime(simtime_picosec ideal_time) { _ideal_time = ideal_time; }
    inline simtime_picosec idealTime() const { return _ideal_time; }
    inline void setOneWayIdealTime(simtime_picosec one_way_ideal_time) { _one_way_ideal_time = one_way_ideal_time; }
    inline simtime_picosec oneWayIdealTime() const { return _one_way_ideal_time; }
    inline void setFlowType(const string& type) { _flow_type = type; }
    inline const string& flowType() const { return _flow_type; }
    inline simtime_picosec flowStartTime() const { return _flow_start_time; }
    inline PacketFlow* flow() { return &_flow; }
    optional<SingMsgTracker*> msg_tracker() { return _msg_tracker; };

    enum class AckPathFeedbackMode { LEGACY, REPS_ACKAGG, BITMAP_ACKAGG };
    inline flowid_t flowId() const { return _flow.flow_id(); }
    inline void setAckPathFeedbackMode(AckPathFeedbackMode mode) { _ack_path_feedback_mode = mode; }
    inline AckPathFeedbackMode ackPathFeedbackMode() const { return _ack_path_feedback_mode; }
    inline void setRepsAckAggMode(bool enabled) {
        _ack_path_feedback_mode = enabled ? AckPathFeedbackMode::REPS_ACKAGG
                                          : AckPathFeedbackMode::LEGACY;
    }
    inline bool repsAckAggMode() const {
        return _ack_path_feedback_mode == AckPathFeedbackMode::REPS_ACKAGG;
    }

    static bool _debug;
    static bool _shown;
    static bool _print_receiver_fct;  // Control whether to print receiver-side FCT logs
    bool _debug_src;
    bool debug() const { return _debug_src; }

private:
    // Build a CC instance for the configured algo; used by initCcForFlow and createPathwiseSubflows.
    std::unique_ptr<BaseCC> buildCcInstance(const FlowBasicParams& params,
                                            const CcProfile& profile,
                                            Sender_CC algo);
    Sender_CC resolveSenderAlgoForFlow() const;
    CcProfile resolveCcProfileForFlow() const;

    std::vector<std::unique_ptr<SingSubflow>> _subflows;
    SingSubflow* _default_subflow = nullptr;
    unique_ptr<SingMultipath> _mp;
    SingNIC& _nic;
    uint32_t _no_of_ports;
    vector <SingSrcPort*> _ports;
    enum class TxState { IN_FLIGHT, PENDING_RTX };
    struct sendRecord {
        mem_b pkt_size = 0;
        simtime_picosec send_time = 0;
        SingBasePacket::seq_t ssn = 0;
        uint16_t owner_subflow_id = 0;
        TxState state = TxState::IN_FLIGHT;
    };
    SingLogger* _logger;
    FlowEventLogger* _flow_logger;
    Trigger* _end_trigger;

    // TODO in-flight packet storage - acks and sacks clear it
    // list<SingDataPacket*> _activePackets;

    // we need to access the in_flight packet list quickly by sequence number, or by send time.
    map<SingDataPacket::seq_t, sendRecord> _tx_bitmap;
    map<pair<uint16_t, SingBasePacket::seq_t>, SingDataPacket::seq_t> _ssn_to_dsn;
    multimap<simtime_picosec, SingDataPacket::seq_t> _send_times;
    map<SingDataPacket::seq_t, uint16_t> _rtx_times;
    size_t _pending_rtx_pkts = 0;
    struct PreparedDataPacket {
        SingDataPacket* pkt = nullptr;
        SingBasePacket::seq_t ssn = 0;
        uint16_t subflow_id = 0;
        uint16_t ev = 0;
        bool path_sel_end = false;
    };
    PreparedDataPacket prepareDataPacketForSend(const Route& route,
                                                SingBasePacket::seq_t dsn,
                                                mem_b pkt_size,
                                                bool is_rtx,
                                                SingSubflow* sf,
                                                optional<SingBasePacket::seq_t> forced_ssn = {});
    mem_b sendNewPacket(const Route& route, SingSubflow* sf = nullptr);
    mem_b sendRtxPacket(const Route& route, SingSubflow* sf = nullptr);
    mem_b sendPacketForSubflow(const Route& route, SingSubflow& sf);
    void createSendRecord(SingDataPacket::seq_t seqno,
                          mem_b pkt_size,
                          SingBasePacket::seq_t ssn,
                          uint16_t owner_subflow_id);
    bool markDsnForRtx(SingBasePacket::seq_t seqno);
    void eraseSendRecord(SingDataPacket::seq_t seqno);
    bool hasPendingRtx() const { return _pending_rtx_pkts > 0; }
    bool isPendingRtx(SingBasePacket::seq_t seqno) const;
    void recalculateRTO();
    void startRTO(simtime_picosec send_time);
    void clearRTO();   // timer just expired, clear the state
    void cancelRTO();  // cancel running timer and clear state

    // not used, except for debugging timer issues
    void checkRTO() {
        if (_rtx_timeout_pending)
            assert(_rto_timer_handle != eventlist().nullHandle());
        else
            assert(_rto_timer_handle == eventlist().nullHandle());
    }

    void rtxTimerExpired();
    mem_b handleAckno(SingDataPacket::seq_t ackno);
    mem_b handleCumulativeAck(SingDataPacket::seq_t next_expected_dsn);
    void processAck(const SingAck& pkt);
    void processNack(const SingNack& pkt);
    void processCnp(const SingCnp& pkt);
    void runSleek(uint32_t ooo, SingBasePacket::seq_t cum_ack);
    uint64_t updateAckRecvdState(const SingAck& pkt);
    simtime_picosec computeAckRawRttSample(const SingAck& pkt);
    void applyAckAccounting(const SingAck& pkt);
    void processAckPathFeedback(const SingAck& pkt);
    void dispatchAckToSubflow(const SingAck& pkt, simtime_picosec raw_rtt,
                              uint64_t newly_recvd_bytes);
    
public:
    mem_b getNextPacketSize();  // Made public for SingNIC scheduler
    mem_b getNextPacketSizeForSubflow(const SingSubflow& sf) const;
    
private:
    bool checkFinished(SingDataPacket::seq_t cum_ack);
    void emitFlowCompletionLog(SingDataPacket::seq_t cum_ack, uint32_t total_messages);

    Stats _stats;
    // Stats over the whole connection lifetime
    NsccStats _nscc_overall_stats;
    SingSink* _sink;

    // unlike in the NDP simulator, we maintain all the main quantities in bytes
    mem_b _flow_size;
    bool _done_sending;  // make sure we only trigger once
    optional<SingMsgTracker*> _msg_tracker;  
    mem_b _backlog;      // how much we need to send, not including retransmissions
    mem_b _maxwnd;
    static mem_b _configured_maxwnd;
    static mem_b _default_max_unack_window;
    SingDataPacket::seq_t _highest_sent;
    SingDataPacket::seq_t _highest_rtx_sent;
    mem_b _in_flight;
    mem_b _bdp;
    
    // Max unacknowledged window (for future extension)
    mem_b _max_unack_window;

    // Record last time this SingSrc was scheduled.
    optional<simtime_picosec> _last_event_time;
    // Record flow start time (set once in startConnection)
    simtime_picosec _flow_start_time;
    // Record ideal completion time (size/bw + rtt)
    simtime_picosec _ideal_time;
    // Record one-way ideal time (size/bw + one_way_latency) for receiver perspective
    simtime_picosec _one_way_ideal_time;
public:
    static GlobalNetworkParams _global_network_params;
    //debug
    static flowid_t _debug_flowid;
    FlowBasicParams buildFlowBasicParams(simtime_picosec peer_rtt, mem_b bdp_bytes) const;
private:
    uint16_t get_avg_pktsize();
    // RTT estimate data for RTO and sender based CC.
    bool _rtx_timeout_pending;       // is the RTO running?
    simtime_picosec _rto_send_time;  // when we sent the oldest packet that the RTO is waiting on.
    simtime_picosec _rtx_timeout;    // when the RTO is currently set to expire
    EventList::Handle _rto_timer_handle;


    //used to drive ACK clock
    uint64_t _recvd_bytes;

    // Smarttrack sender based CC variables.
    simtime_picosec _base_rtt;
    mem_b _base_bdp;

    uint32_t _highest_recv_seqno;

    /******** SLEEK parameters *********/

    static float loss_retx_factor;
    static int min_retx_config ;
    bool _loss_recovery_mode = false;
    uint32_t _recovery_seqno = 0;
    /******** END SLEEK parameters *********/

    // Connectivity
    PacketFlow _flow;
    string _nodename;
    string _flow_type;  // flow type for classification (e.g., "query", "background", etc.)
    int _node_num;
    uint32_t _dstaddr;
    AckPathFeedbackMode _ack_path_feedback_mode = AckPathFeedbackMode::LEGACY;
    std::optional<Sender_CC> _flow_sender_cc_algo;
    std::optional<CcProfile> _flow_cc_profile;
    simtime_picosec _flow_cnp_min_interval = timeFromUs((uint32_t)2);
};


#endif  // SING_SRC_H
