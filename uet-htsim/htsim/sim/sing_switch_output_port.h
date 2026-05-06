// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_OUTPUT_PORT_H
#define SING_SWITCH_OUTPUT_PORT_H

#include <list>
#include <vector>
#include <cstdint>
#include "queue.h"
#include "sing_switch_scheduler.h"

class BufferManager;
class PFCEngine;
class SingSwitch;
class Pipe;

struct QueueEntry {
    Packet* pkt;
    int ingress_port;
};

class SingSwitchQueue : public BaseQueue {
public:
    SingSwitchQueue(linkspeed_bps bitrate, EventList& eventlist);

    void enqueue(Packet* pkt, int ingress_port);
    QueueEntry dequeue();
    void logDrop(Packet& pkt);
    Packet* front() const;
    bool empty() const { return _fifo.empty(); }
    uint64_t byteLength() const { return _byte_length; }
    int packetCount() const { return (int)_fifo.size(); }

    mem_b queuesize() const override { return _byte_length; }
    mem_b maxsize() const override { return _maxsize; }

    void receivePacket(Packet& pkt) override;
    void doNextEvent() override;

private:
    std::list<QueueEntry> _fifo;
    uint64_t _byte_length;
    mem_b _maxsize;
};

class OutputPort : public EventSource {
public:
    OutputPort(int port_id, int num_prios, linkspeed_bps linkspeed,
               EventList& eventlist, BufferManager* buffer_mgr);
    ~OutputPort() override;

    void enqueue(int prio, Packet* pkt, int ingress_port);
    void doNextEvent() override;

    void setPfcEngine(PFCEngine* engine) { _pfc_engine = engine; }
    void setSingSwitch(SingSwitch* sw) { _sing_switch = sw; }

    void resumeIfReady();

    bool isBusy() const { return _busy; }
    bool isEmpty() const;
    SingSwitchQueue& queue(int prio) { return *_queues[prio]; }
    const SingSwitchQueue& queue(int prio) const { return *_queues[prio]; }

    int portId() const { return _port_id; }

private:
    void beginService();
    void completeService();

    int _port_id;
    int _num_prios;
    linkspeed_bps _linkspeed;
    simtime_picosec _ps_per_byte;

    std::vector<SingSwitchQueue*> _queues;
    StrictPriorityScheduler _scheduler;

    BufferManager* _buffer_mgr;
    PFCEngine* _pfc_engine;
    SingSwitch* _sing_switch;

    bool _busy;
    int _servicing_prio;
    int _servicing_ingress_port;
};

#endif
