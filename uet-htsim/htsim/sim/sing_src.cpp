// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_src.h"
#include "sing_sink.h"
#include <math.h>
#include <cstdint>
#include "sing_logger.h"

using namespace std;

// Static stuff
flowid_t SingSrc::_debug_flowid = UINT32_MAX;
// _path_entropy_size is the number of paths we spray across.  If you don't set it, it will default
// to all paths.
int SingSrc::_global_node_count = 0;
bool SingSrc::_shown = false;
mem_b SingSrc::_configured_maxwnd = 0;
mem_b SingSrc::_default_max_unack_window = 0;

/* _min_rto can be tuned using setMinRTO. Don't change it here.  */
simtime_picosec SingSrc::_min_rto = timeFromUs((uint32_t)DEFAULT_UEC_RTO_MIN);

mem_b SingSink::_bytes_unacked_threshold = 16384;
int SingSink::TGT_EV_SIZE = 7;

/* this default will be overridden from packet size*/
uint16_t SingSrc::_hdr_size = 64;
uint16_t SingSrc::_mss = 4096;
uint16_t SingSrc::_mtu = _mss + _hdr_size;

bool SingSrc::_debug = false;
bool SingNIC::_debug = false;
bool SingNICPort::_debug = false;
bool SingSrc::_print_receiver_fct = true;

bool SingSrc::_sender_based_cc = false;

SingSrc::Sender_CC SingSrc::_sender_cc_algo = SingSrc::NSCC;
GlobalNetworkParams SingSrc::_global_network_params = {};

/* SLEEK parameters */
bool SingSrc::_enable_sleek = false;
bool SingSrc::_dump_cc_params = false;
float SingSrc::loss_retx_factor = 1.5;
int SingSrc::min_retx_config = 5;
/* End SLEEK parameters */

/* Pathwise subflows */
int SingSrc::_num_pathwise_subflows = 0;

void SingSrc::initCcGlobalDefaults(simtime_picosec network_rtt, mem_b network_bdp,
                                   linkspeed_bps linkspeed, bool trimming_enabled) {
    _sender_based_cc = true;
    _global_network_params.network_rtt_ps = network_rtt;
    _global_network_params.network_linkspeed_bps = linkspeed;
    _global_network_params.network_bdp_bytes = network_bdp;
    _global_network_params.trimming_enabled = trimming_enabled;
    _global_network_params.default_mtu_bytes = _mtu;
    _global_network_params.default_mss_bytes = _mss;

    cout << "Initializing global sender-cc params:"
        << " network_linkspeed=" << _global_network_params.network_linkspeed_bps
        << " network_rtt=" << _global_network_params.network_rtt_ps
        << " network_bdp=" << _global_network_params.network_bdp_bytes
        << " trimming_enabled=" << _global_network_params.trimming_enabled
        << endl;
}

void SingSrc::setFlowCcSelection(Sender_CC algo, std::optional<CcProfile> profile) {
    _flow_sender_cc_algo = algo;
    _flow_cc_profile = std::move(profile);
}

FlowBasicParams SingSrc::buildFlowBasicParams(simtime_picosec peer_rtt, mem_b bdp_bytes) const {
    FlowBasicParams params;
    params.peer_rtt_ps = peer_rtt;
    params.bdp_bytes = bdp_bytes;
    params.nic_linkspeed_bps = _nic.linkspeed();
    params.mtu_bytes = _global_network_params.default_mtu_bytes ? _global_network_params.default_mtu_bytes : _mtu;
    params.mss_bytes = _global_network_params.default_mss_bytes ? _global_network_params.default_mss_bytes : _mss;
    return params;
}

