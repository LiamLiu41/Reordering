// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "sing_cc.h"

#include <algorithm>
#include <cmath>
#include <ostream>

using std::max;

namespace {

inline double gbpsToBytesPerPs(double gbps) {
    return (gbps * 1e9) / (8.0 * 1e12);
}

inline double bytesPerPsToGbps(double bytes_per_ps) {
    return (bytes_per_ps * 8.0 * 1e12) / 1e9;
}

inline simtime_picosec usToPs(double us) {
    if (us <= 0.0) {
        return 0;
    }
    return static_cast<simtime_picosec>(us * 1e6);
}

inline double psToUs(simtime_picosec ps) {
    return static_cast<double>(ps) / 1e6;
}

}  // namespace

Constant::Constant() : _cfg{} {}

void Constant::initCcParams(const GlobalNetworkParams& global,
                            const FlowBasicParams& params,
                            const CcProfile& profile) {
    _cfg.init_cwnd = CcProfileResolver::getMemB(
        profile, "init_cwnd", params.init_cwnd, global, params);

    // Preferred explicit-rate parameters for Constant CC:
    // 1) rate_bytes_per_ps
    // 2) rate_bps
    // 3) rate_gbps
    // If none are present, keep legacy behavior: rate = init_cwnd / peer_rtt.
    double rate_bytes_per_ps = CcProfileResolver::getDouble(
        profile, "rate_bytes_per_ps", -1.0, global, params);
    if (rate_bytes_per_ps > 0.0) {
        _cfg.rate_bytes_per_ps = rate_bytes_per_ps;
        return;
    }

    double rate_bps = CcProfileResolver::getDouble(
        profile, "rate_bps", -1.0, global, params);
    if (rate_bps > 0.0) {
        _cfg.rate_bytes_per_ps = rate_bps / (8.0 * 1e12);
        return;
    }

    double rate_gbps = CcProfileResolver::getDouble(
        profile, "rate_gbps", -1.0, global, params);
    if (rate_gbps > 0.0) {
        _cfg.rate_bytes_per_ps = (rate_gbps * 1e9) / (8.0 * 1e12);
        return;
    }

    if (params.peer_rtt_ps <= 0) {
        _cfg.rate_bytes_per_ps = 0.0;
        return;
    }
    _cfg.rate_bytes_per_ps = static_cast<double>(_cfg.init_cwnd) /
                             static_cast<double>(params.peer_rtt_ps);
}

void Constant::dumpParams(std::ostream& os) const {
    const double rate_bps = _cfg.rate_bytes_per_ps * 8.0 * 1e12;
    const double rate_gbps = rate_bps / 1e9;
    os << "[CC_PARAM] algo=constant param=init_cwnd value=" << _cfg.init_cwnd << "\n"
       << "[CC_PARAM] algo=constant param=rate_bytes_per_ps value=" << _cfg.rate_bytes_per_ps << "\n"
       << "[CC_PARAM] algo=constant param=rate_bps value=" << rate_bps << "\n"
       << "[CC_PARAM] algo=constant param=rate_gbps value=" << rate_gbps << "\n";
}

void Constant::onAck(simtime_picosec, mem_b, bool, mem_b) {}
void Constant::onNack(mem_b, bool, mem_b) {}
void Constant::onTimeout(mem_b) {}

Dctcp::Dctcp() : _cfg{}, _cwnd(1) {}

void Dctcp::initCcParams(const GlobalNetworkParams& global,
                         const FlowBasicParams& params,
                         const CcProfile& profile) {
    _cfg.min_cwnd = CcProfileResolver::getMemB(
        profile, "min_cwnd", params.mtu_bytes, global, params);
    _cfg.init_cwnd = CcProfileResolver::getMemB(
        profile, "init_cwnd", params.init_cwnd, global, params);
    if (_cfg.init_cwnd == 0) {
        _cfg.init_cwnd = _cfg.min_cwnd;
    }
    _cfg.ai_unit = CcProfileResolver::getMemB(
        profile, "ai_unit", params.mtu_bytes, global, params);

    _cwnd = (_cfg.init_cwnd > 0) ? _cfg.init_cwnd : (_cfg.min_cwnd > 0 ? _cfg.min_cwnd : static_cast<mem_b>(1));
}

void Dctcp::onAck(simtime_picosec, mem_b acked_bytes, bool ecn, mem_b) {
    if (!ecn) {
        if (_cwnd > 0) {
            mem_b ai = (_cfg.ai_unit > 0) ? _cfg.ai_unit : static_cast<mem_b>(1);
            _cwnd += static_cast<mem_b>(static_cast<double>(acked_bytes) * static_cast<double>(ai) /
                                         static_cast<double>(_cwnd));
        }
    } else {
        _cwnd -= acked_bytes / 3;
    }
    _cwnd = max(_cfg.min_cwnd, _cwnd);
}

