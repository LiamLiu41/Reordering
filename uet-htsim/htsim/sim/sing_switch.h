// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_H
#define SING_SWITCH_H

#include "switch.h"
#include "callback_pipe.h"
#include "sing_switch_control_plane.h"
#include "sing_switch_buffer_manager.h"
#include "sing_switch_output_port.h"
#include "sing_switch_pfc_engine.h"
#include <unordered_map>

class FatTreeTopology;
class Pipe;

class SingSwitch : public Switch {
public:
    SingSwitch(EventList& eventlist, const std::string& name,
               FatTreeSwitch::switch_type type, uint32_t id,
               simtime_picosec switch_delay,
               FatTreeTopology* ft,
               uint64_t buffer_capacity, int num_prios = 8);
    ~SingSwitch() override;

    void receivePacket(Packet& pkt) override;
    void doNextEvent() override {}
    void addHostPort(int addr, int flowid, PacketSink* transport) override;

    int addOutputPort(linkspeed_bps speed, Pipe* egress_pipe,
                      Pipe* ingress_pipe = nullptr, PacketSink* remote = nullptr);
    bool configureDynamicPfc(double pfc_alpha, uint64_t pfc_resume_offset,
                             uint64_t headroom_override = 0);

    void onEgressRelease(int ingress_port, int prio);

    uint64_t queueSize(int port, int prio) const;
    SingSwitchQueue& queue(int port, int prio);
    const SingSwitchQueue& queue(int port, int prio) const;
    void bindQueueLogger(int port, int prio, QueueLogger* logger,
                         const std::string& queue_name);
    uint32_t getType() override { return _type; }

    int numOutputPorts() const { return (int)_output_ports.size(); }
    BufferManager& bufferManager() { return _buffer_mgr; }
    PFCEngine& pfcEngine() { return _pfc_engine; }
    static void setPfcStatsLogging(bool enabled) { _log_pfc_stats = enabled; }
    static bool pfcStatsLoggingEnabled() { return _log_pfc_stats; }

private:
    void sendPfcPause(int ingress_port, int prio, bool pause);
    int resolveIngressPort(Packet& pkt) const;
    uint64_t oneHopBdpBytes(int port) const;

    FatTreeSwitch::switch_type _type;
    int _num_prios;

    ControlPlane _control_plane;
    BufferManager _buffer_mgr;
    PFCEngine _pfc_engine;
    std::vector<OutputPort*> _output_ports;
    CallbackPipe* _pipe;

    std::vector<Pipe*> _egress_pipes;
    std::vector<linkspeed_bps> _egress_speeds;
    std::vector<Route*> _pause_routes;
    std::unordered_map<PacketSink*, int> _ingress_pipe_to_port;

    struct IngressInfo {
        int out_port;
        int priority;
        bool ecn_mark;
        int ingress_port;
    };
    std::unordered_map<Packet*, IngressInfo> _pending_packets;

    uint64_t _pfc_xoff_sent = 0;
    uint64_t _pfc_xon_sent = 0;
    uint64_t _pfc_pause_rcvd = 0;
    static bool _log_pfc_stats;
};

#endif
