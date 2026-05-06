// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_BUFFER_MANAGER_H
#define SING_SWITCH_BUFFER_MANAGER_H

#include <vector>
#include <cstdint>
#include "config.h"

class PFCEngine;

struct AdmitResult {
    bool admitted;
    bool ecn_mark;
    bool pfc_trigger;
};

class BufferManager {
public:
    BufferManager(uint64_t total_capacity, int num_ports, int num_prios);

    AdmitResult admit(int egress_port, int prio, mem_b pkt_size, int ingress_port);
    void release(int egress_port, int prio, mem_b pkt_size, int ingress_port);
    bool shouldSendResume(int ingress_port, int prio) const;

    uint64_t egressUsed(int port, int prio) const;
    uint64_t egressUsedTotal(int port) const;
    uint64_t totalUsed() const { return _total_used; }
    uint64_t remaining() const { return _total_capacity - _total_used; }

    uint64_t ingressUsed(int port, int prio) const;
    uint64_t ingressSharedUsed(int port, int prio) const;
    uint64_t ingressHeadroomUsed(int port, int prio) const;
    uint64_t ingressHeadroomCap(int port, int prio) const;
    uint64_t totalSharedUsed() const { return _total_shared_used; }
    uint64_t totalHeadroom() const { return _total_headroom; }
    uint64_t sharedCapacity() const { return _shared_capacity; }
    uint64_t dynamicPauseThreshold() const;
    uint64_t dynamicResumeThreshold() const;

    void addPort();
    void setHeadroomCap(int port, int prio, uint64_t cap_bytes);
    void setEcnThreshold(int port, int prio, uint64_t min_thresh, uint64_t max_thresh);
    void configureSharedBuffer(uint64_t total_headroom, double pfc_alpha,
                               uint64_t pfc_resume_offset);

    void setPfcEngine(PFCEngine* engine) { _pfc_engine = engine; }

    int numPorts() const { return _num_ports; }
    int numPrios() const { return _num_prios; }
    uint64_t totalCapacity() const { return _total_capacity; }
    uint64_t totalEcnMarks() const { return _total_ecn_marks; }
    uint64_t totalDrops() const { return _total_drops; }
    uint64_t dropNoPfcOverPause() const { return _drop_no_pfc_over_pause; }
    uint64_t dropHeadroomExhausted() const { return _drop_headroom_exhausted; }

private:
    uint64_t currentFreeSpace() const;
    uint64_t computePauseThreshold(uint64_t free_space) const;
    uint64_t computeResumeThreshold(uint64_t free_space) const;

    uint64_t _total_capacity;
    uint64_t _total_used;
    uint64_t _total_shared_used;
    uint64_t _total_headroom;
    uint64_t _shared_capacity;
    int _num_ports;
    int _num_prios;

    std::vector<std::vector<uint64_t>> _egress_used;       // [port][prio]
    std::vector<std::vector<uint64_t>> _ingress_shared_used;   // [port][prio]
    std::vector<std::vector<uint64_t>> _ingress_headroom_used; // [port][prio]
    std::vector<std::vector<uint64_t>> _ingress_headroom_cap;  // [port][prio]
    std::vector<std::vector<uint64_t>> _ecn_min_thresh;     // [port][prio]
    std::vector<std::vector<uint64_t>> _ecn_max_thresh;     // [port][prio]

    double _pfc_alpha;
    uint64_t _pfc_resume_offset;

    PFCEngine* _pfc_engine;

    uint64_t _total_drops;
    uint64_t _drop_no_pfc_over_pause;
    uint64_t _drop_headroom_exhausted;
    uint64_t _total_ecn_marks;
};

#endif
