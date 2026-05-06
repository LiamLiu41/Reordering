// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch_pfc_engine.h"
#include <cassert>

PFCEngine::PFCEngine(int num_ports, int num_prios)
    : _pfc_enabled_mask(0x03),
      _num_ports(num_ports),
      _num_prios(num_prios),
      _xoff_sent(num_ports, std::vector<bool>(num_prios, false)),
      _paused_by_downstream(num_ports, std::vector<bool>(num_prios, false)),
      _xoff_threshold(num_ports, std::vector<uint64_t>(num_prios, 0)),
      _xon_threshold(num_ports, std::vector<uint64_t>(num_prios, 0))
{
}

void PFCEngine::setPfcEnabledMask(uint8_t mask) {
    _pfc_enabled_mask = mask;
}

bool PFCEngine::isPfcEnabled(int prio) const {
    return (_pfc_enabled_mask >> prio) & 1;
}

bool PFCEngine::checkXoff(int ingress_port, int prio, bool pause_event) {
    assert(ingress_port >= 0 && ingress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);

    if (!isPfcEnabled(prio))
        return false;

    if (pause_event && !_xoff_sent[ingress_port][prio]) {
        _xoff_sent[ingress_port][prio] = true;
        return true;
    }
    return false;
}

bool PFCEngine::checkXon(int ingress_port, int prio, bool resume_event) {
    assert(ingress_port >= 0 && ingress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);

    if (!isPfcEnabled(prio))
        return false;

    if (resume_event && _xoff_sent[ingress_port][prio]) {
        _xoff_sent[ingress_port][prio] = false;
        return true;
    }
    return false;
}

void PFCEngine::onPfcFrameReceived(int egress_port, int prio, bool pause) {
    assert(egress_port >= 0 && egress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    _paused_by_downstream[egress_port][prio] = pause;
}

bool PFCEngine::isPausedByDownstream(int egress_port, int prio) const {
    assert(egress_port >= 0 && egress_port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _paused_by_downstream[egress_port][prio];
}

uint32_t PFCEngine::getPausedMask(int egress_port) const {
    assert(egress_port >= 0 && egress_port < _num_ports);
    uint32_t mask = 0;
    for (int p = 0; p < _num_prios; p++) {
        if (_paused_by_downstream[egress_port][p])
            mask |= (1u << p);
    }
    return mask;
}

void PFCEngine::setThresholds(int port, int prio, uint64_t xoff, uint64_t xon) {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    _xoff_threshold[port][prio] = xoff;
    _xon_threshold[port][prio] = xon;
}

uint64_t PFCEngine::xoffThreshold(int port, int prio) const {
    assert(port >= 0 && port < _num_ports);
    assert(prio >= 0 && prio < _num_prios);
    return _xoff_threshold[port][prio];
}

void PFCEngine::addPort() {
    _num_ports++;
    _xoff_sent.push_back(std::vector<bool>(_num_prios, false));
    _paused_by_downstream.push_back(std::vector<bool>(_num_prios, false));
    _xoff_threshold.push_back(std::vector<uint64_t>(_num_prios, 0));
    _xon_threshold.push_back(std::vector<uint64_t>(_num_prios, 0));
}
