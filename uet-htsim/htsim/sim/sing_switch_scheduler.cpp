// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_switch_scheduler.h"
#include "sing_switch_output_port.h"

int StrictPriorityScheduler::selectQueue(const std::vector<SingSwitchQueue*>& queues,
                                         uint32_t paused_mask) {
    for (int i = 0; i < (int)queues.size(); i++) {
        if ((paused_mask >> i) & 1)
            continue;
        if (!queues[i]->empty())
            return i;
    }
    return -1;
}
