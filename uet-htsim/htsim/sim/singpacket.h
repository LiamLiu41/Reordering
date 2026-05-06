// -*- c-basic-offset: 4; indent-tabs-mode: nil -*- 
#ifndef SINGPACKET_H
#define SINGPACKET_H

#include <cstdint>
#include <list>
#include <optional>
#include <utility>
#include <vector>
#include "network.h"
#include "ecn.h"

// All UEC packets are subclasses of Packet.
// They incorporate a packet database, to reuse packet objects that are no longer needed.
// Note: you never construct a new UEC packet directly; 
// rather you use the static method newpkt() which knows to reuse old packets from the database.

#define VALUE_NOT_SET -1

class SingBasePacket : public Packet {
public:
    typedef uint64_t seq_t;
  
    uint16_t _eqsrcid;  // source tunnel ID for the source.
    uint16_t _eqtgtid;  // destination tunnel ID. 
    const static int ACKSIZE=64;
    static mem_b get_ack_size() {return ACKSIZE;}
};

class SingDataPacket : public SingBasePacket {
    //using Packet::set_route;
public:
    //typedef enum {_500B,_1KB,_2KB,_4KB} packet_size;   // need to handle arbitrary packet sizes at end of messages

    inline static SingDataPacket* newpkt(PacketFlow &flow, const Route& route, 
                                         seq_t dsn, mem_b full_size, 
                                         bool is_rtx,
                                         uint32_t destination = UINT32_MAX) {
        SingDataPacket* p = _packetdb.allocPacket();
        // Packet::_id is used for packet tracking/logging. In SING this carries DSN.
        p->set_route(flow, route, full_size, dsn);  // also sets size and seqno
        p->_type = SINGDATA;
        p->_is_header = false;
        p->_bounced = false;
        p->_ssn = 0;
        p->_dsn = dsn;
        p->_subflow_id = 0;
        p->_is_rtx = is_rtx;
        p->_send_ts = 0;
        
        p->_eqsrcid = 0;
        p->_eqtgtid = 0;

        p->_syn = false;
        p->_fin = false;
        
        p->_ar = false;
        p->_path_sel_end = false;
        p->set_dst(destination);

        p->_direction = NONE;
        p->_path_len = route.size();
        p->_trim_hop = {};
        p->_trim_direction = NONE;

        return p;
    }
  
    virtual inline void strip_payload(uint16_t trim_size = ACKSIZE) {
        Packet::strip_payload(trim_size); 
        _trim_hop = _nexthop;
        _trim_direction = _direction;
    };

    virtual inline void set_route(const Route &route) {
        if (_trim_hop.has_value())
            _trim_hop = *_trim_hop - route.size();

        Packet::set_route(route);
    }

    virtual inline void set_route(PacketFlow& flow, const Route &route, int pkt_size, packetid_t id){
        if (_trim_hop.has_value())
            _trim_hop = *_trim_hop - route.size();

        Packet::set_route(flow,route,pkt_size,id);
    };


    void free() {set_pathid(UINT32_MAX),  _packetdb.freePacket(this);}
    virtual ~SingDataPacket(){}

    inline seq_t ssn() const {return _ssn;}
    inline seq_t dsn() const {return _dsn;}
    inline uint16_t subflow_id() const {return _subflow_id;}
    inline void set_ssn(seq_t ssn) {_ssn = ssn;}
    inline void set_dsn(seq_t dsn) {_dsn = dsn;}
    inline void set_subflow_id(uint16_t id) {_subflow_id = id;}

    inline bool retransmitted() const {return _is_rtx;}
    inline bool is_rtx() const {return _is_rtx;}
    inline void set_ar(bool ar){ _ar = ar;}
    inline simtime_picosec send_ts() const { return _send_ts; }
    inline void set_send_ts(simtime_picosec ts) { _send_ts = ts; }