void Dctcp::onNack(mem_b nacked_bytes, bool, mem_b) {
    _cwnd -= nacked_bytes;
    _cwnd = max(_cfg.min_cwnd, _cwnd);
}

void Dctcp::onTimeout(mem_b) {
    _cwnd = max(_cfg.min_cwnd, _cwnd / 2);
}

void Dctcp::dumpParams(std::ostream& os) const {
    os << "[CC_PARAM] algo=dctcp param=min_cwnd value=" << _cfg.min_cwnd << "\n"
       << "[CC_PARAM] algo=dctcp param=init_cwnd value=" << _cfg.init_cwnd << "\n"
       << "[CC_PARAM] algo=dctcp param=ai_unit value=" << _cfg.ai_unit << "\n"
       << "[CC_PARAM] algo=dctcp param=cwnd value=" << _cwnd << "\n";
}

Nscc::Nscc(EventList& ev, simtime_picosec base_rtt)
    : _eventlist(ev),
      _cfg(),
      _cwnd(1),
      _base_rtt(base_rtt) {}

void Nscc::initCcParams(const GlobalNetworkParams& global,
                        const FlowBasicParams& params,
                        const CcProfile& profile) {
    _cfg.min_cwnd = CcProfileResolver::getMemB(
        profile, "min_cwnd", params.mtu_bytes, global, params);

    mem_b default_max = static_cast<mem_b>(1.5 * params.bdp_bytes);
    _cfg.max_cwnd = CcProfileResolver::getMemB(
        profile, "max_cwnd", default_max, global, params);

    simtime_picosec default_target = global.trimming_enabled
                                         ? static_cast<simtime_picosec>(global.network_rtt_ps * 0.75)
                                         : global.network_rtt_ps;
    _cfg.target_qdelay = CcProfileResolver::getTimePs(
        profile, "target_qdelay", default_target, global, params);
    if (_cfg.target_qdelay == 0) {
        _cfg.target_qdelay = default_target;
    }

    _cfg.gamma = CcProfileResolver::getDouble(
        profile, "gamma", 0.8, global, params);
    _cfg.delay_alpha = CcProfileResolver::getDouble(
        profile, "delay_alpha", 0.0125, global, params);
    _cfg.disable_quick_adapt = CcProfileResolver::getBool(
        profile, "disable_quick_adapt", false, global, params);
    _cfg.qa_gate = CcProfileResolver::getUint8(
        profile, "qa_gate", 3, global, params);
    _cfg.network_rtt = global.network_rtt_ps;

    const linkspeed_bps reference_network_linkspeed = speedFromGbps(100);
    const simtime_picosec reference_network_rtt = timeFromUs(12u);
    const mem_b reference_network_bdp =
        static_cast<mem_b>(timeAsSec(reference_network_rtt) * (reference_network_linkspeed / 8));

    const double scaling_factor_a =
        (reference_network_bdp > 0) ? (static_cast<double>(global.network_bdp_bytes) /
                                       static_cast<double>(reference_network_bdp))
                                    : 1.0;
    const double scaling_factor_b =
        (reference_network_rtt > 0) ? (static_cast<double>(_cfg.target_qdelay) /
                                       static_cast<double>(reference_network_rtt))
                                    : 1.0;

    _cfg.alpha = 4.0 * params.mss_bytes * scaling_factor_a * scaling_factor_b / _cfg.target_qdelay;
    _cfg.fi = 5 * params.mss_bytes * scaling_factor_a;
    _cfg.fi_scale = .25 * scaling_factor_a;
    _cfg.eta = 0.15 * params.mss_bytes * scaling_factor_a;
    _cfg.qa_threshold = 4 * _cfg.target_qdelay;
    _cfg.adjust_period_threshold = global.network_rtt_ps;
    _cfg.adjust_bytes_threshold = 8 * params.mtu_bytes;
    _cfg.qa_scaling = 1;

    _cwnd = CcProfileResolver::getMemB(profile, "init_cwnd", params.init_cwnd, global, params);
    if (_cwnd == 0) {
        _cwnd = _cfg.max_cwnd > 0 ? _cfg.max_cwnd : _cfg.min_cwnd;
    }

    setCwndBounds();
}

