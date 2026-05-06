// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_PDCSES_H
#define SING_PDCSES_H

#include <list>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <math.h>
#include <array>
#include <optional>

#include "sing_base.h"
#include "eventlist.h"
#include "trigger.h"
#include "singpacket.h"

class SingPdcSes;


/*
* The SingMsg class represents a message. It does work on the payload bytes and sizes.
* It is not concerned with packet headers etc. which is taken care of by the PDC.
*/
class SingMsg : public TriggerTarget {
public:
    typedef uint32_t msgid_t;
    enum MsgStatus {
        Init=0, // Flow exists but has not been started, no packet sent yet.
        SentFirst=1, // Flow has started, i.e. first packet sent
        SentLast=2, // All packets send, not acked, there might be rtx
        RecvdLast=3, // All packets received
        Finished=4, // All packets acked, message is done.
        Count=5
    };
    class StatusCallback {
    public:
        virtual void msg_status_changed(SingPdcSes& pdc, SingMsg::msgid_t msg_id, SingMsg::MsgStatus new_status) = 0;
    };
    struct Stats {
        /* all must be non-negative, but we'll make them signed so we
           can do maths with them without concern about underflow */
        simtime_picosec start_time;
        simtime_picosec end_time;
    };
    SingMsg(SingPdcSes& pdc, msgid_t msg_id, mem_b size, bool debug=false);
    virtual ~SingMsg();
    msgid_t msg_id() {return _msg_id;};
    mem_b size() {return _total_bytes;};
    // Retval:
    // Msg id, size of segment, first packet of message, last packet of message
    pair<mem_b, bool> getNextSegment(SingDataPacket::seq_t seq_no, mem_b mss);
    mem_b addAck(SingDataPacket::seq_t ackno);
    mem_b addRecvd(SingDataPacket::seq_t recvd);
    optional<SingDataPacket::seq_t> getFirstSeqNo() { return _first_seq;};
    optional<SingDataPacket::seq_t> getLastSeqNo() { return _last_seq;};
    /*
    * Add trigger to be called when the specified MsgStatus is reached
    */
    void setTrigger(MsgStatus status, Trigger* trigger) {_triggers.at(status).emplace(trigger);};
    /*
    * Add callback to be called when the specified MsgStatus is reached
    */
    void setStatusCallback(MsgStatus status, StatusCallback* callback) {_callbacks.at(status).emplace(callback);};
    optional<Trigger*> getTrigger(MsgStatus status) {return _triggers.at(status);};
    optional<pair<SingDataPacket::seq_t,SingDataPacket::seq_t>> getSeq();
    /* 
     * Check if the current status is at least status.
     */
    bool status(MsgStatus status);
    bool checkFinished(); // All acked and done

    // Inherited from TriggerTarget
    virtual void activate() override;

    Stats& stats() { return _stats; };
public:  // static
    static bool _output_completion_time;
private:  // Methods
    /* 
     * Set status, fire triggers as needed. Must only be called once per status change.
     */
    void set_status(MsgStatus new_status);
    inline mem_b getRemainingBytes() {return _total_bytes-_sent_bytes;};
private:  // Variables
    bool _debug;
    SingPdcSes& _pdc;
    msgid_t _msg_id;
    MsgStatus _state;
    mem_b _total_bytes;
    mem_b _sent_bytes;
    mem_b _recvd_bytes;
    mem_b _acked_bytes;
    Stats _stats;
    // Once packetized, track the sent and received packets
    // Acked packets are not tracked
    // Order sent -> sent_notrecvd -> sent_notacked
    unordered_set<SingDataPacket::seq_t> _sent_pkt_notrecvd; // sent but not yet received
    unordered_set<SingDataPacket::seq_t> _sent_pkt_notacked; // received but not yet acked
    unordered_set<SingDataPacket::seq_t> _sent_ctrl_notrecvd; // sent but not yet received
    unordered_set<SingDataPacket::seq_t> _sent_ctrl_notacked; // received but not yet acked
    unordered_set<SingDataPacket::seq_t> _ctrl_pkts; // Control packets
    optional<SingDataPacket::seq_t> _first_seq;
    optional<SingDataPacket::seq_t> _last_seq;
    unordered_map<SingDataPacket::seq_t, mem_b> _pkt_size;

    array<optional<Trigger*>, MsgStatus::Count> _triggers;
    array<optional<StatusCallback*>, MsgStatus::Count> _callbacks;
};

