// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch_control_plane.h"
#include "datacenter/fat_tree_topology.h"
#include <cassert>
#include <iostream>

ControlPlane::ControlPlane(FatTreeSwitch::switch_type type, uint32_t id,
                           FatTreeTopology* ft, EventList& eventlist)
    : _type(type), _id(id), _ft(ft), _eventlist(eventlist),
      _uproutes(nullptr), _crt_route(0)
{
    _fib = new RouteTable();
    _hash_salt = random();
}

ControlPlane::~ControlPlane() {
    delete _fib;
}

void ControlPlane::registerPort(int port_idx, Pipe* pipe) {
    _pipe_to_port[pipe] = port_idx;
}

int ControlPlane::mapPriority(Packet::PktPriority prio) {
    switch (prio) {
    case Packet::PRIO_HI:   return 0;
    case Packet::PRIO_MID:  return 1;
    case Packet::PRIO_LO:   return 2;
    case Packet::PRIO_3:    return 3;
    case Packet::PRIO_4:    return 4;
    case Packet::PRIO_5:    return 5;
    case Packet::PRIO_6:    return 6;
    case Packet::PRIO_7:    return 7;
    default:                return 1;
    }
}

ClassifyResult ControlPlane::classify(Packet& pkt) {
    Route* route = getNextHop(pkt);
    assert(route && route->size() >= 2);

    Pipe* pipe = static_cast<Pipe*>(route->at(0));
    auto it = _pipe_to_port.find(pipe);
    assert(it != _pipe_to_port.end());
    int port_idx = it->second;

    int priority = mapPriority(pkt.priority());

    packet_direction dir = pkt.get_direction();

    return {port_idx, priority, route, dir};
}

void ControlPlane::addHostPort(int addr, int flowid, PacketSink* transport) {
    Route* rt = new Route();
    rt->push_back(_ft->pipes_nlp_ns[_ft->cfg().HOST_POD_SWITCH(addr)][addr][0]);
    rt->push_back(transport);
    _fib->addHostRoute(addr, rt, flowid);
}

void ControlPlane::permute_paths(std::vector<FibEntry*>* routes) {
    int len = routes->size();
    for (int i = 0; i < len; i++) {
        int ix = random() % (len - i);
        FibEntry* tmp = (*routes)[ix];
        (*routes)[ix] = (*routes)[len - 1 - i];
        (*routes)[len - 1 - i] = tmp;
    }
}

Route* ControlPlane::getNextHop(Packet& pkt) {
    std::vector<FibEntry*>* available_hops = _fib->getRoutes(pkt.dst());

    if (available_hops) {
        uint32_t ecmp_choice = 0;
        if (available_hops->size() > 1) {
            ecmp_choice = freeBSDHash(pkt.flow_id(), pkt.pathid(), _hash_salt)
                          % available_hops->size();
        }

        FibEntry* e = (*available_hops)[ecmp_choice];
        pkt.set_direction(e->getDirection());
        return e->getEgressPort();
    }

    if (_type == FatTreeSwitch::TOR &&
        _ft->cfg().HOST_POD_SWITCH(pkt.dst()) == _id) {
        HostFibEntry* fe = _fib->getHostRoute(pkt.dst(), pkt.flow_id());
        assert(fe);
        pkt.set_direction(DOWN);
        return fe->getEgressPort();
    }

    switch (_type) {
    case FatTreeSwitch::TOR:  buildFibTOR(pkt);  break;
    case FatTreeSwitch::AGG:  buildFibAGG(pkt);  break;
    case FatTreeSwitch::CORE: buildFibCORE(pkt); break;
    default:
        std::cerr << "ControlPlane: unknown switch type " << _type << std::endl;
        abort();
    }

    assert(_fib->getRoutes(pkt.dst()));
    return getNextHop(pkt);
}

void ControlPlane::buildFibTOR(Packet& pkt) {
    if (_uproutes) {
        _fib->setRoutes(pkt.dst(), _uproutes);
    } else {
        uint32_t agg_min, agg_max;

        if (_ft->cfg().get_tiers() == 3) {
            uint32_t podid = _id / _ft->cfg().tor_switches_per_pod();
            agg_min = _ft->cfg().MIN_POD_AGG_SWITCH(podid);
            agg_max = _ft->cfg().MAX_POD_AGG_SWITCH(podid);
        } else {
            agg_min = 0;
            agg_max = _ft->cfg().getNAGG() - 1;
        }

        for (uint32_t k = agg_min; k <= agg_max; k++) {
            for (uint32_t b = 0; b < _ft->cfg().bundlesize(AGG_TIER); b++) {
                Route* r = new Route();
                r->push_back(_ft->pipes_nlp_nup[_id][k][b]);
                r->push_back(_ft->switches_up[k]);
                _fib->addRoute(pkt.dst(), r, 1, UP);
            }
        }
        _uproutes = _fib->getRoutes(pkt.dst());
        permute_paths(_uproutes);
    }
}

void ControlPlane::buildFibAGG(Packet& pkt) {
    if (_ft->cfg().get_tiers() == 2 ||
        _ft->cfg().HOST_POD(pkt.dst()) == _ft->cfg().AGG_SWITCH_POD_ID(_id)) {
        uint32_t target_tor = _ft->cfg().HOST_POD_SWITCH(pkt.dst());
        for (uint32_t b = 0; b < _ft->cfg().bundlesize(AGG_TIER); b++) {
            Route* r = new Route();
            r->push_back(_ft->pipes_nup_nlp[_id][target_tor][b]);
            r->push_back(_ft->switches_lp[target_tor]);
            _fib->addRoute(pkt.dst(), r, 1, DOWN);
        }
    } else {
        if (_uproutes) {
            _fib->setRoutes(pkt.dst(), _uproutes);
        } else {
            uint32_t podpos = _id % _ft->cfg().agg_switches_per_pod();
            uint32_t uplink_bundles = _ft->cfg().radix_up(AGG_TIER) / _ft->cfg().bundlesize(CORE_TIER);
            for (uint32_t l = 0; l < uplink_bundles; l++) {
                uint32_t core = l * _ft->cfg().agg_switches_per_pod() + podpos;
                for (uint32_t b = 0; b < _ft->cfg().bundlesize(CORE_TIER); b++) {
                    Route* r = new Route();
                    r->push_back(_ft->pipes_nup_nc[_id][core][b]);
                    r->push_back(_ft->switches_c[core]);
                    _fib->addRoute(pkt.dst(), r, 1, UP);
                }
            }
            permute_paths(_fib->getRoutes(pkt.dst()));
        }
    }
}

void ControlPlane::buildFibCORE(Packet& pkt) {
    uint32_t nup = _ft->cfg().MIN_POD_AGG_SWITCH(_ft->cfg().HOST_POD(pkt.dst()))
                   + (_id % _ft->cfg().agg_switches_per_pod());
    for (uint32_t b = 0; b < _ft->cfg().bundlesize(CORE_TIER); b++) {
        Route* r = new Route();
        assert(_ft->pipes_nc_nup[_id][nup][b]);
        r->push_back(_ft->pipes_nc_nup[_id][nup][b]);
        r->push_back(_ft->switches_up[nup]);
        _fib->addRoute(pkt.dst(), r, 1, DOWN);
    }
}