void Nscc::dumpParams(std::ostream& os) const {
    os << "[CC_PARAM] algo=nscc param=min_cwnd value=" << _cfg.min_cwnd << "\n"
       << "[CC_PARAM] algo=nscc param=max_cwnd value=" << _cfg.max_cwnd << "\n"
       << "[CC_PARAM] algo=nscc param=target_qdelay value=" << _cfg.target_qdelay << "\n"
       << "[CC_PARAM] algo=nscc param=gamma value=" << _cfg.gamma << "\n"
       << "[CC_PARAM] algo=nscc param=delay_alpha value=" << _cfg.delay_alpha << "\n"
       << "[CC_PARAM] algo=nscc param=alpha value=" << _cfg.alpha << "\n"
       << "[CC_PARAM] algo=nscc param=fi value=" << _cfg.fi << "\n"
       << "[CC_PARAM] algo=nscc param=fi_scale value=" << _cfg.fi_scale << "\n"
       << "[CC_PARAM] algo=nscc param=eta value=" << _cfg.eta << "\n"
       << "[CC_PARAM] algo=nscc param=disable_quick_adapt value=" << _cfg.disable_quick_adapt << "\n"
       << "[CC_PARAM] algo=nscc param=qa_gate value=" << static_cast<int>(_cfg.qa_gate) << "\n"
       << "[CC_PARAM] algo=nscc param=qa_threshold value=" << _cfg.qa_threshold << "\n"
       << "[CC_PARAM] algo=nscc param=qa_scaling value=" << _cfg.qa_scaling << "\n"
       << "[CC_PARAM] algo=nscc param=adjust_period_threshold value=" << _cfg.adjust_period_threshold << "\n"
       << "[CC_PARAM] algo=nscc param=adjust_bytes_threshold value=" << _cfg.adjust_bytes_threshold << "\n"
       << "[CC_PARAM] algo=nscc param=network_rtt value=" << _cfg.network_rtt << "\n"
       << "[CC_PARAM] algo=nscc param=init_cwnd value=" << _cwnd << "\n";
}

void Nscc::setCwndBounds() {
    if (_cwnd < _cfg.min_cwnd) {
        _cwnd = _cfg.min_cwnd;
    }
    if (_cfg.max_cwnd > 0 && _cwnd > _cfg.max_cwnd) {
        _cwnd = _cfg.max_cwnd;
    }
}

void Nscc::updateDelay(simtime_picosec raw_rtt, bool update_avg, bool skip) {
    simtime_picosec delay = raw_rtt - _base_rtt;
    if (update_avg) {
        if (!skip && delay > _cfg.target_qdelay) {
            _avg_delay = _cfg.delay_alpha * _base_rtt * 0.25 + (1 - _cfg.delay_alpha) * _avg_delay;
        } else {
            if (delay > 5 * _base_rtt) {
                double r = 0.0125;
                _avg_delay = r * delay + (1 - r) * _avg_delay;
            } else {
                _avg_delay = _cfg.delay_alpha * delay + (1 - _cfg.delay_alpha) * _avg_delay;
            }
        }
    }
}

bool Nscc::quickAdapt(bool is_loss, bool skip, simtime_picosec delay, mem_b in_flight) {
    bool qa_done_or_ignore = false;

    if (_cfg.disable_quick_adapt) {
        return false;
    }

    if (_bytes_ignored < _bytes_to_ignore && skip) {
        qa_done_or_ignore = true;
    } else if (_eventlist.now() > _qa_endtime) {
        if (_qa_endtime != 0 &&
            (_trigger_qa || is_loss || (delay > _cfg.qa_threshold)) &&
            _achieved_bytes < (_cfg.max_cwnd >> _cfg.qa_gate)) {
            if (_cwnd < _achieved_bytes) {
                qa_done_or_ignore = true;
            }

            _cwnd = max(_achieved_bytes, _cfg.min_cwnd);
            _bytes_to_ignore = in_flight;
            _bytes_ignored = 0;
            _trigger_qa = false;
        }
        _achieved_bytes = 0;
        _qa_endtime = _eventlist.now() + _base_rtt + _cfg.target_qdelay;
        _inc_bytes = 0;
        _received_bytes = 0;
    }
    return qa_done_or_ignore;
}

void Nscc::fairIncrease(uint32_t newly_acked_bytes) {
    _inc_bytes += _cfg.fi * newly_acked_bytes;
}

void Nscc::proportionalIncrease(uint32_t newly_acked_bytes, simtime_picosec delay) {
    fastIncrease(newly_acked_bytes, delay);
    _inc_bytes += _cfg.alpha * newly_acked_bytes * (_cfg.target_qdelay - delay);
}