// Helper: build a CC instance and initialize its params. Returns nullptr if algo unknown.
unique_ptr<BaseCC> SingSrc::buildCcInstance(const FlowBasicParams& params,
                                            const CcProfile& profile,
                                            Sender_CC algo) {
    unique_ptr<BaseCC> cc;
    switch (algo) {
        case NSCC: {
            auto nscc = make_unique<Nscc>(eventlist(), _base_rtt);
            nscc->initCcParams(_global_network_params, params, profile);
            cc = std::move(nscc);
            break;
        }
        case DCTCP: {
            auto dctcp = make_unique<Dctcp>();
            dctcp->initCcParams(_global_network_params, params, profile);
            cc = std::move(dctcp);
            break;
        }
        case CONSTANT: {
            auto constant = make_unique<Constant>();
            constant->initCcParams(_global_network_params, params, profile);
            cc = std::move(constant);
            break;
        }
        case SWIFT: {
            auto swift = make_unique<Swift>(eventlist(), _base_rtt);
            swift->initCcParams(_global_network_params, params, profile);
            cc = std::move(swift);
            break;
        }
        case BARRE: {
            auto barre = make_unique<Barre>(eventlist(), _base_rtt);
            barre->initCcParams(_global_network_params, params, profile);
            cc = std::move(barre);
            break;
        }
        default:
            break;
    }
    return cc;
}

SingSrc::Sender_CC SingSrc::resolveSenderAlgoForFlow() const {
    if (_flow_sender_cc_algo.has_value()) {
        return *_flow_sender_cc_algo;
    }
    return _sender_cc_algo;
}

CcProfile SingSrc::resolveCcProfileForFlow() const {
    if (_flow_cc_profile.has_value()) {
        return *_flow_cc_profile;
    }
    return CcProfile{};
}

void SingSrc::initCcForFlow(const FlowBasicParams& params) {
    _base_rtt = params.peer_rtt_ps;
    _base_bdp = params.bdp_bytes;
    _bdp = _base_bdp;

    setMaxWnd(1.5*_bdp);
    setConfiguredMaxWnd(1.5*_bdp);

    if (!_sender_based_cc) {
        return;
    }

    if (_enable_sleek) {
        cout << "ERROR: Sleek is temporarily disabled in the subflow architecture." << endl;
        abort();
    }

    Sender_CC algo = resolveSenderAlgoForFlow();
    CcProfile selected_profile = resolveCcProfileForFlow();
    const double cnp_min_interval_us = CcProfileResolver::getDouble(
        selected_profile, "cnp_min_interval_us", 2.0, _global_network_params, params);
    if (cnp_min_interval_us <= 0.0) {
        _flow_cnp_min_interval = timeFromUs((uint32_t)2);
    } else {
        _flow_cnp_min_interval =
            static_cast<simtime_picosec>(cnp_min_interval_us * 1e6);
    }

    // Pathwise mode: subflows are created by createPathwiseSubflows() instead
    if (_num_pathwise_subflows > 1) {
        return;
    }

    auto cc_instance = buildCcInstance(params, selected_profile, algo);
    assert(cc_instance != nullptr);

    if (_dump_cc_params) {
        static bool cc_params_dumped = false;
        if (!cc_params_dumped) {
            cout << "[CC_INPUT]"
                 << " peer_rtt_ps=" << params.peer_rtt_ps
                 << " bdp_bytes=" << params.bdp_bytes
                 << " nic_linkspeed_bps=" << params.nic_linkspeed_bps
                 << " mtu_bytes=" << params.mtu_bytes
                 << " mss_bytes=" << params.mss_bytes
                 << " init_cwnd=" << params.init_cwnd
                 << endl;
            cc_instance->dumpParams(cout);
            cc_params_dumped = true;
        }
    }

    // Native LB: single subflow (subflow_id=0, entropy=0)
    _subflows.clear();
    auto subflow = std::make_unique<SingSubflow>(*this, 0, std::move(cc_instance), 0);
    _default_subflow = subflow.get();
    _subflows.push_back(std::move(subflow));
}

void SingSrc::createPathwiseSubflows(int num_subflows, const FlowBasicParams& params) {
    assert(num_subflows >= 2);
    assert(_sender_based_cc);
    assert(_subflows.empty());  // initCcForFlow must have skipped subflow creation

    Sender_CC algo = resolveSenderAlgoForFlow();
    CcProfile selected_profile = resolveCcProfileForFlow();

    for (int i = 0; i < num_subflows; i++) {
        auto cc_instance = buildCcInstance(params, selected_profile, algo);
        assert(cc_instance != nullptr);

        uint16_t fixed_entropy = (uint16_t)(rand() & 0xFFFF);
        auto sf = std::make_unique<SingSubflow>(*this, i, std::move(cc_instance), fixed_entropy);
        _subflows.push_back(std::move(sf));
    }
    _default_subflow = _subflows[0].get();

    cout << "Created " << num_subflows << " pathwise subflows for flow " << _flow.str()
         << " with entropies:";
    for (auto& sf : _subflows) {
        cout << " " << sf->entropy();
    }
    cout << endl;
}

