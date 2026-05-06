// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_CC_H
#define SING_CC_H

#include <cstdint>
#include <iosfwd>

#include "config.h"
#include "eventlist.h"
#include "sing_cc_profile.h"

class BaseCC {
public:
    virtual ~BaseCC() = default;

    virtual void initCcParams(const GlobalNetworkParams& global,
                              const FlowBasicParams& params,
                              const CcProfile& profile) = 0;

    virtual void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) = 0;
    virtual void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) = 0;
    virtual void onTimeout(mem_b in_flight) = 0;
    virtual void onTx(mem_b pkt_bytes) {}
    virtual void onCnp() {}

    virtual const char* name() const = 0;
    virtual void dumpParams(std::ostream& os) const = 0;

    virtual bool hasWindow() const = 0;
    virtual bool hasRate() const = 0;
    virtual mem_b cwndBytes() const = 0;
    virtual double rateBytesPerPs() const = 0;
};

class Constant : public BaseCC {
public:
    struct Config {
        double rate_bytes_per_ps = 0.0;
        mem_b init_cwnd = 0;
    };

    Constant();

    void initCcParams(const GlobalNetworkParams& global,
                      const FlowBasicParams& params,
                      const CcProfile& profile) override;

    void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) override;
    void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) override;
    void onTimeout(mem_b in_flight) override;

    const char* name() const override { return "constant"; }
    void dumpParams(std::ostream& os) const override;
    bool hasWindow() const override { return false; }
    bool hasRate() const override { return true; }
    mem_b cwndBytes() const override { return 0; }
    double rateBytesPerPs() const override { return _cfg.rate_bytes_per_ps; }

private:
    Config _cfg;
};

class Dctcp : public BaseCC {
public:
    struct Config {
        mem_b min_cwnd = 0;
        mem_b init_cwnd = 0;
        mem_b ai_unit = 0;
    };

    Dctcp();

    void initCcParams(const GlobalNetworkParams& global,
                      const FlowBasicParams& params,
                      const CcProfile& profile) override;

    void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) override;
    void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) override;
    void onTimeout(mem_b in_flight) override;

    const char* name() const override { return "dctcp"; }
    void dumpParams(std::ostream& os) const override;
    bool hasWindow() const override { return true; }
    bool hasRate() const override { return false; }
    mem_b cwndBytes() const override { return _cwnd; }
    double rateBytesPerPs() const override { return 0.0; }

private:
    Config _cfg;
    mem_b _cwnd;
};

class Nscc : public BaseCC {
public:
    struct Config {
        mem_b min_cwnd = 0;
        mem_b max_cwnd = 0;
        simtime_picosec target_qdelay = 0;
        double gamma = 0.8;
        double alpha = 0.0;
        double fi = 0.0;
        double fi_scale = 0.0;
        double eta = 0.0;
        double qa_threshold = 0.0;
        double delay_alpha = 0.0125;
        uint32_t adjust_bytes_threshold = 0;
        simtime_picosec adjust_period_threshold = 0;
        uint32_t qa_scaling = 1;
        bool disable_quick_adapt = false;
        uint8_t qa_gate = 3;
        simtime_picosec network_rtt = 0;
    };

    Nscc(EventList& ev, simtime_picosec base_rtt);

    void initCcParams(const GlobalNetworkParams& global,
                      const FlowBasicParams& params,
                      const CcProfile& profile) override;

    void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) override;
    void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) override;
    void onTimeout(mem_b in_flight) override;

    const char* name() const override { return "nscc"; }
    void dumpParams(std::ostream& os) const override;
    bool hasWindow() const override { return true; }
    bool hasRate() const override { return false; }
    mem_b cwndBytes() const override { return _cwnd; }
    double rateBytesPerPs() const override { return 0.0; }

    // Exposed for migration/debug parity.
    simtime_picosec avgDelay() const { return _avg_delay; }
    simtime_picosec targetQdelay() const { return _cfg.target_qdelay; }

private:
    bool quickAdapt(bool is_loss, bool skip, simtime_picosec delay, mem_b in_flight);
    void fairIncrease(uint32_t newly_acked_bytes);
    void proportionalIncrease(uint32_t newly_acked_bytes, simtime_picosec delay);
    void fastIncrease(uint32_t newly_acked_bytes, simtime_picosec delay);
    void multiplicativeDecrease();
    void fulfillAdjustment();
    void setCwndBounds();
    void updateDelay(simtime_picosec raw_rtt, bool update_avg, bool skip);

    EventList& _eventlist;
    Config _cfg;

    mem_b _cwnd;
    simtime_picosec _base_rtt;

    mem_b _achieved_bytes = 0;
    mem_b _received_bytes = 0;
    uint32_t _fi_count = 0;
    bool _trigger_qa = false;
    simtime_picosec _qa_endtime = 0;
    uint32_t _bytes_to_ignore = 0;
    uint32_t _bytes_ignored = 0;
    uint32_t _inc_bytes = 0;
    simtime_picosec _avg_delay = 0;
    simtime_picosec _last_adjust_time = 0;
    simtime_picosec _last_dec_time = 0;
    simtime_picosec _last_eta_time = 0;
    bool _increase = false;
};