void Nscc::fastIncrease(uint32_t newly_acked_bytes, simtime_picosec) {
    _fi_count += newly_acked_bytes;
    if (_fi_count > _cwnd || _increase) {
        _cwnd += newly_acked_bytes * _cfg.fi_scale;
    } else {
        _fi_count = 0;
    }
}

void Nscc::multiplicativeDecrease() {
    _fi_count = 0;
    simtime_picosec avg_delay = _avg_delay;
    if (avg_delay > _cfg.target_qdelay) {
        if (_eventlist.now() - _last_dec_time > _base_rtt) {
            _cwnd *= max(1 - _cfg.gamma * (avg_delay - _cfg.target_qdelay) / avg_delay, 0.5);
            _cwnd = max(_cwnd, _cfg.min_cwnd);
            _last_dec_time = _eventlist.now();
        }
    }
}

void Nscc::fulfillAdjustment() {
    _cwnd += _inc_bytes / _cwnd;

    if ((_eventlist.now() - _last_adjust_time) >= _cfg.adjust_period_threshold) {
        _cwnd += _cfg.eta;
        _last_adjust_time = _eventlist.now();
    }

    _inc_bytes = 0;
    _received_bytes = 0;
}

void Nscc::onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) {
    simtime_picosec queue_delay = 0;
    if (raw_rtt > _base_rtt) {
        queue_delay = raw_rtt - _base_rtt;
    }

    _achieved_bytes += acked_bytes;
    _received_bytes += acked_bytes;
    _bytes_ignored += acked_bytes;
    updateDelay(_base_rtt + queue_delay, true, ecn);

    if (quickAdapt(false, ecn, queue_delay, in_flight)) {
        return;
    }

    if (!ecn && queue_delay >= _cfg.target_qdelay) {
        fairIncrease(acked_bytes);
    } else if (!ecn && queue_delay < _cfg.target_qdelay) {
        proportionalIncrease(acked_bytes, queue_delay);
    } else if (ecn && queue_delay >= _cfg.target_qdelay) {
        multiplicativeDecrease();
    } else if (ecn && queue_delay < _cfg.target_qdelay) {
        // NOOP
    }

    setCwndBounds();

    if (_received_bytes > _cfg.adjust_bytes_threshold ||
        _eventlist.now() - _last_adjust_time > _cfg.adjust_period_threshold) {
        fulfillAdjustment();
    }

    setCwndBounds();
}

void Nscc::onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) {
    bool adjust_cwnd = true;
    _bytes_ignored += nacked_bytes;

    updateDelay(_base_rtt + _cfg.network_rtt, true, true);
    _trigger_qa = true;

    if (quickAdapt(true, true, 0, in_flight)) {
        adjust_cwnd = false;
    }

    if (adjust_cwnd && !last_hop) {
        _cwnd -= nacked_bytes;
        setCwndBounds();
    }
}

void Nscc::onTimeout(mem_b) {
    // Keep existing SingSrc behavior: timeout path is handled in sender logic.
}

Swift::Swift(EventList& ev, simtime_picosec base_rtt)
    : _eventlist(ev), _cfg(), _cwnd(1), _base_rtt(base_rtt) {}

void Swift::initCcParams(const GlobalNetworkParams& global,
                         const FlowBasicParams& params,
                         const CcProfile& profile) {
    _cfg.min_cwnd = CcProfileResolver::getMemB(
        profile, "min_cwnd", params.mtu_bytes, global, params);
    _cfg.max_cwnd = CcProfileResolver::getMemB(
        profile, "max_cwnd", static_cast<mem_b>(1.5 * params.bdp_bytes), global, params);
    _cfg.init_cwnd = CcProfileResolver::getMemB(
        profile, "init_cwnd", params.init_cwnd, global, params);
    if (_cfg.init_cwnd == 0) {
        _cfg.init_cwnd = _cfg.max_cwnd > 0 ? _cfg.max_cwnd : _cfg.min_cwnd;
    }

    _cfg.pkt_size = CcProfileResolver::getMemB(
        profile, "pkt_size", params.mtu_bytes, global, params);
    _cfg.base_target_delay = CcProfileResolver::getTimePs(
        profile, "base_target_delay", params.peer_rtt_ps, global, params);
    _cfg.ai = CcProfileResolver::getDouble(profile, "ai", 1.0, global, params);
    _cfg.beta = CcProfileResolver::getDouble(profile, "beta", 0.8, global, params);
    _cfg.max_mdf = CcProfileResolver::getDouble(profile, "max_mdf", 0.5, global, params);
    _cfg.rtx_reset_threshold = CcProfileResolver::getUint32(
        profile, "rtx_reset_threshold", 5, global, params);
    _cfg.fs_min_cwnd = CcProfileResolver::getDouble(
        profile, "fs_min_cwnd", 0.1, global, params);
    _cfg.fs_max_cwnd = CcProfileResolver::getDouble(
        profile, "fs_max_cwnd", 100.0, global, params);

    if (_cfg.pkt_size == 0) {
        _cfg.pkt_size = 1;
    }
    if (_cfg.base_target_delay == 0) {
        _cfg.base_target_delay = _base_rtt;
    }

    _cwnd = _cfg.init_cwnd;

    _rto = timeFromMs(1);
    _min_rto = timeFromUs((uint32_t)100);
    _mdev = 0;
    _last_decrease = 0;
    _retransmit_cnt = 0;

    if (_cfg.fs_min_cwnd <= 0.0) {
        _cfg.fs_min_cwnd = 0.1;
    }
    if (_cfg.fs_max_cwnd <= _cfg.fs_min_cwnd) {
        _cfg.fs_max_cwnd = _cfg.fs_min_cwnd + 1.0;
    }

    _fs_range = 5 * _cfg.base_target_delay;
    _fs_alpha = _fs_range /
                ((1.0 / std::sqrt(_cfg.fs_min_cwnd)) - (1.0 / std::sqrt(_cfg.fs_max_cwnd)));
    _fs_beta = -_fs_alpha / std::sqrt(_cfg.fs_max_cwnd);

    setCwndBounds();
}

