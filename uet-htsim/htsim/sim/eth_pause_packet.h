// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef ETHPACKET_H
#define ETHPACKET_H

#include <list>
#include <bitset>
#include "network.h"

// ETHPAUSE is a subclass of Packet
// They incorporate a packet database, to reuse packet objects that are no longer needed.
// Note: you never construct a new EthPause packet directly; 
// rather you use the static method newpkt() which knows to reuse old packets from the database.

#define PAUSESIZE 64

class EthPausePacket : public Packet {
 public:
    inline static EthPausePacket* newpkt(uint32_t sleep, uint32_t senderid, int priority = -1){
        EthPausePacket* p = _packetdb.allocPacket();
        p->_type = ETH_PAUSE;
        p->_sleepTime = sleep;
        p->_senderID = senderid;
        p->_priority = priority;
        p->_size = PAUSESIZE;
        p->_flow = &(Packet::_defaultFlow);
        return p;
    }
  
    virtual PktPriority priority() const {return Packet::PRIO_NONE;}
    void free() {_packetdb.freePacket(this);}
    virtual ~EthPausePacket(){}

    inline uint32_t sleepTime() const {return _sleepTime;}
    inline uint32_t senderID() const {return _senderID;}
    inline int pfcPriority() const {return _priority;}
 protected:
    uint32_t _sleepTime;
    uint32_t _senderID;
    int _priority; // -1 = legacy all-priority pause, >=0 = per-priority PFC
    static PacketDB<EthPausePacket> _packetdb;
};

#endif
