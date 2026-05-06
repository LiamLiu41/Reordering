// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_src.h"
#include "sing_sink.h"
#include <math.h>
#include <cstdint>
#include "sing_logger.h"

using namespace std;

void SingSrc::runSleek(uint32_t ooo, SingBasePacket::seq_t cum_ack) {
    mem_b wnd = currentWindowBytes();
    mem_b avg_size = get_avg_pktsize();
    mem_b threshold = min((mem_b)(loss_retx_factor * wnd), _maxwnd);
    threshold = max(threshold, min_retx_config*avg_size);

    if(_flow.flow_id() == _debug_flowid || _debug_src ){
        cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " rtx_threshold " << threshold/avg_size
            << " ooo " << ooo
            << " _highest_rtx_sent " << _highest_rtx_sent
            << " cwnd_in_pkts " << wnd/avg_size
            << " cum_ack " << cum_ack
            << " highest_sent " << _highest_sent
            << " _backlog " << _backlog
            << endl;
    }

    if (cum_ack >= _recovery_seqno && _loss_recovery_mode) {
        _loss_recovery_mode = false;
        if (_flow.flow_id() == _debug_flowid || _debug_src){
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " exit_loss " <<endl;
        }
    }

    if (ooo < threshold/avg_size && !_loss_recovery_mode)
        return;

    if (!_loss_recovery_mode && !hasPendingRtx()) {
        _loss_recovery_mode = true;
        _recovery_seqno = _highest_sent ;
        if (_flow.flow_id() == _debug_flowid || _debug_src ){
            cout << timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " enter_loss " << " _highest_sent " << _highest_sent <<endl;
        }
    }

    // move the packet to the RTX queue
    for (SingBasePacket::seq_t rtx_seqno = cum_ack; 
          rtx_seqno < _recovery_seqno && rtx_seqno < (cum_ack + wnd/get_avg_pktsize()); 
          rtx_seqno ++ ) {
        if (rtx_seqno < _highest_rtx_sent)
            continue;

        auto i = _tx_bitmap.find(rtx_seqno);
        if (i == _tx_bitmap.end()) {
            // this means this packet seqno has been acked.
            continue;
        }

        if (isPendingRtx(rtx_seqno)) {
            continue;
        }

        if (_flow.flow_id() == _debug_flowid ) {
            cout <<  timeAsUs(eventlist().now()) << " flowid " << _flow.flow_id() << " rtx_seqno " << rtx_seqno
                << " _highest_recv_seqno "<< _highest_recv_seqno
                << " recovery_seqno " << _recovery_seqno
                << endl;
        }       

        _stats._sleek_counter++;

        mem_b pkt_size = i->second.pkt_size;
        assert(pkt_size >= _hdr_size);
        auto seqno = i->first;
        uint16_t owner_subflow_id = i->second.owner_subflow_id;
        assert(owner_subflow_id < _subflows.size());
        _subflows[owner_subflow_id]->reduceSsnInFlight(pkt_size);
        _highest_rtx_sent = seqno+1;
        bool marked = markDsnForRtx(seqno);
        assert(marked);
        // penalizePath(ev, 1);
    }
    // markDsnForRtx() already reschedules the owner subflow with NIC.
}