void Swift::dumpParams(std::ostream& os) const {
    os << "[CC_PARAM] algo=swift param=min_cwnd value=" << _cfg.min_cwnd << "\n"
       << "[CC_PARAM] algo=swift param=max_cwnd value=" << _cfg.max_cwnd << "\n"
       << "[CC_PARAM] algo=swift param=init_cwnd value=" << _cfg.init_cwnd << "\n"
       << "[CC_PARAM] algo=swift param=pkt_size value=" << _cfg.pkt_size << "\n"
       << "[CC_PARAM] algo=swift param=base_target_delay value=" << _cfg.base_target_delay << "\n"
       << "[CC_PARAM] algo=swift param=ai value=" << _cfg.ai << "\n"
       << "[CC_PARAM] algo=swift param=beta value=" << _cfg.beta << "\n"
       << "[CC_PARAM] algo=swift param=max_mdf value=" << _cfg.max_mdf << "\n"
       << "[CC_PARAM] algo=swift param=rtx_reset_threshold value=" << _cfg.rtx_reset_threshold << "\n"
       << "[CC_PARAM] algo=swift param=fs_min_cwnd value=" << _cfg.fs_min_cwnd << "\n"
       << "[CC_PARAM] algo=swift param=fs_max_cwnd value=" << _cfg.fs_max_cwnd << "\n"
       << "[CC_PARAM] algo=swift param=cwnd value=" << _cwnd << "\n";
}

void Swift::setCwndBounds() {
    if (_cwnd < _cfg.min_cwnd) {
        _cwnd = _cfg.min_cwnd;
    }
    if (_cfg.max_cwnd > 0 && _cwnd > _cfg.max_cwnd) {
        _cwnd = _cfg.max_cwnd;
    }
}

void Swift::updateRtt(simtime_picosec raw_rtt) {
    if (raw_rtt != 0) {
        if (_rtt > 0) {
            simtime_picosec abs_delta = (raw_rtt > _rtt) ? (raw_rtt - _rtt) : (_rtt - raw_rtt);
            _mdev = 3 * _mdev / 4 + abs_delta / 4;
            _rtt = 7 * _rtt / 8 + raw_rtt / 8;
            _rto = _rtt + 4 * _mdev;
        } else {
            _rtt = raw_rtt;
            _mdev = raw_rtt / 2;
            _rto = _rtt + 4 * _mdev;
        }
    }
    if (_rto < _min_rto) {
        _rto = _min_rto;
    }
}

simtime_picosec Swift::targetDelay() const {
    double fs_delay = 0.0;
    if (_cwnd > 0 && _cfg.pkt_size > 0) {
        double cwnd_pkts = static_cast<double>(_cwnd) / static_cast<double>(_cfg.pkt_size);
        if (cwnd_pkts > 0.0) {
            fs_delay = _fs_alpha / std::sqrt(cwnd_pkts) + _fs_beta;
        }
    }
    if (fs_delay > static_cast<double>(_fs_range)) {
        fs_delay = static_cast<double>(_fs_range);
    }
    if (fs_delay < 0.0) {
        fs_delay = 0.0;
    }
    return _cfg.base_target_delay + static_cast<simtime_picosec>(fs_delay);
}