    inline bool ar() const {return _ar;}
    inline bool syn() const {return _syn;}
    inline bool fin() const {return _fin;}
    
    inline void set_path_sel_end(bool pse) { _path_sel_end = pse;}
    inline bool path_sel_end() const {return _path_sel_end;}

    inline int32_t trim_hop() const {return _trim_hop.value_or(INT32_MAX);}
    inline packet_direction trim_direction() const {return _trim_direction;}

    inline int32_t path_id() const {if (_pathid!=UINT32_MAX) return _pathid; else return _route->path_id();}

    virtual PktPriority priority() const {
        if (_is_header) {
            return Packet::PRIO_HI;
        } else {
            return PRIO_MID;
        }
    }

protected:
    seq_t _ssn;
    seq_t _dsn = 0;
    uint16_t _subflow_id = 0;

    bool _ar;
    bool _syn;
    bool _fin;
    bool _path_sel_end;

    // SING data packets only carry the RTX-vs-new distinction.
    bool _is_rtx = false;
    simtime_picosec _send_ts = 0;

    //trim information, need to see if this stays here or goes to separate header.
    std::optional<int32_t> _trim_hop;
    packet_direction _trim_direction;
    static PacketDB<SingDataPacket> _packetdb;
};

class SingAck : public SingBasePacket {
    using Packet::set_route;
public:
    inline static SingAck* newpkt(PacketFlow &flow, const Route *route,
                                        seq_t next_expected_ssn, seq_t next_expected_dsn, uint16_t subflow_id,
                                        seq_t sack_base_dsn, seq_t trigger_dsn, simtime_picosec echo_send_ts,
                                        uint16_t path_id, bool ecn_marked, uint64_t recv_bytes, uint8_t rcv_wnd_pen,
                                        uint32_t destination = UINT32_MAX) {
        SingAck* p = _packetdb.allocPacket();
        p->set_attrs(flow, ACKSIZE, 0);
        if (route) {
            // we may want to late-bind the route
            p->set_route();
        }

        assert(p->size()==ACKSIZE);
        p->_type = SINGACK;
        p->_is_header = true;
        p->_bounced = false;
        p->_sack_base_dsn = sack_base_dsn;
        p->_trigger_dsn = trigger_dsn;
        p->_echo_send_ts = echo_send_ts;

        p->_next_expected_ssn = next_expected_ssn;
        p->_next_expected_dsn = next_expected_dsn;
        p->_subflow_id = subflow_id;
        p->_ev = path_id;
        p->set_pathid(path_id);
        p->_direction = NONE;
        p->_sack_bitmap = 0;
        p->_ecn_echo = ecn_marked;
        p->set_dst(destination);

        p->_recvd_bytes = recv_bytes;
        p->_rcv_cwnd_pen = rcv_wnd_pen;
        p->_path_sel_end = false;
        p->_agg_non_ecn_evs.clear();
        p->_agg_ecn_ev_valid = false;
        p->_agg_ecn_ev = 0;
        p->_agg_ecn_evs.clear();
        return p;
    }
  
    void free() {set_pathid(UINT32_MAX), _packetdb.freePacket(this);}
    inline seq_t sack_base_dsn() const {return _sack_base_dsn;}
    inline seq_t trigger_dsn() const {return _trigger_dsn;}
    inline simtime_picosec echo_send_ts() const { return _echo_send_ts; }
    inline void set_echo_send_ts(simtime_picosec ts) { _echo_send_ts = ts; }
    inline seq_t next_expected_ssn() const {return _next_expected_ssn;}
    inline void set_next_expected_ssn(seq_t ssn) {_next_expected_ssn = ssn;}
    inline seq_t next_expected_dsn() const {return _next_expected_dsn;}
    inline uint16_t subflow_id() const {return _subflow_id;}
    inline void set_next_expected_dsn(seq_t dsn) {_next_expected_dsn = dsn;}
    inline void set_subflow_id(uint16_t id) {_subflow_id = id;}
    inline simtime_picosec residency_time() const {return _residency_time;}
    inline uint64_t recvd_bytes() const {return _recvd_bytes;}
    inline uint8_t rcv_wnd_pen() const {return _rcv_cwnd_pen;}
    inline void set_ooo(uint32_t out_of_order_count) { _out_of_order_count = out_of_order_count;}
    inline uint32_t ooo() const {return _out_of_order_count; }