/*
scaling_factor_a = current_BDP/100Gbps*net_base_rtt //12us scaling_factor_b = 12/target_Qdelay
beta = 5*scaling_factor_a
gamma = 0.15* scaling_factor_a
alpha = 4.0* scaling_factor_a*scaling_factor_b/base_rtt gamma_g = 0.8
*/

////////////////////////////////////////////////////////////////
//  UEC NIC PORT
////////////////////////////////////////////////////////////////

SingSrcPort::SingSrcPort(SingSrc& src, uint32_t port_num)
    : _src(src), _port_num(port_num) {
}

void SingSrcPort::setRoute(const Route& route) {
    _route = &route;
}

void SingSrcPort::receivePacket(Packet& pkt) {
    _src.receivePacket(pkt, _port_num);
}

const string& SingSrcPort::nodename() {
    return _src.nodename();
}

////////////////////////////////////////////////////////////////
//  UEC SRC
////////////////////////////////////////////////////////////////

SingSrc::SingSrc(TrafficLogger* trafficLogger, 
               EventList& eventList,
			   unique_ptr<SingMultipath> mp, 
               SingNIC& nic, 
               uint32_t no_of_ports)
        : EventSource(eventList, "uecSrc"), 
          _mp(move(mp)),
          _nic(nic), 
          _msg_tracker(),
          _last_event_time(),
          _flow_start_time(0),
          _ideal_time(0),
          _one_way_ideal_time(0),
          _flow(trafficLogger)
          {
    assert(_mp != nullptr);
    
    _mp->set_debug_tag(_flow.str());
    
    _node_num = _global_node_count++;
    _nodename = "uecSrc " + to_string(_node_num);

    _no_of_ports = no_of_ports;
    _ports.resize(no_of_ports);
    for (uint32_t p = 0; p < _no_of_ports; p++) {
        _ports[p] = new SingSrcPort(*this, p);
    }

    _rtx_timeout_pending = false;
    _rtx_timeout = timeInf;
    _rto_timer_handle = eventlist().nullHandle();

    _flow_logger = NULL;

    _logger = NULL;

    _maxwnd = 50 * _mtu;
    _flow_size = 0;
    _done_sending = false;
    _backlog = 0;
    _pending_rtx_pkts = 0;
    _in_flight = 0;
    _max_unack_window = _default_max_unack_window;
    _highest_sent = 0;

    // stats for debugging
    _stats = {};

    // by default, end silently
    _end_trigger = 0;

    _dstaddr = UINT32_MAX;
    //_route = NULL;
    _mtu = Packet::data_packet_size();
    _mss = _mtu - _hdr_size;

    _debug_src = SingSrc::_debug;
    _bdp = 0;
    _base_rtt = 0;

    //if (_node_num == 2) _debug_src = true; // use this to enable debugging on one flow at a
    // time
    _recvd_bytes = 0;

    _highest_recv_seqno = 0;
    _highest_rtx_sent = 0;

    _nscc_overall_stats = {};
}


void SingSrc::connectPort(uint32_t port_num,
                          Route& routeout,
                          Route& routeback,
                          SingSink& sink,
                          simtime_picosec start_time) {
    _ports[port_num]->setRoute(routeout);
    //_route = &routeout;

    if (port_num == 0) {
        _sink = &sink;
        _sink->setCnpMinInterval(_flow_cnp_min_interval);
        //_flow.set_id(get_id());  // identify the packet flow with the UEC source that generated it
        _flow._name = _name;

        if (start_time != TRIGGER_START) {
            eventlist().sourceIsPending(*this, timeFromUs((uint32_t)start_time));
        }
    }
    assert(_sink == &sink);
    _sink->connectPort(port_num, *this, routeback);
}


bool SingSrc::isTotallyFinished() {
    if (_msg_tracker.has_value()) {
        return _msg_tracker.value()->isTotallyFinished();
    } else {
        return _done_sending;
    }
}