void Swift::onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool, mem_b) {
    simtime_picosec now = _eventlist.now();
    updateRtt(raw_rtt);

    _retransmit_cnt = 0;
    bool can_decrease = (_rtt == 0) || ((now - _last_decrease) >= _rtt);
    simtime_picosec tdelay = targetDelay();

    if (raw_rtt < tdelay) {
        if (_cwnd >= _cfg.pkt_size) {
            _cwnd = _cwnd + static_cast<mem_b>(static_cast<double>(_cfg.pkt_size) * _cfg.ai *
                                                static_cast<double>(acked_bytes) /
                                                static_cast<double>(_cwnd));
        } else {
            _cwnd = _cwnd + static_cast<mem_b>(_cfg.ai * static_cast<double>(acked_bytes));
        }
    } else if (can_decrease && raw_rtt > 0) {
        double factor = std::max(1.0 - _cfg.beta * (static_cast<double>(raw_rtt - tdelay) /
                                                    static_cast<double>(raw_rtt)),
                                 1.0 - _cfg.max_mdf);
        _cwnd = static_cast<mem_b>(static_cast<double>(_cwnd) * factor);
        _last_decrease = now;
    }

    setCwndBounds();
}

void Swift::onNack(mem_b, bool, mem_b) {
    simtime_picosec now = _eventlist.now();
    bool can_decrease = (_rtt == 0) || ((now - _last_decrease) >= _rtt);
    if (can_decrease) {
        _cwnd = static_cast<mem_b>(static_cast<double>(_cwnd) * (1.0 - _cfg.max_mdf));
        _last_decrease = now;
        setCwndBounds();
    }
}

void Swift::onTimeout(mem_b) {
    _retransmit_cnt++;
    if (_retransmit_cnt >= _cfg.rtx_reset_threshold) {
        _cwnd = _cfg.min_cwnd;
    } else {
        simtime_picosec now = _eventlist.now();
        bool can_decrease = (_rtt == 0) || ((now - _last_decrease) >= _rtt);
        if (can_decrease) {
            _cwnd = static_cast<mem_b>(static_cast<double>(_cwnd) * (1.0 - _cfg.max_mdf));
            _last_decrease = now;
        }
    }
    setCwndBounds();
}

Barre::Barre(EventList& ev, simtime_picosec base_rtt)
    : _eventlist(ev), _cfg(), _rtt_base(base_rtt) {}

