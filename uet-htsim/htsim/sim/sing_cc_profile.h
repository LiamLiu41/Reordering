// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef SING_CC_PROFILE_H
#define SING_CC_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "config.h"

struct GlobalNetworkParams {
    simtime_picosec network_rtt_ps = 0;
    mem_b network_bdp_bytes = 0;
    linkspeed_bps network_linkspeed_bps = 0;
    bool trimming_enabled = false;
    mem_b default_mtu_bytes = 0;
    mem_b default_mss_bytes = 0;
};

struct FlowBasicParams {
    simtime_picosec peer_rtt_ps = 0;
    mem_b bdp_bytes = 0;
    linkspeed_bps nic_linkspeed_bps = 0;
    mem_b mtu_bytes = 0;
    mem_b mss_bytes = 0;
    mem_b init_cwnd = 0;
};

enum class CcAlgoId {
    DCTCP,
    NSCC,
    CONSTANT,
    SWIFT,
    BARRE,
};

struct CcAlgoIdHash {
    std::size_t operator()(CcAlgoId algo) const noexcept {
        return static_cast<std::size_t>(algo);
    }
};

struct CcProfileParam {
    enum class Kind {
        NUMBER,
        BOOL,
        EXPR,
    };

    Kind kind = Kind::NUMBER;
    double number = 0.0;
    bool boolean = false;
    std::string expr;
};

struct CcProfile {
    std::string id;
    CcAlgoId algo = CcAlgoId::NSCC;
    std::unordered_map<std::string, CcProfileParam> params;
};

struct FlowCcOverrideInput {
    std::optional<CcAlgoId> algo;
    std::optional<std::string> profile_id;
};

struct FlowCcSelectionSpec {
    CcAlgoId algo = CcAlgoId::NSCC;
    std::optional<CcProfile> profile;
};

class CcProfileStore {
public:
    bool loadFromJsonFile(const std::string& path, std::string* error = nullptr);

    std::optional<CcProfile> findById(const std::string& profile_id) const;
    std::optional<CcProfile> findDefaultForAlgo(CcAlgoId algo) const;

    bool empty() const { return _profiles.empty(); }

private:
    std::unordered_map<CcAlgoId, std::string, CcAlgoIdHash> _defaults;
    std::unordered_map<std::string, CcProfile> _profiles;
};

class CcProfileFlowSelectionResolver {
public:
    static const char* algoName(CcAlgoId algo);
    static bool parseAlgo(const std::string& algo, CcAlgoId* out);

    static bool resolve(const FlowCcOverrideInput& input,
                        CcAlgoId global_algo,
                        const CcProfileStore* store,
                        FlowCcSelectionSpec* out,
                        std::string* error = nullptr);
};

class CcProfileResolver {
public:
    static double getDouble(const CcProfile& profile,
                            const std::string& key,
                            double default_value,
                            const GlobalNetworkParams& global,
                            const FlowBasicParams& flow);

    static mem_b getMemB(const CcProfile& profile,
                         const std::string& key,
                         mem_b default_value,
                         const GlobalNetworkParams& global,
                         const FlowBasicParams& flow);

    static simtime_picosec getTimePs(const CcProfile& profile,
                                     const std::string& key,
                                     simtime_picosec default_value,
                                     const GlobalNetworkParams& global,
                                     const FlowBasicParams& flow);

    static uint32_t getUint32(const CcProfile& profile,
                              const std::string& key,
                              uint32_t default_value,
                              const GlobalNetworkParams& global,
                              const FlowBasicParams& flow);

    static uint8_t getUint8(const CcProfile& profile,
                            const std::string& key,
                            uint8_t default_value,
                            const GlobalNetworkParams& global,
                            const FlowBasicParams& flow);

    static bool getBool(const CcProfile& profile,
                        const std::string& key,
                        bool default_value,
                        const GlobalNetworkParams& global,
                        const FlowBasicParams& flow);

private:
    static std::optional<double> resolve(const CcProfile& profile,
                                         const std::string& key,
                                         const GlobalNetworkParams& global,
                                         const FlowBasicParams& flow);
};

#endif