    inline void set_bitmap(uint64_t bitmap){_sack_bitmap = bitmap;};
    uint16_t  ev() const {return _ev;}
    inline void set_ev(uint16_t ev) { _ev = ev; set_pathid(ev); }
    inline bool ecn_echo() const {return _ecn_echo;}
    inline void set_ecn_echo(bool ecn_echo) { _ecn_echo = ecn_echo; }
    uint64_t bitmap() const {return _sack_bitmap;}
    virtual PktPriority priority() const {return Packet::PRIO_HI;}
    
    inline void set_rtx_echo(bool rtx_bit){_rtx_echo = rtx_bit;};
    inline bool rtx_echo() const {return _rtx_echo;}
    
    inline void set_path_sel_end(bool pse) { _path_sel_end = pse;}
    inline bool path_sel_end() const {return _path_sel_end;}
    inline void set_agg_non_ecn_evs(const std::vector<uint16_t>& evs) { _agg_non_ecn_evs = evs; }
    inline void set_agg_non_ecn_evs(std::vector<uint16_t>&& evs) { _agg_non_ecn_evs = std::move(evs); }
    inline const std::vector<uint16_t>& agg_non_ecn_evs() const { return _agg_non_ecn_evs; }
    inline void clear_agg_non_ecn_evs() { _agg_non_ecn_evs.clear(); }
    inline void set_agg_ecn_ev(uint16_t ev) { _agg_ecn_ev = ev; _agg_ecn_ev_valid = true; }
    inline bool agg_ecn_ev_valid() const { return _agg_ecn_ev_valid; }
    inline uint16_t agg_ecn_ev() const { return _agg_ecn_ev; }
    inline void clear_agg_ecn_ev() { _agg_ecn_ev_valid = false; _agg_ecn_ev = 0; }
    inline void set_agg_ecn_evs(const std::vector<uint16_t>& evs) { _agg_ecn_evs = evs; }
    inline void set_agg_ecn_evs(std::vector<uint16_t>&& evs) { _agg_ecn_evs = std::move(evs); }
    inline const std::vector<uint16_t>& agg_ecn_evs() const { return _agg_ecn_evs; }
    inline void clear_agg_ecn_evs() { _agg_ecn_evs.clear(); }

    virtual ~SingAck(){}

protected:
    seq_t _sack_base_dsn;  // base DSN for interpreting the SACK bitmap
    seq_t _trigger_dsn; // DSN of the packet that triggered this ACK
    simtime_picosec _echo_send_ts = 0;
    seq_t _next_expected_ssn;  // next expected SSN on this subflow (exclusive upper bound)
    seq_t _next_expected_dsn = 0;  // next expected DSN globally (exclusive upper bound)
    uint16_t _subflow_id = 0;
    //SACK bitmap here 
    uint64_t _sack_bitmap;
    uint16_t _ev; //path id for the packet that triggered the SACK 

    uint64_t _recvd_bytes;
    uint8_t _rcv_cwnd_pen;

    bool _rnr;
    bool _ecn_echo;
    bool _rtx_echo;
    bool _path_sel_end;
    // ACK aggregation metadata for ack-aggregation modes.
    // This metadata is simulation-only and intentionally does not affect ACK size.
    std::vector<uint16_t> _agg_non_ecn_evs;
    bool _agg_ecn_ev_valid = false;
    uint16_t _agg_ecn_ev = 0;
    std::vector<uint16_t> _agg_ecn_evs;
    simtime_picosec _residency_time;
    uint32_t _out_of_order_count;