void Barre::initCcParams(const GlobalNetworkParams& global,
                         const FlowBasicParams& params,
                         const CcProfile& profile) {
    const double default_alpha_base_gbps = 0.01;
    const double default_alpha_fast_gbps = 0.4;

    _cfg.alpha_base_increase_gbps = CcProfileResolver::getDouble(
        profile, "alpha_base_increase_gbps", default_alpha_base_gbps, global, params);
    _cfg.alpha_fast_increase_A_gbps = CcProfileResolver::getDouble(
        profile, "alpha_fast_increase_A_gbps", default_alpha_fast_gbps, global, params);
    _cfg.beta_cnp_decrease_ratio = CcProfileResolver::getDouble(
        profile, "beta_cnp_decrease_ratio", 0.98, global, params);
    _cfg.fast_increase_rounds_threshold_T = CcProfileResolver::getUint32(
        profile, "fast_increase_rounds_threshold_T", 3, global, params);
    _cfg.dual_lock_tx_bytes_threshold_N_bytes = CcProfileResolver::getMemB(
        profile, "dual_lock_tx_bytes_threshold_N_bytes", static_cast<mem_b>(8 * 1024), global, params);
    _cfg.alpha_k_rtt_ref_C_us = CcProfileResolver::getDouble(
        profile, "alpha_k_rtt_ref_C_us", 8.0, global, params);
    if (_cfg.alpha_k_rtt_ref_C_us <= 0.0) {
        _cfg.alpha_k_rtt_ref_C_us = 8.0;
    }
    _cfg.quarter_decrease_phi = CcProfileResolver::getDouble(
        profile, "quarter_decrease_phi", 1.5, global, params);
    if (_cfg.quarter_decrease_phi <= 0.0) {
        _cfg.quarter_decrease_phi = 1.5;
    }

    const double default_rate_max_gbps = static_cast<double>(params.nic_linkspeed_bps) / 1e9;
    const double default_rate_min_bytes_per_ps = (params.peer_rtt_ps > 0)
                                                     ? static_cast<double>(params.mtu_bytes) /
                                                           static_cast<double>(params.peer_rtt_ps)
                                                     : (default_rate_max_gbps > 0.0
                                                            ? gbpsToBytesPerPs(default_rate_max_gbps * 0.01)
                                                            : 1e-9);
    const double default_rate_min_gbps = bytesPerPsToGbps(default_rate_min_bytes_per_ps);
    const double default_rate_init_gbps = default_rate_max_gbps;

    _cfg.rate_min_gbps = CcProfileResolver::getDouble(
        profile, "rate_min_gbps", default_rate_min_gbps, global, params);
    _cfg.rate_max_gbps = CcProfileResolver::getDouble(
        profile, "rate_max_gbps", default_rate_max_gbps, global, params);
    _cfg.rate_init_gbps = CcProfileResolver::getDouble(
        profile, "rate_init_gbps", default_rate_init_gbps, global, params);

    if (_cfg.rate_min_gbps <= 0.0) {
        _cfg.rate_min_gbps = bytesPerPsToGbps(1e-9);
    }
    if (_cfg.rate_max_gbps <= 0.0) {
        _cfg.rate_max_gbps = _cfg.rate_min_gbps;
    }
    if (_cfg.rate_max_gbps < _cfg.rate_min_gbps) {
        _cfg.rate_max_gbps = _cfg.rate_min_gbps;
    }

    if (_rtt_base == 0) {
        _rtt_base = params.peer_rtt_ps;
    }
    if (_rtt_base == 0) {
        _rtt_base = usToPs(_cfg.alpha_k_rtt_ref_C_us);
    }
    _real_time_rtt = _rtt_base;
    _last_real_time_rtt_update_ts = 0;
    _last_rate_increase_ts = 0;

    _rate_prev_bytes_per_ps = gbpsToBytesPerPs(_cfg.rate_init_gbps);
    _rate_cur_bytes_per_ps = gbpsToBytesPerPs(_cfg.rate_init_gbps);
    _tx_bytes_increase_accum_bytes = 0;
    _tx_bytes_inflight_accum_bytes = 0;
    _cnp_recv = false;
    _cnt_alpha = 0;
    _fn_inflight = false;

    recomputeAlphaK();
    clampRate();
}

void Barre::dumpParams(std::ostream& os) const {
    os << "[CC_PARAM] algo=barre param=alpha_base_increase_gbps value="
       << _cfg.alpha_base_increase_gbps << "\n"
       << "[CC_PARAM] algo=barre param=alpha_fast_increase_A_gbps value="
       << _cfg.alpha_fast_increase_A_gbps << "\n"
       << "[CC_PARAM] algo=barre param=beta_cnp_decrease_ratio value="
       << _cfg.beta_cnp_decrease_ratio << "\n"
       << "[CC_PARAM] algo=barre param=fast_increase_rounds_threshold_T value="
       << _cfg.fast_increase_rounds_threshold_T << "\n"
       << "[CC_PARAM] algo=barre param=dual_lock_tx_bytes_threshold_N_bytes value="
       << _cfg.dual_lock_tx_bytes_threshold_N_bytes << "\n"
       << "[CC_PARAM] algo=barre param=alpha_k_rtt_ref_C_us value="
       << _cfg.alpha_k_rtt_ref_C_us << "\n"
       << "[CC_PARAM] algo=barre param=quarter_decrease_phi value="
       << _cfg.quarter_decrease_phi << "\n"
       << "[CC_PARAM] algo=barre param=rate_init_gbps value="
       << _cfg.rate_init_gbps << "\n"
       << "[CC_PARAM] algo=barre param=rate_min_gbps value="
       << _cfg.rate_min_gbps << "\n"
       << "[CC_PARAM] algo=barre param=rate_max_gbps value="
       << _cfg.rate_max_gbps << "\n"
       << "[CC_PARAM] algo=barre param=rtt_base_us value=" << psToUs(_rtt_base) << "\n"
       << "[CC_PARAM] algo=barre param=real_time_rtt_us value=" << psToUs(_real_time_rtt) << "\n"
       << "[CC_PARAM] algo=barre param=alpha_k_gbps value=" << bytesPerPsToGbps(_alpha_k_bytes_per_ps) << "\n"
       << "[CC_PARAM] algo=barre param=rate_cur_gbps value=" << bytesPerPsToGbps(_rate_cur_bytes_per_ps) << "\n";
}

