// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch.h"
#include "datacenter/fat_tree_topology.h"
#include "eth_pause_packet.h"
#include "ecn.h"
#include "pipe.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

bool SingSwitch::_log_pfc_stats = false;

SingSwitch::SingSwitch(EventList& eventlist, const std::string& name,
                       FatTreeSwitch::switch_type type, uint32_t id,
                       simtime_picosec switch_delay,
                       FatTreeTopology* ft,
                       uint64_t buffer_capacity, int num_prios)
    : Switch(eventlist, name),
      _type(type),
      _num_prios(num_prios),
      _control_plane(type, id, ft, eventlist),
      _buffer_mgr(buffer_capacity, 0, num_prios),
      _pfc_engine(0, num_prios)
{
    _id = id;
    _pipe = new CallbackPipe(switch_delay, eventlist, this);
    _buffer_mgr.setPfcEngine(&_pfc_engine);
}

SingSwitch::~SingSwitch() {
    if (_log_pfc_stats) {
        std::cout << "SingSwitchStats"
                  << " name=" << _name
                  << " drops=" << _buffer_mgr.totalDrops()
                  << " ecn=" << _buffer_mgr.totalEcnMarks()
                  << " pfc_xoff_sent=" << _pfc_xoff_sent
                  << " pfc_xon_sent=" << _pfc_xon_sent
                  << " pfc_pause_rcvd=" << _pfc_pause_rcvd
                  << std::endl;
    }
    delete _pipe;
    for (auto* op : _output_ports)
        delete op;
    for (auto* r : _pause_routes)
        delete r;
}

int SingSwitch::addOutputPort(linkspeed_bps speed, Pipe* egress_pipe,
                              Pipe* ingress_pipe, PacketSink* remote) {
    int port_idx = (int)_output_ports.size();

    _buffer_mgr.addPort();
    _pfc_engine.addPort();

    OutputPort* op = new OutputPort(port_idx, _num_prios, speed,
                                    eventlist(), &_buffer_mgr);
    op->setPfcEngine(&_pfc_engine);
    op->setSingSwitch(this);
    _output_ports.push_back(op);

    _egress_pipes.push_back(egress_pipe);
    _egress_speeds.push_back(speed);

    Route* pause_route = new Route();
    if (egress_pipe && remote) {
        pause_route->push_back(egress_pipe);
        pause_route->push_back(remote);
    }
    _pause_routes.push_back(pause_route);

    if (ingress_pipe) {
        _ingress_pipe_to_port[(PacketSink*)ingress_pipe] = port_idx;
    }

    _control_plane.registerPort(port_idx, egress_pipe);

    return port_idx;
}

uint64_t SingSwitch::oneHopBdpBytes(int port) const {
    assert(port >= 0 && port < (int)_egress_pipes.size());
    if (!_egress_pipes[port])
        return 0;

    long double bits = static_cast<long double>(_egress_speeds[port]) *
                       static_cast<long double>(_egress_pipes[port]->delay()) / 1.0e12L;
    long double bytes = bits / 8.0L;
    return static_cast<uint64_t>(std::ceil(bytes));
}

bool SingSwitch::configureDynamicPfc(double pfc_alpha, uint64_t pfc_resume_offset,
                                     uint64_t headroom_override) {
    uint64_t total_headroom = 0;
    uint64_t mtu_bytes = static_cast<uint64_t>(Packet::data_packet_size());

    for (int p = 0; p < (int)_output_ports.size(); p++) {
        uint64_t one_hop_bdp = oneHopBdpBytes(p);
        uint64_t default_headroom = 0;
        if (one_hop_bdp >
            (std::numeric_limits<uint64_t>::max() - (2 * mtu_bytes)) / 2) {
            default_headroom = std::numeric_limits<uint64_t>::max();
        } else {
            default_headroom = (2 * one_hop_bdp) + (2 * mtu_bytes);
        }
        uint64_t headroom = headroom_override ? headroom_override : default_headroom;
        for (int q = 0; q < _num_prios; q++) {
            if (_pfc_engine.isPfcEnabled(q)) {
                _buffer_mgr.setHeadroomCap(p, q, headroom);
                total_headroom += headroom;
            } else {
                _buffer_mgr.setHeadroomCap(p, q, 0);
            }
        }
    }

    if (total_headroom >= _buffer_mgr.totalCapacity()) {
        std::cerr << "FATAL: invalid PFC headroom on switch " << _name
                  << ": total_headroom=" << total_headroom
                  << " total_capacity=" << _buffer_mgr.totalCapacity()
                  << std::endl;
        return false;
    }

    _buffer_mgr.configureSharedBuffer(total_headroom, pfc_alpha, pfc_resume_offset);
    return true;
}

void SingSwitch::addHostPort(int addr, int flowid, PacketSink* transport) {
    _control_plane.addHostPort(addr, flowid, transport);
}