    static PacketDB<SingAck> _packetdb;
};

class SingNack : public SingBasePacket {
    using Packet::set_route;
public:
    inline static SingNack* newpkt(PacketFlow &flow, const Route *route, 
                                         seq_t ref_dsn,
                                         uint16_t path_id,uint64_t recv_bytes, uint64_t tbytes,
                                         uint32_t destination = UINT32_MAX) {
        SingNack* p = _packetdb.allocPacket();
        p->set_attrs(flow, ACKSIZE, ref_dsn);
        if (route) {
            // we may want to late-bind the route
            p->set_route();
        }

        assert(p->size()==ACKSIZE);
        p->_type = SINGNACK;
        p->_is_header = true;
        p->_bounced = false;
        p->_ref_dsn = ref_dsn;
        p->_ev = path_id; // used to indicate which path the data packet was trimmed on
        p->set_pathid(path_id);
        p->_ecn_echo = false;
        p->_rnr = false;

        p->_direction = NONE;
        p->_path_len = 0;
        p->set_dst(destination);

        p->_recvd_bytes = recv_bytes;
        p->_target_bytes = tbytes;
        p->_last_hop = false;
        p->_subflow_id = 0;

        return p;
    }
  
    void free() {set_pathid(UINT32_MAX), _packetdb.freePacket(this);}
    inline seq_t ref_dsn() const {return _ref_dsn;}
    uint16_t ev() const {return _ev;}
    inline uint16_t subflow_id() const {return _subflow_id;}
    inline void set_subflow_id(uint16_t id) {_subflow_id = id;}
    inline void set_ecn_echo(bool ecn_echo) {_ecn_echo = ecn_echo;}
    inline bool ecn_echo() const {return _ecn_echo;}
    inline uint64_t recvd_bytes() const {return _recvd_bytes;}
    inline uint64_t target_bytes() const {return _target_bytes;}

    inline void set_last_hop(bool lh){ _last_hop = lh;}
    inline bool last_hop() const { return _last_hop;}
    virtual PktPriority priority() const {return Packet::PRIO_HI;}
  
    virtual ~SingNack(){}

protected:
    seq_t _ref_dsn;
    uint16_t _ev;
    uint16_t _subflow_id = 0;
    uint64_t _recvd_bytes;
    uint64_t _target_bytes;
    bool _last_hop;

    bool _rnr;
    bool _ecn_echo;
    static PacketDB<SingNack> _packetdb;
};

class SingCnp : public SingBasePacket {
    using Packet::set_route;
public:
    inline static SingCnp* newpkt(PacketFlow& flow, const Route* route,
                                  uint16_t subflow_id, uint16_t path_id,
                                  uint32_t destination = UINT32_MAX) {
        SingCnp* p = _packetdb.allocPacket();
        p->set_attrs(flow, ACKSIZE, 0);
        if (route) {
            p->set_route(route);
        }

        assert(p->size() == ACKSIZE);
        p->_type = SINGCNP;
        p->_is_header = true;
        p->_bounced = false;
        p->_subflow_id = subflow_id;
        p->_ev = path_id;
        p->set_pathid(path_id);
        p->_direction = NONE;
        p->set_dst(destination);
        return p;
    }

    void free() { set_pathid(UINT32_MAX), _packetdb.freePacket(this); }
    inline uint16_t subflow_id() const { return _subflow_id; }
    inline uint16_t ev() const { return _ev; }
    inline void set_subflow_id(uint16_t id) { _subflow_id = id; }
    inline void set_ev(uint16_t ev) { _ev = ev; set_pathid(ev); }
    virtual PktPriority priority() const { return Packet::PRIO_HI; }

    virtual ~SingCnp() {}

protected:
    uint16_t _subflow_id = 0;
    uint16_t _ev = 0;
    static PacketDB<SingCnp> _packetdb;
};

#endif