void Barre::clampRate() {
    const double rate_min_bytes_per_ps = gbpsToBytesPerPs(_cfg.rate_min_gbps);
    const double rate_max_bytes_per_ps = gbpsToBytesPerPs(_cfg.rate_max_gbps);
    if (_rate_cur_bytes_per_ps < rate_min_bytes_per_ps) {
        _rate_cur_bytes_per_ps = rate_min_bytes_per_ps;
    }
    if (_rate_cur_bytes_per_ps > rate_max_bytes_per_ps) {
        _rate_cur_bytes_per_ps = rate_max_bytes_per_ps;
    }
}

void Barre::recomputeAlphaK() {
    if (_cfg.alpha_k_rtt_ref_C_us <= 0.0) {
        _cfg.alpha_k_rtt_ref_C_us = 1.0;
    }
    simtime_picosec alpha_k_rtt_ref_c_ps = usToPs(_cfg.alpha_k_rtt_ref_C_us);
    if (alpha_k_rtt_ref_c_ps == 0) {
        alpha_k_rtt_ref_c_ps = timeFromUs((uint32_t)1);
    }
    const double alpha_base_increase_bytes_per_ps =
        gbpsToBytesPerPs(_cfg.alpha_base_increase_gbps);
    _alpha_k_bytes_per_ps = (static_cast<double>(_rtt_base) *
                             alpha_base_increase_bytes_per_ps) /
                            static_cast<double>(alpha_k_rtt_ref_c_ps);
    if (_alpha_k_bytes_per_ps <= 0.0) {
        _alpha_k_bytes_per_ps = alpha_base_increase_bytes_per_ps;
    }
}

void Barre::onAck(simtime_picosec raw_rtt, mem_b, bool, mem_b) {
    if (raw_rtt == 0) {
        return;
    }
    simtime_picosec now = _eventlist.now();
    if (_real_time_rtt == 0 || (now - _last_real_time_rtt_update_ts) > _real_time_rtt) {
        _real_time_rtt = raw_rtt;
        _last_real_time_rtt_update_ts = now;
        if (_rtt_base == 0 || _real_time_rtt < _rtt_base) {
            _rtt_base = _real_time_rtt;
            recomputeAlphaK();
        }
        _tx_bytes_inflight_accum_bytes = 0;
    }
}

void Barre::onNack(mem_b, bool, mem_b) {
    // No-op by design.
}

void Barre::onTimeout(mem_b) {
    // No-op by design.
}

void Barre::onTx(mem_b pkt_bytes) {
    if (pkt_bytes == 0) {
        return;
    }

    _rate_prev_bytes_per_ps = _rate_cur_bytes_per_ps;
    _tx_bytes_increase_accum_bytes += pkt_bytes;
    _tx_bytes_inflight_accum_bytes += pkt_bytes;

    simtime_picosec now = _eventlist.now();
    simtime_picosec effective_rtt = _real_time_rtt > 0 ? _real_time_rtt : _rtt_base;
    if (effective_rtt == 0) {
        effective_rtt = timeFromUs((uint32_t)1);
    }

    if (_tx_bytes_increase_accum_bytes > _cfg.dual_lock_tx_bytes_threshold_N_bytes &&
        (now - _last_rate_increase_ts) > effective_rtt) {
        if (!_cnp_recv) {
            if (_cnt_alpha > _cfg.fast_increase_rounds_threshold_T) {
                _rate_cur_bytes_per_ps =
                    _rate_prev_bytes_per_ps + gbpsToBytesPerPs(_cfg.alpha_fast_increase_A_gbps);
            } else {
                _rate_cur_bytes_per_ps = _rate_prev_bytes_per_ps + _alpha_k_bytes_per_ps;
            }
            _cnt_alpha++;
            clampRate();
        }

        _cnp_recv = false;
        _tx_bytes_increase_accum_bytes = 0;
        _last_rate_increase_ts = now;
    }

    const double inflight_threshold =
        _cfg.quarter_decrease_phi * _rate_cur_bytes_per_ps * static_cast<double>(effective_rtt);
    if (!_fn_inflight &&
        static_cast<double>(_tx_bytes_inflight_accum_bytes) > inflight_threshold) {
        _fn_inflight = true;
        _rate_cur_bytes_per_ps = _rate_prev_bytes_per_ps * 0.25;
        clampRate();
    }
}

void Barre::onCnp() {
    _rate_cur_bytes_per_ps = _rate_prev_bytes_per_ps * _cfg.beta_cnp_decrease_ratio;
    _cnp_recv = true;
    _cnt_alpha = 0;
    _tx_bytes_inflight_accum_bytes = 0;
    _tx_bytes_increase_accum_bytes = 0;

    if (_fn_inflight) {
        _rate_cur_bytes_per_ps = _rate_prev_bytes_per_ps * 4.0;
        _fn_inflight = false;
    }
    clampRate();
}
