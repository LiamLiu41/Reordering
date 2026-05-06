// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_CONTROL_PLANE_H
#define SING_SWITCH_CONTROL_PLANE_H

#include "routetable.h"
#include "network.h"
#include "datacenter/fat_tree_switch.h"
#include <unordered_map>

class FatTreeTopology;

struct ClassifyResult {
    int out_port_idx;
    int priority;
    Route* route;
    packet_direction direction;
};

class ControlPlane {
public:
    ControlPlane(FatTreeSwitch::switch_type type, uint32_t id,
                 FatTreeTopology* ft, EventList& eventlist);
    ~ControlPlane();

    ClassifyResult classify(Packet& pkt);
    void addHostPort(int addr, int flowid, PacketSink* transport);

    void registerPort(int port_idx, Pipe* pipe);

    static int mapPriority(Packet::PktPriority prio);

private:
    Route* getNextHop(Packet& pkt);
    void buildFibTOR(Packet& pkt);
    void buildFibAGG(Packet& pkt);
    void buildFibCORE(Packet& pkt);

    void permute_paths(std::vector<FibEntry*>* routes);

    FatTreeSwitch::switch_type _type;
    uint32_t _id;
    FatTreeTopology* _ft;
    EventList& _eventlist;

    RouteTable* _fib;
    std::vector<FibEntry*>* _uproutes;

    uint32_t _hash_salt;
    uint32_t _crt_route;

    std::unordered_map<Pipe*, int> _pipe_to_port;
};

#endif
