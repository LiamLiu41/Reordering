// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch_output_port.h"
#include "sing_switch_buffer_manager.h"
#include "sing_switch_pfc_engine.h"
#include "sing_switch.h"
#include "pipe.h"
#include <cassert>
#include <cstdlib>
#include <limits>
#include <sstream>

// ─── SingSwitchQueue ───

SingSwitchQueue::SingSwitchQueue(linkspeed_bps bitrate, EventList& eventlist)
    : BaseQueue(bitrate, eventlist, nullptr),
      _byte_length(0),
      _maxsize(std::numeric_limits<mem_b>::max()) {
    _nodename = "SingSwitchQueue";
}

void SingSwitchQueue::enqueue(Packet* pkt, int ingress_port) {
    assert(ingress_port >= 0);
    _fifo.push_back({pkt, ingress_port});
    _byte_length += pkt->size();
    if (_logger) {
        _logger->logQueue(*this, QueueLogger::PKT_ENQUEUE, *pkt);
    }
}

QueueEntry SingSwitchQueue::dequeue() {
    assert(!_fifo.empty());
    QueueEntry entry = _fifo.front();
    _fifo.pop_front();
    assert(_byte_length >= entry.pkt->size());
    _byte_length -= entry.pkt->size();
    if (_logger) {
        _logger->logQueue(*this, QueueLogger::PKT_SERVICE, *entry.pkt);
    }
    return entry;
}

void SingSwitchQueue::logDrop(Packet& pkt) {
    if (_logger) {
        _logger->logQueue(*this, QueueLogger::PKT_DROP, pkt);
    }
}

Packet* SingSwitchQueue::front() const {
    assert(!_fifo.empty());
    return _fifo.front().pkt;
}

void SingSwitchQueue::receivePacket(Packet& pkt) {
    (void)pkt;
    abort();
}

void SingSwitchQueue::doNextEvent() {
    abort();
}

// ─── OutputPort ───

OutputPort::OutputPort(int port_id, int num_prios, linkspeed_bps linkspeed,
                       EventList& eventlist, BufferManager* buffer_mgr)
    : EventSource(eventlist, "OutputPort"),
      _port_id(port_id),
      _num_prios(num_prios),
      _linkspeed(linkspeed),
      _buffer_mgr(buffer_mgr),
      _pfc_engine(nullptr),
      _sing_switch(nullptr),
      _busy(false),
      _servicing_prio(-1),
      _servicing_ingress_port(-1)
{
    _ps_per_byte = (simtime_picosec)((double)1e12 / ((double)_linkspeed / 8.0));
    std::stringstream ss;
    ss << "OutputPort(" << port_id << ")";
    setName(ss.str());

    _queues.reserve(_num_prios);
    for (int prio = 0; prio < _num_prios; prio++) {
        _queues.push_back(new SingSwitchQueue(_linkspeed, eventlist));
    }
}

OutputPort::~OutputPort() {
    for (SingSwitchQueue* q : _queues) {
        delete q;
    }
}

void OutputPort::enqueue(int prio, Packet* pkt, int ingress_port) {
    assert(prio >= 0 && prio < _num_prios);
    assert(ingress_port >= 0);
    _queues[prio]->enqueue(pkt, ingress_port);

    if (!_busy) {
        beginService();
    }
}

void OutputPort::beginService() {
    uint32_t paused = _pfc_engine ? _pfc_engine->getPausedMask(_port_id) : 0;
    int selected = _scheduler.selectQueue(_queues, paused);
    if (selected < 0) {
        _busy = false;
        return;
    }

    _busy = true;
    _servicing_prio = selected;

    Packet* pkt = _queues[selected]->front();
    simtime_picosec drain_time = (simtime_picosec)(pkt->size() * _ps_per_byte);
    eventlist().sourceIsPendingRel(*this, drain_time);
}

void OutputPort::completeService() {
    assert(_busy && _servicing_prio >= 0);

    QueueEntry entry = _queues[_servicing_prio]->dequeue();

    _buffer_mgr->release(_port_id, _servicing_prio, entry.pkt->size(), entry.ingress_port);

    if (_sing_switch) {
        _sing_switch->onEgressRelease(entry.ingress_port, _servicing_prio);
    }

    entry.pkt->sendOn();

    _servicing_prio = -1;
    _servicing_ingress_port = -1;

    uint32_t paused = _pfc_engine ? _pfc_engine->getPausedMask(_port_id) : 0;
    int next = _scheduler.selectQueue(_queues, paused);
    if (next >= 0) {
        beginService();
    } else {
        _busy = false;
    }
}

void OutputPort::resumeIfReady() {
    if (!_busy && !isEmpty()) {
        beginService();
    }
}

void OutputPort::doNextEvent() {
    completeService();
}

bool OutputPort::isEmpty() const {
    for (int i = 0; i < _num_prios; i++) {
        if (!_queues[i]->empty())
            return false;
    }
    return true;
}