class Swift : public BaseCC {
public:
    struct Config {
        // Window bounds (active in current implementation).
        mem_b min_cwnd = 0;
        mem_b max_cwnd = 0;
        // Initial cwnd (active).
        mem_b init_cwnd = 0;
        // Packet quantum used by fs_delay(cwnd/pkt_size) and AI step (active).
        mem_b pkt_size = 0;
        // Base target delay. In Phase C this is injected from per-flow base RTT (active).
        simtime_picosec base_target_delay = 0;
        // Delay-based AIMD parameters (active).
        double ai = 1.0;
        double beta = 0.8;
        double max_mdf = 0.5;
        // Timeout backoff threshold (active in onTimeout()).
        uint32_t rtx_reset_threshold = 5;
        // Flow-scaling range endpoints from original Swift formula (active).
        double fs_min_cwnd = 0.1;
        double fs_max_cwnd = 100.0;
    };

    Swift(EventList& ev, simtime_picosec base_rtt);

    void initCcParams(const GlobalNetworkParams& global,
                      const FlowBasicParams& params,
                      const CcProfile& profile) override;

    // Called from SingSrc::processAck() on each ACK sample.
    // Current behavior: raw_rtt-based AIMD update + cwnd bounds.
    void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) override;
    // Called from SingSrc::processNack() when a NACK is processed.
    // Current behavior: one multiplicative decrease (no FR state machine).
    void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) override;
    // Called from SingSrc::rtxTimerExpired() on RTO timeout.
    // Current behavior: timeout-based backoff/reset using rtx_reset_threshold.
    void onTimeout(mem_b in_flight) override;

    const char* name() const override { return "swift"; }
    void dumpParams(std::ostream& os) const override;
    bool hasWindow() const override { return true; }
    bool hasRate() const override { return false; }
    mem_b cwndBytes() const override { return _cwnd; }
    double rateBytesPerPs() const override { return 0.0; }

private:
    void setCwndBounds();
    // RTT estimator retained from Swift update rules; currently used for decrease gating.
    void updateRtt(simtime_picosec raw_rtt);
    // Phase C target delay: base_target_delay + fs_delay(cwnd), no hop component.
    simtime_picosec targetDelay() const;

    // Active: used for timing gates and now() in onAck/onNack/onTimeout.
    EventList& _eventlist;
    // Active: immutable parameter bundle for this CC instance.
    Config _cfg;

    // Active: output window exposed to Sing scheduler path.
    mem_b _cwnd;
    // Active: per-flow base RTT injected by SingSrc::initCcForFlow().
    simtime_picosec _base_rtt;
    // Active: smoothed RTT for gating "one decrease per RTT".
    simtime_picosec _rtt = 0;
    // Reserved/diagnostic in Phase C: updated by updateRtt(), not consumed by external timeout scheduler.
    simtime_picosec _rto = 0;
    // Reserved/diagnostic in Phase C: floor for _rto.
    simtime_picosec _min_rto = 0;
    // Reserved/diagnostic in Phase C: RTT variation estimator.
    simtime_picosec _mdev = 0;
    // Active: last MD timestamp for decrease gating.
    simtime_picosec _last_decrease = 0;
    // Active: timeout backoff counter used by onTimeout().
    uint32_t _retransmit_cnt = 0;

    // Active: fs_delay(cwnd) parameterization (same formula shape as original Swift).
    simtime_picosec _fs_range = 0;
    double _fs_alpha = 0.0;
    double _fs_beta = 0.0;
};

class Barre : public BaseCC {
public:
    struct Config {
        double alpha_base_increase_gbps = 0.0;
        double alpha_fast_increase_A_gbps = 0.0;
        double beta_cnp_decrease_ratio = 0.8;
        uint32_t fast_increase_rounds_threshold_T = 3;
        mem_b dual_lock_tx_bytes_threshold_N_bytes = 8 * 1024;
        double alpha_k_rtt_ref_C_us = 1.0;
        double quarter_decrease_phi = 1.5;
        double rate_init_gbps = 0.0;
        double rate_min_gbps = 0.0;
        double rate_max_gbps = 0.0;
    };

    Barre(EventList& ev, simtime_picosec base_rtt);

    void initCcParams(const GlobalNetworkParams& global,
                      const FlowBasicParams& params,
                      const CcProfile& profile) override;

    void onAck(simtime_picosec raw_rtt, mem_b acked_bytes, bool ecn, mem_b in_flight) override;
    void onNack(mem_b nacked_bytes, bool last_hop, mem_b in_flight) override;
    void onTimeout(mem_b in_flight) override;
    void onTx(mem_b pkt_bytes) override;
    void onCnp() override;

    const char* name() const override { return "barre"; }
    void dumpParams(std::ostream& os) const override;
    bool hasWindow() const override { return false; }
    bool hasRate() const override { return true; }
    mem_b cwndBytes() const override { return 0; }
    double rateBytesPerPs() const override { return _rate_cur_bytes_per_ps; }

private:
    void clampRate();
    void recomputeAlphaK();

    EventList& _eventlist;
    Config _cfg;

    simtime_picosec _rtt_base = 0;
    simtime_picosec _real_time_rtt = 0;
    simtime_picosec _last_real_time_rtt_update_ts = 0;
    simtime_picosec _last_rate_increase_ts = 0;

    double _rate_prev_bytes_per_ps = 0.0;
    double _rate_cur_bytes_per_ps = 0.0;
    double _alpha_k_bytes_per_ps = 0.0;

    mem_b _tx_bytes_increase_accum_bytes = 0;
    mem_b _tx_bytes_inflight_accum_bytes = 0;
    bool _cnp_recv = false;
    uint32_t _cnt_alpha = 0;
    bool _fn_inflight = false;
};

#endif
