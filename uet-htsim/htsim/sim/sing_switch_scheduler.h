// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_SWITCH_SCHEDULER_H
#define SING_SWITCH_SCHEDULER_H

#include <vector>
#include <cstdint>

class SingSwitchQueue;

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual int selectQueue(const std::vector<SingSwitchQueue*>& queues, uint32_t paused_mask) = 0;
};

class StrictPriorityScheduler : public Scheduler {
public:
    int selectQueue(const std::vector<SingSwitchQueue*>& queues, uint32_t paused_mask) override;
};

#endif
