// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_PFC_ENGINE_H
#define SING_SWITCH_PFC_ENGINE_H

#include <vector>
#include <cstdint>

class PFCEngine {
public:
    PFCEngine(int num_ports, int num_prios);

    void setPfcEnabledMask(uint8_t mask);
    bool isPfcEnabled(int prio) const;
    uint8_t pfcEnabledMask() const { return _pfc_enabled_mask; }

    bool checkXoff(int ingress_port, int prio, bool pause_event);
    bool checkXon(int ingress_port, int prio, bool resume_event);

    void onPfcFrameReceived(int egress_port, int prio, bool pause);
    bool isPausedByDownstream(int egress_port, int prio) const;
    uint32_t getPausedMask(int egress_port) const;

    void setThresholds(int port, int prio, uint64_t xoff, uint64_t xon);
    uint64_t xoffThreshold(int port, int prio) const;

    void addPort();

    int numPorts() const { return _num_ports; }

private:
    uint8_t _pfc_enabled_mask;
    int _num_ports;
    int _num_prios;

    std::vector<std::vector<bool>> _xoff_sent;             // [port][prio]
    std::vector<std::vector<bool>> _paused_by_downstream;  // [port][prio]
    std::vector<std::vector<uint64_t>> _xoff_threshold;    // [port][prio]
    std::vector<std::vector<uint64_t>> _xon_threshold;     // [port][prio]
};

#endif