void SingSrc::doNextEvent() {
    if (_rtx_timeout_pending && eventlist().now() == _rtx_timeout) {
        clearRTO();
        assert(_logger == 0);

        if (_logger)
            _logger->logUec(*this, SingLogger::UEC_TIMEOUT);

        rtxTimerExpired();
    } else if(_highest_sent == 0) {
        if (_debug_src)
            cout << _flow.str() << " " << "Starting flow " << _name << endl;
        startConnection();
    }

}


bool SingSrc::hasStarted() {
    return _last_event_time.has_value();
}


bool SingSrc::isActivelySending() {
    bool is_sending = false;
    /*
        Cases to consider:
        1. if we still have packets in the backlog or there are packets in the
          rtx queue, we can be sure that this connection is still being serviced.
        2. if there nothing to be sent, but the connection is not done yet,
          we must have a timeout running.
    */
    if (!(_backlog == 0 && !hasPendingRtx())) {
        // 1.
        is_sending = true;
    } else if (!_done_sending) {
        // 2.
        // Nothing to send, but the connection is not fully acked yet.
        // Make sure there is still a timeout around
        assert(_rtx_timeout_pending);
        is_sending = false;
    } else {
        // Nothing to send, everything has been sent
        assert(_rtx_timeout_pending==false);
        is_sending = false;
    }
    
    return is_sending;
}


void SingSrc::setFlowsize(uint64_t flow_size_in_bytes) {
    assert(!_msg_tracker.has_value());
    _flow_size = flow_size_in_bytes;
}


void SingSrc::addToBacklog(mem_b size) {
    _backlog += size;
    _flow_size += size;
    if (_done_sending) {
        _done_sending = false;
    }
}


void SingSrc::startConnection() {
    if (_debug_src)
        cout << _flow.str() << " " << "startflow " << _flow._name << " CWND " << currentWindowBytes() << " at "
             << timeAsUs(eventlist().now()) << " flow " << _flow.str() << endl;

    if (_last_event_time.has_value() and _last_event_time.value() == eventlist().now()) {
        cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename << " duplicate call to starting at "
            << timeAsUs(eventlist().now()) << endl;
        abort();
    } 

    assert(!hasStarted());
    _last_event_time.emplace(eventlist().now());
    _flow_start_time = eventlist().now();  // Record flow start time
    
    // Notify NIC that a flow has started
    _nic.flowStarted();

    // cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename << " starting at "
    //      << timeAsUs(eventlist().now()) << endl;


    if (_flow_logger) {
        _flow_logger->logEvent(_flow, *this, FlowEventLogger::START, _flow_size, 0);
    }

    clearRTO();
    _in_flight = 0;
    // backlog is total amount of data we expect to send, including headers
    if (!_msg_tracker.has_value()) {
        _backlog = ceil(((double)_flow_size) / _mss) * _hdr_size + _flow_size;
    } else {
        // In this case, _backlog will be populated directly from the PDC
    }

    _pending_rtx_pkts = 0;
    for (auto& sf : _subflows) {
        while (sf->hasRtx()) {
            sf->popRtxDsn();
        }
    }
    _max_unack_window = _default_max_unack_window;

    assert(_default_subflow != nullptr);
    for (auto& sf : _subflows) {
        _nic.registerSubflowForScheduling(sf.get(), eventlist().now());
    }
    
    if (_debug_src) {
        cout << _flow.str() << " registered " << _subflows.size()
             << " subflow(s) with NIC scheduler at "
             << timeAsUs(eventlist().now()) << endl;
    }
}


void SingSrc::continueConnection() {
    if (_debug_src)
        cout << "Flow " << _name << " flowId " << flowId() << " " << _nodename << " continue at "
            << timeAsUs(eventlist().now()) << endl;

    assert(_msg_tracker.has_value());
    assert(hasStarted());
    assert(_backlog > 0);
    assert(!hasPendingRtx());

    _last_event_time.emplace(eventlist().now());

    assert(_default_subflow != nullptr);
    for (auto& sf : _subflows) {
        _nic.registerSubflowForScheduling(sf.get(), eventlist().now());
    }
}


void SingSrc::activate() {
    cout << _flow.str() << " activate" << endl;
    startConnection();
}

void SingSrc::setEndTrigger(Trigger& end_trigger) {
    _end_trigger = &end_trigger;
};
