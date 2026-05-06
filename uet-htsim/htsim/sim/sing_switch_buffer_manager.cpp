// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch_buffer_manager.h"
#include "sing_switch_pfc_engine.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

BufferManager::BufferManager(uint64_t total_capacity, int num_ports, int num_prios)
    : _total_capacity(total_capacity),
      _total_used(0),
      _total_shared_used(0),
      _total_headroom(0),
      _shared_capacity(total_capacity),
      _num_ports(num_ports),
      _num_prios(num_prios),
      _egress_used(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _ingress_shared_used(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _ingress_headroom_used(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _ingress_headroom_cap(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _ecn_min_thresh(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _ecn_max_thresh(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _pfc_alpha(0.125),
      _pfc_resume_offset(0),
      _pfc_engine(nullptr),
      _total_drops(0),
      _drop_no_pfc_over_pause(0),
      _drop_headroom_exhausted(0),
      _total_ecn_marks(0)
{
}

uint64_t BufferManager::currentFreeSpace() const {
    if (_total_shared_used >= _shared_capacity)
        return 0;
    return _shared_capacity - _total_shared_used;
}

uint64_t BufferManager::computePauseThreshold(uint64_t free_space) const {
    long double dyn = _pfc_alpha * static_cast<long double>(free_space);
    uint64_t dyn_u64 = static_cast<uint64_t>(std::floor(dyn));
    return std::max(_pfc_resume_offset, dyn_u64);
}

uint64_t BufferManager::computeResumeThreshold(uint64_t free_space) const {
    long double dyn = _pfc_alpha * static_cast<long double>(free_space);
    uint64_t dyn_u64 = static_cast<uint64_t>(std::floor(dyn));
    if (dyn_u64 <= _pfc_resume_offset)
        return 0;
    return dyn_u64 - _pfc_resume_offset;
}

uint64_t BufferManager::dynamicPauseThreshold() const {
    return computePauseThreshold(currentFreeSpace());
}

uint64_t BufferManager::dynamicResumeThreshold() const {
    return computeResumeThreshold(currentFreeSpace());
}

void BufferManager::configureSharedBuffer(uint64_t total_headroom, double pfc_alpha,
                                          uint64_t pfc_resume_offset) {
    _total_headroom = total_headroom;
    _pfc_alpha = pfc_alpha;
    _pfc_resume_offset = pfc_resume_offset;

    if (_total_headroom >= _total_capacity) {
        std::cerr << "FATAL: invalid shared-buffer configuration: total_headroom="
                  << _total_headroom << " >= total_capacity=" << _total_capacity
                  << std::endl;
        abort();
    }

    _shared_capacity = _total_capacity - _total_headroom;
    if (_total_shared_used > _shared_capacity) {
        std::cerr << "FATAL: existing shared usage exceeds configured shared capacity"
                  << std::endl;
        abort();
    }
}

AdmitResult BufferManager::admit(int egress_port, int prio, mem_b pkt_size, int ingress_port) {
    assert(egress_port >= 0 && egress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    assert(ingress_port >= 0 && ingress_port < _num_ports);
    assert(pkt_size >= 0);

    bool ecn = false;
    bool pfc_trigger = false;

    uint64_t pkt_bytes = static_cast<uint64_t>(pkt_size);
    uint64_t pre_shared = _ingress_shared_used[ingress_port][prio];
    uint64_t free_space = currentFreeSpace();
    uint64_t pause_thresh = computePauseThreshold(free_space);

    uint64_t shared_room_port = 0;
    if (pre_shared < pause_thresh) {
        shared_room_port = pause_thresh - pre_shared;
    }
    uint64_t shared_room = std::min(shared_room_port, free_space);
    uint64_t to_shared = std::min(pkt_bytes, shared_room);

    bool pfc_enabled = _pfc_engine && _pfc_engine->isPfcEnabled(prio);
    bool pause_event = pfc_enabled && ((pkt_bytes > shared_room) || (pre_shared >= pause_thresh));
    pfc_trigger = pause_event;

    // Temporary shared bookkeeping for atomic admission; rolled back on drop.
    _ingress_shared_used[ingress_port][prio] += to_shared;
    _total_shared_used += to_shared;

    uint64_t rem = pkt_bytes - to_shared;
    if (rem > 0) {
        if (!pfc_enabled) {
            _ingress_shared_used[ingress_port][prio] -= to_shared;
            _total_shared_used -= to_shared;
            _total_drops++;
            _drop_no_pfc_over_pause++;
            return {false, false, pfc_trigger};
        }

        uint64_t hcap = _ingress_headroom_cap[ingress_port][prio];
        uint64_t hused = _ingress_headroom_used[ingress_port][prio];
        uint64_t hroom = (hcap > hused) ? (hcap - hused) : 0;
        if (rem > hroom) {
            _ingress_shared_used[ingress_port][prio] -= to_shared;
            _total_shared_used -= to_shared;
            _total_drops++;
            _drop_headroom_exhausted++;
            return {false, false, pfc_trigger};
        }
        _ingress_headroom_used[ingress_port][prio] += rem;
    }

    assert(_total_shared_used <= _shared_capacity);

    _egress_used[egress_port][prio] += pkt_size;
    _total_used += pkt_size;
    assert(_total_used <= _total_capacity);

    uint64_t used = _egress_used[egress_port][prio];
    uint64_t ecn_min = _ecn_min_thresh[egress_port][prio];
    uint64_t ecn_max = _ecn_max_thresh[egress_port][prio];

    if (ecn_max > 0 && ecn_min > 0) {
        if (used > ecn_max) {
            ecn = true;
        } else if (used > ecn_min) {
            uint64_t p = (0x7FFFFFFF * (used - ecn_min)) / (ecn_max - ecn_min);
            if (static_cast<uint64_t>(random()) < p) {
                ecn = true;
            }
        }
    }

    if (ecn)
        _total_ecn_marks++;

    return {true, ecn, pfc_trigger};
}

void BufferManager::release(int egress_port, int prio, mem_b pkt_size, int ingress_port) {
    assert(egress_port >= 0 && egress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    assert(ingress_port >= 0 && ingress_port < _num_ports);
    assert(pkt_size >= 0);
    assert(_egress_used[egress_port][prio] >= static_cast<uint64_t>(pkt_size));
    assert(_total_used >= static_cast<uint64_t>(pkt_size));

    _egress_used[egress_port][prio] -= pkt_size;
    _total_used -= pkt_size;

    uint64_t rem = static_cast<uint64_t>(pkt_size);
    bool pfc_enabled = _pfc_engine && _pfc_engine->isPfcEnabled(prio);
    if (pfc_enabled) {
        uint64_t h = std::min(rem, _ingress_headroom_used[ingress_port][prio]);
        _ingress_headroom_used[ingress_port][prio] -= h;
        rem -= h;
    }

    assert(_ingress_shared_used[ingress_port][prio] >= rem);
    _ingress_shared_used[ingress_port][prio] -= rem;
    assert(_total_shared_used >= rem);
    _total_shared_used -= rem;
    assert(_total_shared_used <= _shared_capacity);
}

bool BufferManager::shouldSendResume(int ingress_port, int prio) const {
    assert(ingress_port >= 0 && ingress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);

    if (!_pfc_engine || !_pfc_engine->isPfcEnabled(prio))
        return false;

    if (_ingress_headroom_used[ingress_port][prio] != 0)
        return false;

    uint64_t resume_thresh = computeResumeThreshold(currentFreeSpace());
    return _ingress_shared_used[ingress_port][prio] < resume_thresh;
}

uint64_t BufferManager::egressUsed(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _egress_used[port][prio];
}

uint64_t BufferManager::egressUsedTotal(int port) const {
    assert(port >= 0 && port < _num_ports);
    uint64_t total = 0;
    for (int q = 0; q < _num_prios; q++)
        total += _egress_used[port][q];
    return total;
}

uint64_t BufferManager::ingressUsed(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _ingress_shared_used[port][prio] + _ingress_headroom_used[port][prio];
}

uint64_t BufferManager::ingressSharedUsed(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _ingress_shared_used[port][prio];
}

uint64_t BufferManager::ingressHeadroomUsed(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _ingress_headroom_used[port][prio];
}

uint64_t BufferManager::ingressHeadroomCap(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _ingress_headroom_cap[port][prio];
}

void BufferManager::addPort() {
    _num_ports++;
    _egress_used.push_back(std::vector<uint64_t>(_num_prios, 0));
    _ingress_shared_used.push_back(std::vector<uint64_t>(_num_prios, 0));
    _ingress_headroom_used.push_back(std::vector<uint64_t>(_num_prios, 0));
    _ingress_headroom_cap.push_back(std::vector<uint64_t>(_num_prios, 0));

    _ecn_min_thresh.push_back(std::vector<uint64_t>(_num_prios, 0));
    _ecn_max_thresh.push_back(std::vector<uint64_t>(_num_prios, 0));
}

void BufferManager::setHeadroomCap(int port, int prio, uint64_t cap_bytes) {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    _ingress_headroom_cap[port][prio] = cap_bytes;
}

void BufferManager::setEcnThreshold(int port, int prio,
                                    uint64_t min_thresh, uint64_t max_thresh) {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    _ecn_min_thresh[port][prio] = min_thresh;
    _ecn_max_thresh[port][prio] = max_thresh;
}