int SingSwitch::resolveIngressPort(Packet& pkt) const {
    PacketSink* prev = pkt.previousHop();
    if (!prev) {
        std::cerr << "FATAL: " << _name
                  << " failed to resolve ingress port: packet has no previous hop"
                  << std::endl;
        abort();
    }
    auto it = _ingress_pipe_to_port.find(prev);
    if (it == _ingress_pipe_to_port.end()) {
        std::cerr << "FATAL: " << _name
                  << " failed to resolve ingress port: previous hop "
                  << prev->nodename() << " is not in ingress map"
                  << std::endl;
        abort();
    }
    return it->second;
}

void SingSwitch::sendPfcPause(int ingress_port, int prio, bool pause) {
    assert(ingress_port >= 0 && ingress_port < (int)_egress_pipes.size());

    uint32_t sleep_time = pause ? 1000 : 0;
    EthPausePacket* p = EthPausePacket::newpkt(sleep_time, _id, prio);

    if (_pause_routes[ingress_port]->size() >= 2) {
        p->set_route(*_pause_routes[ingress_port]);
        p->sendOn();
        if (pause)
            _pfc_xoff_sent++;
        else
            _pfc_xon_sent++;
    } else {
        p->free();
    }
}

void SingSwitch::onEgressRelease(int ingress_port, int prio) {
    bool resume_event = _buffer_mgr.shouldSendResume(ingress_port, prio);
    if (_pfc_engine.checkXon(ingress_port, prio, resume_event)) {
        sendPfcPause(ingress_port, prio, false);
    }
}

void SingSwitch::receivePacket(Packet& pkt) {
    if (pkt.type() == ETH_PAUSE) {
        EthPausePacket& pause_pkt = static_cast<EthPausePacket&>(pkt);
        int port = resolveIngressPort(pkt);

        int pfc_prio = pause_pkt.pfcPriority();
        bool is_pause = (pause_pkt.sleepTime() > 0);
        _pfc_pause_rcvd++;

        if (pfc_prio >= 0) {
            _pfc_engine.onPfcFrameReceived(port, pfc_prio, is_pause);
            if (!is_pause) {
                _output_ports[port]->resumeIfReady();
            }
        } else {
            for (int q = 0; q < _num_prios; q++) {
                _pfc_engine.onPfcFrameReceived(port, q, is_pause);
            }
            if (!is_pause) {
                _output_ports[port]->resumeIfReady();
            }
        }

        pause_pkt.free();
        return;
    }

    auto it = _pending_packets.find(&pkt);

    if (it == _pending_packets.end()) {
        // ─── Ingress stage ───
        int ingress_port = resolveIngressPort(pkt);

        ClassifyResult cr = _control_plane.classify(pkt);

        AdmitResult ar = _buffer_mgr.admit(cr.out_port_idx, cr.priority,
                                           pkt.size(), ingress_port);

        if (ar.pfc_trigger) {
            if (_pfc_engine.checkXoff(ingress_port, cr.priority, true)) {
                sendPfcPause(ingress_port, cr.priority, true);
            }
        }

        if (!ar.admitted) {
            assert(cr.out_port_idx >= 0 && cr.out_port_idx < (int)_output_ports.size() &&
                   cr.priority >= 0 && cr.priority < _num_prios);
            _output_ports[cr.out_port_idx]->queue(cr.priority).logDrop(pkt);
            pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_DROP);
            pkt.free();
            return;
        }

        _pending_packets[&pkt] = {cr.out_port_idx, cr.priority, ar.ecn_mark, ingress_port};

        pkt.set_route(*cr.route);
        pkt.set_direction(cr.direction);

        _pipe->receivePacket(pkt);
    } else {
        // ─── Egress stage (callback from CallbackPipe) ───
        IngressInfo info = it->second;
        _pending_packets.erase(it);

        if (info.ecn_mark) {
            pkt.set_flags(pkt.flags() | ECN_CE);
        }

        assert(info.out_port >= 0 && info.out_port < (int)_output_ports.size());
        _output_ports[info.out_port]->enqueue(info.priority, &pkt, info.ingress_port);
    }
}

uint64_t SingSwitch::queueSize(int port, int prio) const {
    return queue(port, prio).queuesize();
}

SingSwitchQueue& SingSwitch::queue(int port, int prio) {
    assert(port >= 0 && port < (int)_output_ports.size());
    assert(prio >= 0 && prio < _num_prios);
    return _output_ports[port]->queue(prio);
}

const SingSwitchQueue& SingSwitch::queue(int port, int prio) const {
    assert(port >= 0 && port < (int)_output_ports.size());
    assert(prio >= 0 && prio < _num_prios);
    return _output_ports[port]->queue(prio);
}

void SingSwitch::bindQueueLogger(int port, int prio, QueueLogger* logger,
                                 const std::string& queue_name) {
    SingSwitchQueue& q = queue(port, prio);
    q.setLogger(logger);
    q.forceName(queue_name);
}