class SingPdcSes : public EventSource, public SingMsgTracker {
    friend class SingMsg;
public: // Methods
    SingPdcSes(SingTransportConnection* connection,
              EventList& eventlist,
              mem_b mss, 
              mem_b hdr_size,
              string debug_tag);
    ~SingPdcSes();
    virtual void set_debug_tag(string debug_tag) { _debug_tag = debug_tag; };
    // void setFlow(PacketFlow* flow) {_flow.emplace(flow);};
    /*
    * Add message to backlog.
    * If scheduled_time is set, it is assumed that the message is time-triggered. 
    * If not, it is assumed that the message is event triggered.
    * If the message should be enqueued immediately, scheduled_time should be set to 0.
    * If schedule_event is set to true and scheduled_time is set, an event will be
    * scheduled for the given point in time.
    */
    SingMsg* enque(mem_b size, optional<simtime_picosec> scheduled_time, bool schedule_event = false);

    /*
    * None if the msg has not be completely sent yet.
    * If the message has been sent completely, return
    * start and end seq no of that msg.
    */
    optional<pair<SingDataPacket::seq_t,SingDataPacket::seq_t>> getMsgSeq(SingMsg::msgid_t msg_id);

    mem_b eligiblePktSize();
    flowid_t flow_id();
    SingTransportConnection* connection() { return _connection; };
    SingMsg* getMsg(SingMsg::msgid_t msg_id);
    SingMsg::msgid_t getMsgId(SingDataPacket::seq_t seq_no);
    bool hasScheduledMsg(simtime_picosec time);

    /*
    * This event is only used to make scheduled messages eligible.
    */
    void doNextEvent();
public:  // UecMsgTacker
    virtual mem_b getNextPacket(SingDataPacket::seq_t seq_no) override;
    virtual void notifyCtrlSeqno(SingDataPacket::seq_t seq_no) override;
    virtual void addRecvd(SingDataPacket::seq_t seq_no) override;
    virtual void addCumAck(SingDataPacket::seq_t cum_ack) override;
    virtual void addSAck(SingDataPacket::seq_t ackno) override;
    virtual bool checkDoneSending() override;
    virtual bool checkFinished() override;
    virtual bool isTotallyFinished() override;
    virtual uint32_t getMsgCompleted() override;
public:  // Variables
    static bool _debug; 
private:  // Methods
    inline SingMsg::msgid_t get_next_msg_id();
    /*
    * Check if new messages became eligible on the given timestamp.
    */
    mem_b updateScheduledMsgs(simtime_picosec now);
    mem_b makeMsgEligible(SingMsg* msg);
    void schedule_connection(mem_b new_bytes);
    inline mem_b calc_packeted_size(mem_b size) {
        return ceil(((double)size) / _mss) * _hdr_size + size;
    }
private:  // Variables
    SingTransportConnection* _connection;
    mem_b _mss;
    mem_b _hdr_size;
    string _connection_name;
    SingMsg::msgid_t _next_msg_id;
    mem_b _total_pkt_bytes;
    mem_b _scheduled_pkt_bytes;
    mem_b _triggered_pkt_bytes;
    mem_b _eligible_pkt_bytes;
    mem_b _sent_pkt_bytes;
    mem_b _recvd_pkt_bytes;
    mem_b _acked_pkt_bytes;

    string _debug_tag;

    // Min/max seq number that is currently in flight
    optional<SingDataPacket::seq_t> _max_contiguous_ack;
    // We need these because unfortunately, uec assigns seq_t's to 
    // control packets. So at start time of a message, we cannot
    // know all sequence numbers that we are going to use.
    optional<SingDataPacket::seq_t> _min_seq_no;
    optional<SingDataPacket::seq_t> _max_seq_no;

    // Track messages scheduled but not eligible yet. 
    multimap<simtime_picosec, SingMsg*> _msgs_queue_scheduled;
    // Messages that are triggered by events but have not yet, hence
    // not eligible yet.
    set<SingMsg*> _msgs_queue_triggered;
    // Messages that are eligible to be send or being sent currently
    // but have not been completely sent yet.
    list<SingMsg*> _msgs_queue_eligible;
    // Messages that have been sent, though they might not have been
    // fully acked yet.
    unordered_set<SingMsg*> _msgs_in_flight;
    // Messages that have been completed.
    list<SingMsg*> _msgs_complete;
    // The message currently being sent.
    optional<SingMsg*> _cur_msg;
    unordered_set<SingDataPacket::seq_t> _ctrl_seq;
    map<SingDataPacket::seq_t, SingMsg*> _seq_to_msg;
    // All messages by id.
    unordered_map<SingMsg::msgid_t, SingMsg*> _msgs;

    // Track times for which we have scheduled events
    set<simtime_picosec> _events_scheduled;
};

#endif  // SING_PDCSES_H