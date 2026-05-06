// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_MP_H
#define SING_MP_H

#include <list>
#include <optional>
#include "eventlist.h"
#include "buffer_reps.h"

class SingMultipath {
public:
    enum PathFeedback {PATH_GOOD, PATH_ECN, PATH_NACK, PATH_TIMEOUT};
    enum EvDefaults {UNKNOWN_EV};
    enum MpType {OBLIVIOUS, BITMAP, REPS, REPS_LEGACY, MIXED};
    enum IntraGranularityBehavior { KEEP_PATH, RANDOM_PATH };

    struct PathSelectionResult {
        uint16_t path_id;
        bool path_sel_end;
    };
    
    SingMultipath(bool debug): _debug(debug), _debug_tag(""), _path_selection_granularity(0),
                               _intra_granularity_behavior(KEEP_PATH),
                               _bytes_sent_since_last_path_update(0),
                               _current_pathid(0), _path_initialized(false) {};
    virtual ~SingMultipath() {};
    virtual void set_debug_tag(string debug_tag) { _debug_tag = debug_tag; };
    
    /**
     * @return MpType The type of multipath algorithm
     */
    virtual MpType getType() const = 0;
    
    /**
     * @param uint16_t path_id The path ID/entropy value as received by ACK/NACK
     * @param PathFeedback path_id The ACK/NACK response
     */
    virtual void processEv(uint16_t path_id, PathFeedback feedback) = 0;
    /**
     * @param uint64_t seq_sent The sequence number to be sent
     * @param uint64_t cur_cwnd_in_pkts The current congestion window in packets.
     */
    virtual uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) = 0;
    
    /**
     * @return uint16_t A purely random path ID (for intra-granularity random behavior)
     */
    virtual uint16_t randomEntropy() = 0;

    // Configure path selection granularity behavior.
    void setPathSelectionPolicy(mem_b granularity, IntraGranularityBehavior behavior);
    // Unified path selection used by single-subflow senders.
    PathSelectionResult selectPathForPacket(mem_b pkt_size, uint64_t seq_sent,
                                            uint64_t cur_cwnd_in_pkts);
protected:
    bool _debug;
    string _debug_tag;

private:
    mem_b _path_selection_granularity;
    IntraGranularityBehavior _intra_granularity_behavior;
    mem_b _bytes_sent_since_last_path_update;
    uint16_t _current_pathid;
    bool _path_initialized;
};

class SingMpOblivious : public SingMultipath {
public:
    SingMpOblivious(uint16_t no_of_paths, bool debug);
    MpType getType() const override { return OBLIVIOUS; }
    void processEv(uint16_t path_id, PathFeedback feedback) override;
    uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) override;
    uint16_t randomEntropy() override;
private:
    uint16_t _no_of_paths;       // must be a power of 2
    uint16_t _path_random;       // random upper bits of EV, set at startup and never changed
    uint16_t _path_xor;          // random value set each time we wrap the entropy values - XOR with
                                 // _current_ev_index
    uint16_t _current_ev_index;  // count through _no_of_paths and then wrap.  XOR with _path_xor to
};

class SingMpBitmap : public SingMultipath {
public:
    SingMpBitmap(uint16_t no_of_paths, bool debug, uint8_t ecn_penalty = 4);
    MpType getType() const override { return BITMAP; }
    void processEv(uint16_t path_id, PathFeedback feedback) override;
    uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) override;
    uint16_t randomEntropy() override;
private:
    uint16_t _no_of_paths;       // must be a power of 2
    uint16_t _path_random;       // random upper bits of EV, set at startup and never changed
    uint16_t _path_xor;          // random value set each time we wrap the entropy values - XOR with
                                 // _current_ev_index
    uint16_t _current_ev_index;  // count through _no_of_paths and then wrap.  XOR with _path_xor to
    vector<uint8_t> _ev_skip_bitmap;  // paths scores for load balancing

    uint16_t _ev_skip_count;
    uint8_t _max_penalty;             // max value we allow in _path_penalties (typically 1 or 2).
    uint8_t _ecn_penalty;             // penalty value for ECN feedback
};

class SingMpRepsLegacy : public SingMultipath {
public:
    SingMpRepsLegacy(uint16_t no_of_paths, bool debug);
    MpType getType() const override { return REPS_LEGACY; }
    void processEv(uint16_t path_id, PathFeedback feedback) override;
    uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) override;
    uint16_t randomEntropy() override;
    optional<uint16_t> nextEntropyRecycle();
private:
    uint16_t _no_of_paths;
    uint16_t _crt_path;
    list<uint16_t> _next_pathid;
};


class SingMpReps : public SingMultipath {
public:
    SingMpReps(uint16_t no_of_paths, bool debug, bool is_trimming_enabled);
    MpType getType() const override { return REPS; }
    void processEv(uint16_t path_id, PathFeedback feedback) override;
    uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) override;
    uint16_t randomEntropy() override;
private:
    uint16_t _no_of_paths;
    CircularBufferREPS<uint16_t> *circular_buffer_reps;
    uint16_t _crt_path;
    list<uint16_t> _next_pathid;
    bool _is_trimming_enabled = true;  // whether to trim the circular buffer
};

class SingMpMixed : public SingMultipath {
public:
    SingMpMixed(uint16_t no_of_paths, bool debug, uint8_t ecn_penalty = 4);
    MpType getType() const override { return MIXED; }
    void processEv(uint16_t path_id, PathFeedback feedback) override;
    uint16_t nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) override;
    uint16_t randomEntropy() override;
    void set_debug_tag(string debug_tag) override;
private:
    SingMpBitmap _bitmap;
    SingMpRepsLegacy _reps_legacy;
};


#endif  // SING_MP_H
