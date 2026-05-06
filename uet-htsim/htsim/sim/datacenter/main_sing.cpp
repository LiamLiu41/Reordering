// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//#include "config.h"
#include <cassert>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string.h>
#include <iomanip>

#include <math.h>
#include <unistd.h>
#include "network.h"
#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "sing_logger.h"
#include "clock.h"
#include "sing_base.h"
#include "sing_src.h"
#include "sing_sink.h"
#include "sing_nic.h"
#include "sing_mp.h"
#include "sing_pdcses.h"
#include "sing_switch.h"
#include "compositequeue.h"
#include "topology.h"
#include "connection_matrix.h"




#include "fat_tree_topology.h"
#include "fat_tree_switch.h"

#include <list>

// Simulation params

//#define PRINTPATHS 1

#include "main.h"

int DEFAULT_NODES = 128;
uint32_t DEFAULT_TRIMMING_QUEUESIZE_FACTOR = 1;
uint32_t DEFAULT_NONTRIMMING_QUEUESIZE_FACTOR = 5;
// #define DEFAULT_CWND 50

EventList eventlist;

namespace {

string flowContext(const connection& c) {
    stringstream ss;
    ss << "src=" << c.src << " dst=" << c.dst;
    if (c.flowid != 0) {
        ss << " flowid=" << c.flowid;
    }
    if (c.cm_line != 0) {
        ss << " line=" << c.cm_line;
    }
    return ss.str();
}

[[noreturn]] void failFlowCcConfig(const connection& c, const string& message) {
    cerr << "CC flow override error (" << flowContext(c) << "): " << message << endl;
    exit(1);
}

bool hasFlowCcOverride(const connection& c) {
    return c.cc_algo.has_value() || c.cc_profile.has_value();
}

}  // namespace

void exit_error(char* progr) {
    cout << "Usage " << progr << " [-nodes N]\n\t[-cwnd cwnd_size]\n\t[-max_unack_window_pkts N]\n\t[-q queue_size]\n\t[-queue_type composite|random|lossless|lossless_input|]\n\t[-tm traffic_matrix_file]\n\t[-strat route_strategy (single,rand,perm,ecmp,\n\tecmp_host path_count,ecmp_ar,ecmp_rr,\n\tecmp_host_ar ar_thresh)]\n\t[-log log_level]\n\t[-seed random_seed]\n\t[-end end_time_in_usec]\n\t[-mtu MTU]\n\t[-hop_latency x] per hop wire latency in us,default 1\n\t[-switch_latency x] switching latency in us, default 0\n\t[-host_queue_type  swift|prio|fair_prio]\n\t[-cc_profile filename] CC profile JSON file\n\t[-dump_cc_params] dump CC parameters for first flow\n\t[-logtime dt] sample time for sinklogger, etc\n\t[-conn_reuse] enable connection reuse\n\t[-path_selection_granularity x] path selection granularity in bytes (0=per-packet)\n\t[-reps_buffer_size x] REPS circular buffer size (default 8)\n\t[-intra_granularity_behavior keep|random] behavior within granularity (default: keep)\n\t[-ecn_penalty x] ECN penalty for bitmap load balancing (default 4)\n\t[-failed x] number of failed links\n\t[-failed_link_ratio x] speed ratio for failed links (default 0.25 = 25%)\n\t[-pfc_alpha x] dynamic-threshold alpha for shared-buffer PFC\n\t[-pfc_resume_offset bytes] resume offset (default 3*MTU)\n\t[-log_pfc_stats] print per-switch PFC stats at simulation end\n\t[-interest_file filename] file containing queue names to log" << endl;
    exit(1);
}

simtime_picosec calculate_rtt(FatTreeTopologyCfg* t_cfg, linkspeed_bps host_linkspeed) { 
    /*
    Using the host linkspeed here is not very accurate, but hopefully good enough for this usecase.
    */
    simtime_picosec rtt = 2 * t_cfg->get_diameter_latency() 
                + (Packet::data_packet_size() * 8 / speedAsGbps(host_linkspeed) * t_cfg->get_diameter() * 1000) 
                + (SingBasePacket::get_ack_size() * 8 / speedAsGbps(host_linkspeed) * t_cfg->get_diameter() * 1000);
    
    return rtt;
};

// Calculate RTT between two specific hosts
simtime_picosec calculate_rtt_between_hosts(FatTreeTopologyCfg* t_cfg, uint32_t src, uint32_t dst, linkspeed_bps host_linkspeed) {
    if (src == dst) {
        // Same host, no network traversal
        return 0;
    }
    
    // Use the existing method to get propagation + switch delay between two points
    simtime_picosec base_latency = t_cfg->get_two_point_diameter_latency(src, dst);
    
    // Add serialization delay for ACK packet only
    // Note: Data serialization is already included in transmission_time
    // get_two_point_diameter_latency includes one-way propagation + switch delay
    simtime_picosec ack_serialization = (SingBasePacket::get_ack_size() * 8 * 1e12) / host_linkspeed;
    
    // RTT = 2 * base_latency (round trip) + ACK serialization
    simtime_picosec rtt = 2 * base_latency + ack_serialization;
    
    return rtt;
}

// Calculate one-way latency between hosts (for receiver perspective)
simtime_picosec calculate_one_way_latency(FatTreeTopologyCfg* t_cfg, uint32_t src, uint32_t dst) {
    if (src == dst) {
        // Same host, no network traversal
        return 0;
    }
    
    // Use the existing method to get propagation + switch delay between two points
    // This is the pure one-way latency without any serialization overhead
    simtime_picosec base_latency = t_cfg->get_two_point_diameter_latency(src, dst);
    
    return base_latency;
}

// Calculate ideal completion time for a flow: transmission_time + rtt
simtime_picosec calculate_ideal_time(FatTreeTopologyCfg* t_cfg, uint32_t src, uint32_t dst, 
                                      uint32_t flow_size, linkspeed_bps host_linkspeed) {
    // Transmission time: time to send all data at line rate
    simtime_picosec transmission_time = (flow_size * 8 * 1e12) / host_linkspeed;
    
    // RTT between src and dst
    simtime_picosec rtt = calculate_rtt_between_hosts(t_cfg, src, dst, host_linkspeed);
    
    // Ideal time = transmission time + one RTT (for the first packet to go and ACK to come back)
    simtime_picosec ideal_time = transmission_time + rtt;
    
    return ideal_time;
}

// Calculate one-way ideal time for a flow: transmission_time + one_way_latency
simtime_picosec calculate_one_way_ideal_time(FatTreeTopologyCfg* t_cfg, uint32_t src, uint32_t dst, 
                                              uint32_t flow_size, linkspeed_bps host_linkspeed) {
    // Transmission time: time to send all data at line rate
    simtime_picosec transmission_time = (flow_size * 8 * 1e12) / host_linkspeed;
    
    // One-way latency between src and dst (pure propagation + switch delay)
    simtime_picosec one_way_latency = calculate_one_way_latency(t_cfg, src, dst);
    
    // One-way ideal time = transmission time + one-way latency
    simtime_picosec one_way_ideal_time = transmission_time + one_way_latency;
    
    return one_way_ideal_time;
}

uint32_t calculate_bdp_pkt(FatTreeTopologyCfg* t_cfg, linkspeed_bps host_linkspeed) {
    simtime_picosec rtt = calculate_rtt(t_cfg, host_linkspeed);
    uint32_t bdp_pkt = ceil((timeAsSec(rtt) * (host_linkspeed/8)) / (double)Packet::data_packet_size()); 

    return bdp_pkt;
}

uint32_t calculate_bdp_pkt_with_rtt(simtime_picosec rtt, linkspeed_bps host_linkspeed) {
    uint32_t bdp_pkt = ceil((timeAsSec(rtt) * (host_linkspeed/8)) / (double)Packet::data_packet_size()); 
    return bdp_pkt;
}

int main(int argc, char **argv) {
    Clock c(timeFromSec(5 / 100.), eventlist);
    bool param_queuesize_set = false;
    uint32_t queuesize_pkt = 0;
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    int packet_size = 4150;
    uint32_t path_entropy_size = 64;
    uint32_t cwnd = 0, no_of_nodes = 0;
    uint32_t tiers = 3; // we support 2 and 3 tier fattrees
    uint32_t planes = 1;  // multi-plane topologies
    uint32_t ports = 1;  // ports per NIC
    bool disable_trim = false; // Disable trimming, drop instead
    uint16_t trimsize = 64; // size of a trimmed packet
    string switch_model = "fattree";
    uint64_t switch_buffer_mb = 64;
    uint8_t pfc_mask = 0x03;
    double pfc_alpha = 0.125;
    uint64_t pfc_resume_offset = 0;
    bool pfc_resume_offset_set = false;
    uint64_t pfc_xoff = 0;
    uint64_t pfc_xon = 0;
    uint64_t pfc_headroom = 0;
    uint32_t rto_min_us = 0;
    uint32_t max_unack_window_pkts = 0;
    simtime_picosec logtime = timeFromMs(0.25); // ms;
    stringstream filename(ios_base::out);
    simtime_picosec hop_latency = timeFromUs((uint32_t)1);
    simtime_picosec switch_latency = timeFromUs((uint32_t)0);
    queue_type qt = COMPOSITE;

    enum LoadBalancing_Algo { BITMAP, BITMAP_ACKAGG, REPS, REPS_ACKAGG, REPS_LEGACY, OBLIVIOUS, MIXED};
    LoadBalancing_Algo load_balancing_algo = MIXED;

    bool log_sink = false;
    bool log_nic = false;
    bool log_flow_events = true;

    bool log_tor_downqueue = false;
    bool log_tor_upqueue = false;
    bool log_traffic = false;
    bool log_switches = false;
    bool log_queue_usage = false;
    bool log_pfc_stats = false;
    const double ecn_thresh = 0.5; // default marking threshold for ECN load balancing

    bool param_ecn_set = false;
    bool ecn = true;
    uint32_t ecn_low = 0;
    uint32_t ecn_high = 0;
    uint32_t queue_size_bdp_factor = 0;
    uint32_t topo_num_failed = 0;
    double failed_link_ratio = 0.25;  // default 25% speed for failed links

    double user_specified_rtt_us = 0.0;
    bool use_user_rtt = false;
    
    RouteStrategy route_strategy = NOT_SET;
    
    int seed = 13;
    int i = 1;

    filename << "logout.dat";
    int end_time = 1000;//in microseconds
    bool enable_accurate_base_rtt = false;

    //unsure how to set this. 
    queue_type snd_type = FAIR_PRIO;

    float ar_sticky_delta = 10;
    FatTreeSwitch::sticky_choices ar_sticky = FatTreeSwitch::PER_PACKET;

    char* tm_file = NULL;
    char* topo_file = NULL;
    char* interest_file = NULL;
    char* cc_profile_file = NULL;
    std::shared_ptr<CcProfileStore> cc_profile_store = nullptr;
    bool conn_reuse = false;
    mem_b path_selection_granularity = 0;  // 0 means per-packet (default)
    SingMultipath::IntraGranularityBehavior intra_granularity_behavior = SingMultipath::KEEP_PATH;
    uint8_t ecn_penalty = 1;  // default ECN penalty for bitmap load balancing
    uint16_t reps_buffer_size = CircularBufferREPS<uint16_t>::repsBufferSize;

    while (i<argc) {
        if (!strcmp(argv[i],"-o")) {
            filename.str(std::string());
            filename << argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-conn_reuse")){
            conn_reuse = true;
            cout << "Enabling connection reuse" << endl;
        } else if (!strcmp(argv[i],"-end")) {
            end_time = atoi(argv[i+1]);
            cout << "endtime(us) "<< end_time << endl;
            i++;            
        } else if (!strcmp(argv[i],"-nodes")) {
            no_of_nodes = atoi(argv[i+1]);
            cout << "no_of_nodes "<<no_of_nodes << endl;
            i++;
        } else if (!strcmp(argv[i],"-tiers")) {
            tiers = atoi(argv[i+1]);
            cout << "tiers " << tiers << endl;
            assert(tiers == 2 || tiers == 3);
            i++;
        } else if (!strcmp(argv[i],"-planes")) {
            planes = atoi(argv[i+1]);
            ports = planes;
            cout << "planes " << planes << endl;
            cout << "ports per NIC " << ports << endl;
            assert(planes >= 1 && planes <= 8);
            i++;
        } else if (!strcmp(argv[i],"-sender_cc_only")) {
            SingSrc::_sender_based_cc = true;
            cout << "sender based CC enabled ONLY" << endl;
        } else if (!strcmp(argv[i],"-queue_size_bdp_factor")) {
            queue_size_bdp_factor = atoi(argv[i+1]);
            cout << "Setting queue size to "<< queue_size_bdp_factor << "x BDP." << endl;
            i++;
        } else if (!strcmp(argv[i],"-rtt_us")) {
            user_specified_rtt_us = atof(argv[i+1]);
            use_user_rtt = true;
            cout << "Using user-specified RTT: " << user_specified_rtt_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i],"-sender_cc_algo")) {
            SingSrc::_sender_based_cc = true;

            CcAlgoId parsed_algo;
            if (!CcProfileFlowSelectionResolver::parseAlgo(argv[i+1], &parsed_algo)) {
                cout << "UNKNOWN CC ALGO " << argv[i+1]
                     << ", expected one of nscc|dctcp|constant|swift|barre" << endl;
                exit(1);
            }
            SingSrc::_sender_cc_algo = parsed_algo;
            cout << "sender based algo " << CcProfileFlowSelectionResolver::algoName(parsed_algo) << endl;
            i++;
        } else if (!strcmp(argv[i],"-sender_cc")) {
            SingSrc::_sender_based_cc = true;
            cout << "sender based CC enabled " << endl;
        } else if (!strcmp(argv[i],"-cc_profile")) {
            cc_profile_file = argv[i+1];
            cout << "CC profile file: " << cc_profile_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-dump_cc_params")) {
            SingSrc::_dump_cc_params = true;
        }
        else if (!strcmp(argv[i],"-load_balancing_algo")){
            if (!strcmp(argv[i+1], "bitmap")) {
                load_balancing_algo = BITMAP;
            }
            else if (!strcmp(argv[i+1], "bitmap_ackagg")) {
                load_balancing_algo = BITMAP_ACKAGG;
            }
            else if (!strcmp(argv[i+1], "reps")) {
                load_balancing_algo = REPS;
            }
            else if (!strcmp(argv[i+1], "reps_ackagg")) {
                load_balancing_algo = REPS_ACKAGG;
            }
            else if (!strcmp(argv[i+1], "reps_legacy")) {
                load_balancing_algo = REPS_LEGACY;
            }
            else if (!strcmp(argv[i+1], "oblivious")) {
                load_balancing_algo = OBLIVIOUS;
            }
            else if (!strcmp(argv[i+1], "mixed")) {
                load_balancing_algo = MIXED;
            }
            else {
                cout << "Unknown load balancing algorithm of type " << argv[i+1]
                     << ", expecting bitmap, bitmap_ackagg, reps, reps_ackagg, reps_legacy, oblivious or mixed" << endl;
                exit_error(argv[0]);
            }
            cout << "Load balancing algorithm set to  "<< argv[i+1] << endl;
            i++;
        }
        else if (!strcmp(argv[i],"-queue_type")) {
            if (!strcmp(argv[i+1], "composite")) {
                qt = COMPOSITE;
            } 
            else if (!strcmp(argv[i+1], "composite_ecn")) {
                qt = COMPOSITE_ECN;
            }
            else if (!strcmp(argv[i+1], "aeolus")){
                qt = AEOLUS;
            }
            else if (!strcmp(argv[i+1], "aeolus_ecn")){
                qt = AEOLUS_ECN;
            }
            else {
                cout << "Unknown queue type " << argv[i+1] << endl;
                exit_error(argv[0]);
            }
            cout << "queue_type "<< qt << endl;
            i++;
        } else if (!strcmp(argv[i],"-debug")) {
            SingSrc::_debug = true;
            SingNIC::_debug = true;
            SingNICPort::_debug = true;
            SingPdcSes::_debug = true;
        } else if (!strcmp(argv[i],"-debug_src")) {
            SingSrc::_debug = true;
        } else if (!strcmp(argv[i],"-debug_nic")) {
            SingNIC::_debug = true;
        } else if (!strcmp(argv[i],"-debug_port")) {
            SingNICPort::_debug = true;
        } else if (!strcmp(argv[i],"-host_queue_type")) {
            if (!strcmp(argv[i+1], "swift")) {
                snd_type = SWIFT_SCHEDULER;
            } 
            else if (!strcmp(argv[i+1], "prio")) {
                snd_type = PRIORITY;
            }
            else if (!strcmp(argv[i+1], "fair_prio")) {
                snd_type = FAIR_PRIO;
            }
            else {
                cout << "Unknown host queue type " << argv[i+1] << " expecting one of swift|prio|fair_prio" << endl;
                exit_error(argv[0]);
            }
            cout << "host queue_type "<< snd_type << endl;
            i++;
        } else if (!strcmp(argv[i],"-log")){
            if (!strcmp(argv[i+1], "flow_events")) {
                log_flow_events = true;
            } else if (!strcmp(argv[i+1], "sink")) {
                cout << "logging sinks\n";
                log_sink = true;
            } else if (!strcmp(argv[i+1], "nic")) {
                cout << "logging nics\n";
                log_nic = true;
            } else if (!strcmp(argv[i+1], "tor_downqueue")) {
                cout << "logging tor downqueues\n";
                log_tor_downqueue = true;
            } else if (!strcmp(argv[i+1], "tor_upqueue")) {
                cout << "logging tor upqueues\n";
                log_tor_upqueue = true;
            } else if (!strcmp(argv[i+1], "switch")) {
                cout << "logging total switch queues\n";
                log_switches = true;
            } else if (!strcmp(argv[i+1], "traffic")) {
                cout << "logging traffic\n";
                log_traffic = true;
            } else if (!strcmp(argv[i+1], "queue_usage")) {
                cout << "logging queue usage\n";
                log_queue_usage = true;
            } else {
                exit_error(argv[0]);
            }
            i++;
        } else if (!strcmp(argv[i],"-no_receiver_fct")) {
            cout << "Disabling receiver-side FCT logging\n";
            SingSrc::_print_receiver_fct = false;
        } else if (!strcmp(argv[i],"-cwnd")) {
            cwnd = atoi(argv[i+1]);
            cout << "cwnd "<< cwnd << endl;
            i++;
        } else if (!strcmp(argv[i],"-max_unack_window_pkts")) {
            int pkts = atoi(argv[i+1]);
            if (pkts < 0) {
                cout << "Invalid -max_unack_window_pkts " << pkts << ", expected >= 0" << endl;
                exit_error(argv[0]);
            }
            max_unack_window_pkts = static_cast<uint32_t>(pkts);
            cout << "max_unack_window set to " << max_unack_window_pkts << " * MTU" << endl;
            i++;
        } else if (!strcmp(argv[i],"-tm")){
            tm_file = argv[i+1];
            cout << "traffic matrix input file: "<< tm_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-topo")){
            topo_file = argv[i+1];
            cout << "FatTree topology input file: "<< topo_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-interest_queues_file")){
            interest_file = argv[i+1];
            cout << "Interest queue file: "<< interest_file << endl;
            i++;
        } else if (!strcmp(argv[i],"-q")){
            param_queuesize_set = true;
            queuesize_pkt = atoi(argv[i+1]);
            cout << "Setting queuesize to " << queuesize_pkt << " packets " << endl;
            i++;
        }
        else if (!strcmp(argv[i],"-sack_threshold")){
            SingSink::_bytes_unacked_threshold = atoi(argv[i+1]);
            cout << "Setting receiver SACK bytes threshold to " << SingSink::_bytes_unacked_threshold  << " bytes " << endl;
            i++;            
        }
        else if (!strcmp(argv[i],"-enable_accurate_base_rtt")){
            enable_accurate_base_rtt = true;
            cout << "Enable accurate base rtt configuration, each flow uses the accurate end-to-end delay for the current sender/receiver pair as rtt upper bound." << endl;
        }
        else if (!strcmp(argv[i],"-sleek")){
            SingSrc::_enable_sleek = true;
            cout << "Using SLEEK, the sender-based fast loss recovery heuristic " << endl;
        }
        else if (!strcmp(argv[i],"-ecn")){
            // fraction of queuesize, between 0 and 1
            param_ecn_set = true;
            ecn = true;
            ecn_low = atoi(argv[i+1]); 
            ecn_high = atoi(argv[i+2]);
            i+=2;
        } else if (!strcmp(argv[i],"-disable_trim")) {
            disable_trim = true;
            cout << "Trimming disabled, dropping instead." << endl;
        } else if (!strcmp(argv[i],"-trimsize")){
            // size of trimmed packet in bytes
            trimsize = atoi(argv[i+1]);
            cout << "trimmed packet size: " << trimsize << " bytes\n";
            i+=1;
        } else if (!strcmp(argv[i],"-logtime")){
            double log_ms = atof(argv[i+1]);            
            logtime = timeFromMs(log_ms);
            cout << "logtime "<< log_ms << " ms" << endl;
            i++;
        } else if (!strcmp(argv[i],"-logtime_us")){
            double log_us = atof(argv[i+1]);            
            logtime = timeFromUs(log_us);
            cout << "logtime "<< log_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i],"-failed")){
            // number of failed links (failed to 25% linkspeed by default)
            topo_num_failed = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-failed_link_ratio")){
            // ratio of linkspeed for failed links (0.25 = 25% of original speed)
            failed_link_ratio = atof(argv[i+1]);
            cout << "Failed link ratio set to " << failed_link_ratio << " (" << (failed_link_ratio * 100) << "%)" << endl;
            i++;
        } else if (!strcmp(argv[i],"-linkspeed")){
            // linkspeed specified is in Mbps
            linkspeed = speedFromMbps(atof(argv[i+1]));
            i++;
        } else if (!strcmp(argv[i],"-seed")){
            seed = atoi(argv[i+1]);
            cout << "random seed "<< seed << endl;
            i++;
        } else if (!strcmp(argv[i],"-mtu")){
            packet_size = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-paths")){
            path_entropy_size = atoi(argv[i+1]);
            cout << "no of paths " << path_entropy_size << endl;
            i++;
        } else if (!strcmp(argv[i],"-hop_latency")){
            hop_latency = timeFromUs(atof(argv[i+1]));
            cout << "Hop latency set to " << timeAsUs(hop_latency) << endl;
            i++;
        } else if (!strcmp(argv[i],"-switch_latency")){
            switch_latency = timeFromUs(atof(argv[i+1]));
            cout << "Switch latency set to " << timeAsUs(switch_latency) << endl;
            i++;
        } else if (!strcmp(argv[i],"-switch_model")){
            switch_model = argv[i+1];
            cout << "Switch model set to " << switch_model << endl;
            i++;
        } else if (!strcmp(argv[i],"-switch_buffer")){
            switch_buffer_mb = atoi(argv[i+1]);
            cout << "Switch buffer set to " << switch_buffer_mb << " MB" << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_mask")){
            pfc_mask = atoi(argv[i+1]);
            cout << "PFC enabled mask set to " << (int)pfc_mask << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_alpha")){
            pfc_alpha = atof(argv[i+1]);
            if (pfc_alpha < 0) {
                cout << "PFC alpha must be non-negative." << endl;
                exit(1);
            }
            cout << "PFC alpha set to " << pfc_alpha << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_resume_offset")){
            pfc_resume_offset = strtoull(argv[i+1], NULL, 10);
            pfc_resume_offset_set = true;
            cout << "PFC resume offset set to " << pfc_resume_offset << " bytes" << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_xoff")){
            pfc_xoff = strtoull(argv[i+1], NULL, 10);
            cout << "PFC XOFF threshold set to " << pfc_xoff << " bytes" << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_xon")){
            pfc_xon = strtoull(argv[i+1], NULL, 10);
            cout << "PFC XON threshold set to " << pfc_xon << " bytes" << endl;
            i++;
        } else if (!strcmp(argv[i],"-pfc_headroom")){
            pfc_headroom = strtoull(argv[i+1], NULL, 10);
            cout << "PFC headroom set to " << pfc_headroom << " bytes" << endl;
            i++;
        } else if (!strcmp(argv[i],"-log_pfc_stats")){
            log_pfc_stats = true;
            cout << "Per-switch PFC stats logging enabled" << endl;
        } else if (!strcmp(argv[i],"-rto_min")){
            rto_min_us = atoi(argv[i+1]);
            cout << "Min RTO set to " << rto_min_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i],"-ar_sticky_delta")){
            ar_sticky_delta = atof(argv[i+1]);
            cout << "Adaptive routing sticky delta " << ar_sticky_delta << "us" << endl;
            i++;
        } else if (!strcmp(argv[i],"-ar_granularity")){
            if (!strcmp(argv[i+1],"packet"))
                ar_sticky = FatTreeSwitch::PER_PACKET;
            else if (!strcmp(argv[i+1],"flow"))
                ar_sticky = FatTreeSwitch::PER_FLOWLET;
            else  {
                cout << "Expecting -ar_granularity packet|flow, found " << argv[i+1] << endl;
                exit(1);
            }   
            i++;
        } else if (!strcmp(argv[i],"-ar_method")){
            if (!strcmp(argv[i+1],"pause")){
                cout << "Adaptive routing based on pause state " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pause;
            }
            else if (!strcmp(argv[i+1],"queue")){
                cout << "Adaptive routing based on queue size " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_queuesize;
            }
            else if (!strcmp(argv[i+1],"bandwidth")){
                cout << "Adaptive routing based on bandwidth utilization " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_bandwidth;
            }
            else if (!strcmp(argv[i+1],"pqb")){
                cout << "Adaptive routing based on pause, queuesize and bandwidth utilization " << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pqb;
            }
            else if (!strcmp(argv[i+1],"pq")){
                cout << "Adaptive routing based on pause, queuesize" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pq;
            }
            else if (!strcmp(argv[i+1],"pb")){
                cout << "Adaptive routing based on pause, bandwidth utilization" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_pb;
            }
            else if (!strcmp(argv[i+1],"qb")){
                cout << "Adaptive routing based on queuesize, bandwidth utilization" << endl;
                FatTreeSwitch::fn = &FatTreeSwitch::compare_qb; 
            }
            else {
                cout << "Unknown AR method expecting one of pause, queue, bandwidth, pqb, pq, pb, qb" << endl;
                exit(1);
            }
            i++;
        } else if (!strcmp(argv[i],"-path_selection_granularity")){
            path_selection_granularity = atoi(argv[i+1]);
            cout << "Path selection granularity set to " << path_selection_granularity << " bytes" << endl;
            if (path_selection_granularity == 0) {
                cout << "  (0 = per-packet path selection)" << endl;
            }
            i++;
        } else if (!strcmp(argv[i], "-reps_buffer_size")) {
            int n = atoi(argv[i+1]);
            if (n < 1) {
                cout << "Invalid -reps_buffer_size " << n << ", expected >= 1" << endl;
                exit_error(argv[0]);
            }
            reps_buffer_size = static_cast<uint16_t>(n);
            cout << "REPS circular buffer size set to " << reps_buffer_size << endl;
            i++;
        } else if (!strcmp(argv[i],"-intra_granularity_behavior")){
            if (!strcmp(argv[i+1], "keep")) {
                intra_granularity_behavior = SingMultipath::KEEP_PATH;
                cout << "Intra-granularity behavior: KEEP_PATH (use same path within granularity)" << endl;
            } else if (!strcmp(argv[i+1], "random")) {
                intra_granularity_behavior = SingMultipath::RANDOM_PATH;
                cout << "Intra-granularity behavior: RANDOM_PATH (random path for each packet)" << endl;
            } else {
                cout << "Unknown intra-granularity behavior: " << argv[i+1] << ", expecting 'keep' or 'random'" << endl;
                exit_error(argv[0]);
            }
            i++;
        } else if (!strcmp(argv[i],"-ecn_penalty")){
            ecn_penalty = atoi(argv[i+1]);
            cout << "ECN penalty for bitmap load balancing set to " << (uint32_t)ecn_penalty << endl;
            i++;
        } else if (!strcmp(argv[i],"-strat")){
            if (!strcmp(argv[i+1], "ecmp_host")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
            } else if (!strcmp(argv[i+1], "rr_ecmp")) {
                //this is the host route strategy;
                route_strategy = ECMP_FIB_ECN;
                qt = COMPOSITE_ECN_LB;
                //this is the switch route strategy. 
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR_ECMP);
            } else if (!strcmp(argv[i+1], "ecmp_host_ecn")) {
                route_strategy = ECMP_FIB_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                qt = COMPOSITE_ECN_LB;
            } else if (!strcmp(argv[i+1], "reactive_ecn")) {
                // Jitu's suggestion for something really simple
                // One path at a time, but switch whenever we get a trim or ecn
                //this is the host route strategy;
                route_strategy = REACTIVE_ECN;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
                qt = COMPOSITE_ECN_LB;
            } else if (!strcmp(argv[i+1], "ecmp_ar")) {
                route_strategy = ECMP_FIB;
                path_entropy_size = 1;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ADAPTIVE_ROUTING);
            } else if (!strcmp(argv[i+1], "ecmp_host_ar")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP_ADAPTIVE);
                //the stuff below obsolete
                //FatTreeSwitch::set_ar_fraction(atoi(argv[i+2]));
                //cout << "AR fraction: " << atoi(argv[i+2]) << endl;
                //i++;
            } else if (!strcmp(argv[i+1], "ecmp_rr")) {
                // switch round robin
                route_strategy = ECMP_FIB;
                path_entropy_size = 1;
                FatTreeSwitch::set_strategy(FatTreeSwitch::RR);
            }
            i++;
        } else if (!strcmp(argv[i],"-pathwise_subflows")) {
            int n = atoi(argv[i+1]);
            if (n < 1) n = 1;
            SingSrc::_num_pathwise_subflows = n;
            cout << "Pathwise CC LB subflows: " << n << endl;
            i++;
        } else {
            cout << "Unknown parameter " << argv[i] << endl;
            exit_error(argv[0]);
        }
        i++;
    }

    if (end_time > 0 && logtime >= timeFromUs((uint32_t)end_time)){
        cout << "Logtime set to endtime" << endl;
        logtime = timeFromUs((uint32_t)end_time) - 1;
    }

    assert(trimsize >= 64 && trimsize <= (uint32_t)packet_size);

    cout << "Packet size (MTU) is " << packet_size << endl;

    srand(seed);
    srandom(seed);
    cout << "Parsed args\n";
    Packet::set_packet_size(packet_size);
    CircularBufferREPS<uint16_t>::setBufferSize(reps_buffer_size);
    cout << "Using REPS circular buffer size: " << CircularBufferREPS<uint16_t>::repsBufferSize << endl;


    SingSrc::_mtu = Packet::data_packet_size();
    SingSrc::_mss = SingSrc::_mtu - SingSrc::_hdr_size;
    if (max_unack_window_pkts > 0) {
        mem_b max_unack_window_bytes = static_cast<mem_b>(max_unack_window_pkts) * SingSrc::_mtu;
        SingSrc::setDefaultMaxUnackWindow(max_unack_window_bytes);
        cout << "Setting max unacked window to " << max_unack_window_pkts
             << " packets (" << max_unack_window_bytes << " bytes)" << endl;
    } else {
        SingSrc::setDefaultMaxUnackWindow(0);
    }

    if (!pfc_resume_offset_set) {
        pfc_resume_offset = 3ULL * SingSrc::_mtu;
        cout << "PFC resume offset defaulted to 3*MTU = "
             << pfc_resume_offset << " bytes" << endl;
    }

    bool use_reps_ackagg = (load_balancing_algo == REPS_ACKAGG);
    bool use_bitmap_ackagg = (load_balancing_algo == BITMAP_ACKAGG);
    int effective_pathwise_subflows = SingSrc::_num_pathwise_subflows > 0 ? SingSrc::_num_pathwise_subflows : 1;
    if (use_reps_ackagg || use_bitmap_ackagg) {
        if (effective_pathwise_subflows != 1) {
            cout << "ERROR: "
                 << (use_reps_ackagg ? "reps_ackagg" : "bitmap_ackagg")
                 << " only supports Native LB (pathwise_subflows must be 1)." << endl;
            exit(1);
        }
        if (path_selection_granularity != SingSrc::_mtu) {
            cout << "ERROR: "
                 << (use_reps_ackagg ? "reps_ackagg" : "bitmap_ackagg")
                 << " requires -path_selection_granularity to equal MTU ("
                 << SingSrc::_mtu << " bytes)." << endl;
            exit(1);
        }
    }

    if (route_strategy==NOT_SET){
        route_strategy = ECMP_FIB;
        FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
    }

    /*
    SingSink::_oversubscribed_congestion_control = oversubscribed_congestion_control;
    */

    FatTreeSwitch::_ar_sticky = ar_sticky;
    FatTreeSwitch::_sticky_delta = timeFromUs(ar_sticky_delta);
    FatTreeSwitch::_ecn_threshold_fraction = ecn_thresh;
    FatTreeSwitch::_disable_trim = disable_trim;
    FatTreeSwitch::_trim_size = trimsize;

    eventlist.setEndtime(timeFromUs((uint32_t)end_time));

    switch (route_strategy) {
    case ECMP_FIB_ECN:
    case REACTIVE_ECN:
        if (qt != COMPOSITE_ECN_LB) {
            fprintf(stderr, "Route Strategy is ECMP ECN.  Must use an ECN queue\n");
            exit(1);
        }
        assert(ecn_thresh > 0 && ecn_thresh < 1);
        // no break, fall through
    case ECMP_FIB:
        if (path_entropy_size > 10000) {
            fprintf(stderr, "Route Strategy is ECMP.  Must specify path count using -paths\n");
            exit(1);
        }
        break;
    case NOT_SET:
        fprintf(stderr, "Route Strategy not set.  Use the -strat param.  \nValid values are perm, rand, rg and single\n");
        exit(1);
    default:
        break;
    }

    // prepare the loggers

    cout << "Logging to " << filename.str() << endl;
    //Logfile 
    Logfile logfile(filename.str(), eventlist);

    cout << "Linkspeed set to " << linkspeed/1000000000 << "Gbps" << endl;
    logfile.setStartTime(timeFromSec(0));

    vector<unique_ptr<SingNIC>> nics;

    SingSinkLoggerSampling* sink_logger = NULL;
    if (log_sink) {
        sink_logger = new SingSinkLoggerSampling(logtime, eventlist);
        logfile.addLogger(*sink_logger);
    }
    NicLoggerSampling* nic_logger = NULL;
    if (log_nic) {
        nic_logger = new NicLoggerSampling(logtime, eventlist);
        logfile.addLogger(*nic_logger);
    }
    TrafficLoggerSimple* traffic_logger = NULL;
    if (log_traffic) {
        traffic_logger = new TrafficLoggerSimple();
        logfile.addLogger(*traffic_logger);
    }
    FlowEventLoggerSimple* event_logger = NULL;
    if (log_flow_events) {
        event_logger = new FlowEventLoggerSimple();
        logfile.addLogger(*event_logger);
    }

    //SingSrc::setMinRTO(50000); //increase RTO to avoid spurious retransmits
    SingSrc* uec_src;
    SingSink* uec_snk;

    //Route* routeout, *routein;

    QueueLoggerFactory *qlf = 0;
    if (log_tor_downqueue || log_tor_upqueue) {
        qlf = new QueueLoggerFactory(&logfile, QueueLoggerFactory::LOGGER_SAMPLING, eventlist);
        qlf->set_sample_period(logtime);
    } else if (log_queue_usage) {
        qlf = new QueueLoggerFactory(&logfile, QueueLoggerFactory::LOGGER_EMPTY, eventlist);
        qlf->set_sample_period(logtime);
    }

    auto conns = std::make_unique<ConnectionMatrix>(no_of_nodes);

    if (tm_file){
        cout << "Loading connection matrix from  " << tm_file << endl;

        if (!conns->load(tm_file)){
            cout << "Failed to load connection matrix " << tm_file << endl;
            exit(-1);
        }
    }
    else {
        cout << "Loading connection matrix from  standard input" << endl;        
        conns->load(cin);
    }

    if (conns->N != no_of_nodes && no_of_nodes != 0){
        cout << "Connection matrix number of nodes is " << conns->N << " while I am using " << no_of_nodes << endl;
        exit(-1);
    }

    no_of_nodes = conns->N;

    if (!param_queuesize_set) {
        cout << "Automatic queue sizing enabled ";        
        if (queue_size_bdp_factor==0) {
            if (disable_trim) {
                queue_size_bdp_factor = DEFAULT_NONTRIMMING_QUEUESIZE_FACTOR;
                cout << "non-trimming";
            } else {
                queue_size_bdp_factor = DEFAULT_TRIMMING_QUEUESIZE_FACTOR;
                cout << "trimming";
            }
        }
        cout << " queue-size-to-bdp-factor is " << queue_size_bdp_factor << "xBDP"
             << endl;
    }

    unique_ptr<FatTreeTopologyCfg> topo_cfg;
    if (topo_file) {
        topo_cfg = FatTreeTopologyCfg::load(topo_file, memFromPkt(queuesize_pkt), qt, snd_type);

        if (topo_cfg->no_of_nodes() != no_of_nodes) {
            cerr << "Mismatch between connection matrix (" << no_of_nodes << " nodes) and topology ("
                    << topo_cfg->no_of_nodes() << " nodes)" << endl;
            exit(1);
        }
    } else {
        topo_cfg = make_unique<FatTreeTopologyCfg>(tiers, no_of_nodes, linkspeed, memFromPkt(queuesize_pkt),
                                                   hop_latency, switch_latency, 
                                                   qt, snd_type);
    }

    if (switch_model == "sing") {
        topo_cfg->_switch_model = FatTreeTopologyCfg::SING_SWITCH;
        topo_cfg->_sing_switch_buffer = switch_buffer_mb * 1024ULL * 1024;
        cout << "Using SingSwitch with " << switch_buffer_mb << " MB buffer" << endl;
    }
    SingSwitch::setPfcStatsLogging(log_pfc_stats);

    topo_cfg->_pfc_mask = pfc_mask;
    topo_cfg->_pfc_alpha = pfc_alpha;
    topo_cfg->_pfc_resume_offset = pfc_resume_offset;
    topo_cfg->_pfc_xoff = pfc_xoff;
    topo_cfg->_pfc_xon = pfc_xon;
    topo_cfg->_pfc_headroom = pfc_headroom;

    if (pfc_mask != 0) {
        cout << "PFC enabled: mask=0x" << hex << (int)pfc_mask << dec
             << " alpha=" << pfc_alpha
             << " resume_offset=" << pfc_resume_offset
             << " xoff=" << pfc_xoff << " xon=" << pfc_xon
             << " headroom=" << pfc_headroom << endl;
    }

    simtime_picosec network_max_unloaded_rtt;
    if (use_user_rtt) {
        network_max_unloaded_rtt = timeFromUs(user_specified_rtt_us);
        cout << "Using user-specified RTT: " << timeAsUs(network_max_unloaded_rtt) << " us" << endl;
    } else {
        network_max_unloaded_rtt = calculate_rtt(topo_cfg.get(), linkspeed);
        cout << "Calculated RTT: " << timeAsUs(network_max_unloaded_rtt) << " us" << endl;
    }

    mem_b queuesize = 0;
    uint32_t bdp_pkt;
    if (!param_queuesize_set) {
        if (use_user_rtt) {
            bdp_pkt = calculate_bdp_pkt_with_rtt(network_max_unloaded_rtt, linkspeed);
            cout << "BDP calculated with user RTT: " << bdp_pkt << " packets" << endl;
        } else {
            bdp_pkt = calculate_bdp_pkt(topo_cfg.get(), linkspeed);
            cout << "BDP calculated with computed RTT: " << bdp_pkt << " packets" << endl;
        }
        mem_b queuesize_pkt = bdp_pkt * queue_size_bdp_factor;
        queuesize = memFromPkt(queuesize_pkt);
    } else {
        queuesize = memFromPkt(queuesize_pkt);
    }
    topo_cfg->set_queue_sizes(queuesize);

    if (topo_num_failed > 0) {
        topo_cfg->set_failed_links(topo_num_failed);
        topo_cfg->set_failed_link_ratio(failed_link_ratio);
        cout << "Setting " << topo_num_failed << " failed links with " << (failed_link_ratio * 100) << "% speed ratio" << endl;
    }

    //2 priority queues; 3 hops for incast
    SingSrc::_min_rto = timeFromUs(15 + queuesize * 6.0 * 8 * 1000000 / linkspeed);
    if (rto_min_us > 0) {
        SingSrc::_min_rto = timeFromUs(rto_min_us);
    }
    cout << "Setting min RTO to " << timeAsUs(SingSrc::_min_rto) << endl;

    if (ecn){
        if (!param_ecn_set) {
            ecn_low = memFromPkt(ceil(bdp_pkt * 0.2));
            ecn_high = memFromPkt(ceil(bdp_pkt * 0.8));
        } else {
            ecn_low = ecn_low;
            ecn_high = ecn_high;
        }
        cout << "Setting ECN to parameters low " << ecn_low << " high " << ecn_high <<  " enable on tor downlink " << true << endl;
        topo_cfg->set_ecn_parameters(true, true, ecn_low, ecn_high);
        assert(ecn_low <= ecn_high);
        assert(ecn_high <= queuesize);
    }

    cout << *topo_cfg << endl;

    vector<unique_ptr<FatTreeTopology>> topo;
    topo.resize(planes);
    for (uint32_t p = 0; p < planes; p++) {
        topo[p] = make_unique<FatTreeTopology>(topo_cfg.get(), qlf, &eventlist, nullptr, &logfile, interest_file);

        if (log_switches) {
            topo[p]->add_switch_loggers(logfile, logtime);
        }
    }
    cout << "network_max_unloaded_rtt " << timeAsUs(network_max_unloaded_rtt) << endl;
    
    //handle link failures specified in the connection matrix.
    for (size_t c = 0; c < conns->failures.size(); c++){
        failure* crt = conns->failures.at(c);

        cout << "Adding link failure switch type" << crt->switch_type << " Switch ID " << crt->switch_id << " link ID "  << crt->link_id << endl;
        // xxx we only support failures in plane 0 for now.
        topo[0]->add_failed_link(crt->switch_type,crt->switch_id,crt->link_id);
    }

    // Initialize congestion control algorithms
    // SingSrc::parameterScaleToTargetQ();
    bool trimming_enabled = !disable_trim;
    mem_b network_bdp_bytes = static_cast<mem_b>(timeAsSec(network_max_unloaded_rtt) * (linkspeed / 8));
    SingSrc::initCcGlobalDefaults(network_max_unloaded_rtt, network_bdp_bytes, linkspeed, trimming_enabled);
    if (cc_profile_file != NULL) {
        cc_profile_store = std::make_shared<CcProfileStore>();
        std::string error;
        if (!cc_profile_store->loadFromJsonFile(cc_profile_file, &error)) {
            cout << "Failed to load cc_profile: " << error << endl;
            exit(1);
        }
    }

    if (path_selection_granularity == 0) {
        cout << "Path selection: per-packet (default)" << endl;
    } else {
        cout << "Path selection granularity: " << path_selection_granularity << " bytes" << endl;
    }

    for (size_t ix = 0; ix < no_of_nodes; ix++){
        auto &nic = nics.emplace_back(make_unique<SingNIC>(ix, eventlist,
                                                          linkspeed, ports));
        if (log_nic) {
            nic_logger->monitorNic(nic.get());
        }
    }

    // used just to print out stats data at the end
    list <const Route*> routes;

    vector<connection*>* all_conns = conns->getAllConnections();
    vector <SingSrc*> uec_srcs;

    map<flowid_t, pair<SingSrc*, SingSink*>> flowmap;
    map<flowid_t, SingPdcSes*> flow_pdc_map;
    if(planes != 1){
        cout << "We are taking the plane 0 to calculate the network rtt; If all the planes have the same tiers, you can remove this check." << endl;
        assert(false);
    }

    mem_b cwnd_b = cwnd*Packet::data_packet_size();
    for (size_t c = 0; c < all_conns->size(); c++){
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;
        const bool flow_reuse_hit =
            conn_reuse && crt->flowid && flowmap.find(crt->flowid) != flowmap.end();

        if (!conn_reuse and crt->msgid.has_value()) {
            cout << "msg keyword can only be used when conn_reuse is enabled.\n";
            abort();
        }
        if (flow_reuse_hit && hasFlowCcOverride(*crt)) {
            failFlowCcConfig(*crt, "cc override is only allowed on the first row of a reused flow");
        }

        assert(planes > 0);
        simtime_picosec transmission_delay = (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed) * topo_cfg->get_diameter() * 1000) 
                                             + (SingBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed) * topo_cfg->get_diameter() * 1000);
        simtime_picosec base_rtt_bw_two_points = 2*topo_cfg->get_two_point_diameter_latency(src, dest) + transmission_delay;

        //cout << "Connection " << crt->src << "->" <<crt->dst << " starting at " << crt->start << " size " << crt->size << endl;

        if (!conn_reuse 
            || (crt->flowid and flowmap.find(crt->flowid) == flowmap.end())) {
            unique_ptr<SingMultipath> mp = nullptr;
            if (load_balancing_algo == BITMAP || load_balancing_algo == BITMAP_ACKAGG){
                mp = make_unique<SingMpBitmap>(path_entropy_size, SingSrc::_debug, ecn_penalty);
            } else if (load_balancing_algo == REPS || load_balancing_algo == REPS_ACKAGG){
                mp = make_unique<SingMpReps>(path_entropy_size, SingSrc::_debug, !disable_trim);
            } else if (load_balancing_algo == REPS_LEGACY){
                mp = make_unique<SingMpRepsLegacy>(path_entropy_size, SingSrc::_debug);
            }else if (load_balancing_algo == OBLIVIOUS){
                mp = make_unique<SingMpOblivious>(path_entropy_size, SingSrc::_debug);
            } else if (load_balancing_algo == MIXED){
                mp = make_unique<SingMpMixed>(path_entropy_size, SingSrc::_debug, ecn_penalty);
            } else {
                cout << "ERROR: Failed to set multipath algorithm, abort." << endl;
                abort();
            }
            mp->setPathSelectionPolicy(path_selection_granularity, intra_granularity_behavior);

            uec_src = new SingSrc(traffic_logger, eventlist, move(mp), *nics.at(src), ports);

            if (crt->flowid) {
                uec_src->setFlowId(crt->flowid);
                assert(flowmap.find(crt->flowid) == flowmap.end()); // don't have dups
            }

            if (conn_reuse) {
                stringstream uec_src_dbg_tag;
                uec_src_dbg_tag << "flow_id " << uec_src->flowId();
                SingPdcSes* pdc = new SingPdcSes(uec_src, EventList::getTheEventList(), SingSrc::_mss, SingSrc::_hdr_size, uec_src_dbg_tag.str());
                uec_src->makeReusable(pdc);
                flow_pdc_map[uec_src->flowId()] = pdc;
            }

            uec_snk = new SingSink(NULL, *nics.at(dest), ports);
            SingSrc::AckPathFeedbackMode ack_feedback_mode = SingSrc::AckPathFeedbackMode::LEGACY;
            SingSink::AckFeedbackMode sink_ack_feedback_mode = SingSink::AckFeedbackMode::LEGACY;
            if (use_reps_ackagg) {
                ack_feedback_mode = SingSrc::AckPathFeedbackMode::REPS_ACKAGG;
                sink_ack_feedback_mode = SingSink::AckFeedbackMode::REPS_ACKAGG;
            } else if (use_bitmap_ackagg) {
                ack_feedback_mode = SingSrc::AckPathFeedbackMode::BITMAP_ACKAGG;
                sink_ack_feedback_mode = SingSink::AckFeedbackMode::BITMAP_ACKAGG;
            }
            uec_src->setAckPathFeedbackMode(ack_feedback_mode);
            uec_snk->setAckFeedbackMode(sink_ack_feedback_mode);

            flowmap[uec_src->flowId()] = { uec_src, uec_snk };

            if (crt->flowid) {
                uec_snk->setFlowId(crt->flowid);
            }

            simtime_picosec flow_rtt = enable_accurate_base_rtt ? base_rtt_bw_two_points : network_max_unloaded_rtt;
            mem_b flow_bdp_bytes = static_cast<mem_b>(timeAsSec(flow_rtt) * (nics.at(src)->linkspeed() / 8));
            FlowBasicParams flow_basic_params = uec_src->buildFlowBasicParams(flow_rtt, flow_bdp_bytes);
            flow_basic_params.init_cwnd = (cwnd_b == 0)
                ? static_cast<mem_b>(1.5 * flow_bdp_bytes)
                : cwnd_b;
            FlowCcOverrideInput flow_cc_input{crt->cc_algo, crt->cc_profile};
            FlowCcSelectionSpec flow_cc_selection;
            std::string flow_cc_error;
            if (!CcProfileFlowSelectionResolver::resolve(
                    flow_cc_input,
                    SingSrc::_sender_cc_algo,
                    cc_profile_store.get(),
                    &flow_cc_selection,
                    &flow_cc_error)) {
                failFlowCcConfig(*crt, flow_cc_error);
            }
            uec_src->setFlowCcSelection(flow_cc_selection.algo, flow_cc_selection.profile);
            uec_src->initCcForFlow(flow_basic_params);
            if (SingSrc::_num_pathwise_subflows > 1) {
                uec_src->createPathwiseSubflows(SingSrc::_num_pathwise_subflows, flow_basic_params);
            }
            uec_srcs.push_back(uec_src);
            uec_src->setDst(dest);

            if (log_flow_events) {
                uec_src->logFlowEvents(*event_logger);
            }
            

            uec_src->setName("Uec_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*uec_src);
            uec_snk->setSrc(src);

            ((DataReceiver*)uec_snk)->setName("Uec_sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*(DataReceiver*)uec_snk);

            // Set flow type if specified
            if (!crt->type.empty()) {
                uec_src->setFlowType(crt->type);
            }

            if (!conn_reuse) {
                if (crt->size>0){
                    uec_src->setFlowsize(crt->size);
                    // Calculate and set ideal completion time
                    simtime_picosec ideal_time = calculate_ideal_time(topo_cfg.get(), src, dest, crt->size, linkspeed);
                    uec_src->setIdealTime(ideal_time);
                    // Calculate and set one-way ideal time for receiver perspective
                    simtime_picosec one_way_ideal_time = calculate_one_way_ideal_time(topo_cfg.get(), src, dest, crt->size, linkspeed);
                    uec_src->setOneWayIdealTime(one_way_ideal_time);
                }

                if (crt->trigger) {
                    Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
                    trig->add_target(*uec_src);
                }

                if (crt->send_done_trigger) {
                    Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
                    uec_src->setEndTrigger(*trig);
                }

                if (crt->recv_done_trigger) {
                    Trigger* trig = conns->getTrigger(crt->recv_done_trigger, eventlist);
                    uec_snk->setEndTrigger(*trig);
                }
            } else {
                assert(crt->size > 0);

                optional<simtime_picosec> start_ts = {};
                if (crt->start != TRIGGER_START) {
                    start_ts.emplace(timeFromUs((uint32_t)crt->start));
                } 

                SingPdcSes* pdc = flow_pdc_map.find(crt->flowid)->second;
                SingMsg* msg = pdc->enque(crt->size, start_ts, true);

                if (crt->trigger) {
                    Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
                    trig->add_target(*msg);
                }

                if (crt->send_done_trigger) {
                    Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
                    msg->setTrigger(SingMsg::MsgStatus::SentLast, trig);
                }

                if (crt->recv_done_trigger) {
                    Trigger* trig = conns->getTrigger(crt->recv_done_trigger, eventlist);
                    uec_snk->setEndTrigger(*trig);
                    msg->setTrigger(SingMsg::MsgStatus::RecvdLast, trig);
                }
            }

            //uec_snk->set_priority(crt->priority);
                            
            for (uint32_t p = 0; p < planes; p++) {
                switch (route_strategy) {
                case ECMP_FIB:
                case ECMP_FIB_ECN:
                case REACTIVE_ECN:
                    {
                        Route* srctotor = new Route();
                        srctotor->push_back(topo[p]->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
                        srctotor->push_back(topo[p]->pipes_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
                        srctotor->push_back(topo[p]->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]->getRemoteEndpoint());

                        Route* dsttotor = new Route();
                        dsttotor->push_back(topo[p]->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
                        dsttotor->push_back(topo[p]->pipes_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
                        dsttotor->push_back(topo[p]->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]->getRemoteEndpoint());

                        uec_src->connectPort(p, *srctotor, *dsttotor, *uec_snk, crt->start);
                        //uec_src->setPaths(path_entropy_size);
                        //uec_snk->setPaths(path_entropy_size);

                        //register src and snk to receive packets from their respective TORs. 
                        assert(topo[p]->switches_lp[topo_cfg->HOST_POD_SWITCH(src)]);
                        assert(topo[p]->switches_lp[topo_cfg->HOST_POD_SWITCH(src)]);
                        topo[p]->switches_lp[topo_cfg->HOST_POD_SWITCH(src)]->addHostPort(src,uec_snk->flowId(),uec_src->getPort(p));
                        topo[p]->switches_lp[topo_cfg->HOST_POD_SWITCH(dest)]->addHostPort(dest,uec_src->flowId(),uec_snk->getPort(p));
                        break;
                    }
                default:
                    abort();
                }
            }

            if (log_sink) {
                sink_logger->monitorSink(uec_snk);
            }
        } else {
            // Use existing connection for this message
            assert(crt->msgid.has_value());

            SingPdcSes* pdc = flow_pdc_map.find(crt->flowid)->second;
            uec_src = nullptr;
            uec_snk = nullptr;

            optional<simtime_picosec> start_ts = {};
            if (crt->start != TRIGGER_START) {
                start_ts.emplace(timeFromUs((uint32_t)crt->start));
            } 

            SingMsg* msg = pdc->enque(crt->size, start_ts, true);

            if (crt->trigger) {
                Trigger* trig = conns->getTrigger(crt->trigger, eventlist);
                trig->add_target(*msg);
            }

            if (crt->send_done_trigger) {
                Trigger* trig = conns->getTrigger(crt->send_done_trigger, eventlist);
                msg->setTrigger(SingMsg::MsgStatus::SentLast, trig);
            }

            if (crt->recv_done_trigger) {
                Trigger* trig = conns->getTrigger(crt->recv_done_trigger, eventlist);
                msg->setTrigger(SingMsg::MsgStatus::RecvdLast, trig);
            }
        }
    }

    Logged::dump_idmap();
    // Record the setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# hostnicrate = " + ntoa(linkspeed/1000000) + " Mbps");
    //logfile.write("# corelinkrate = " + ntoa(HOST_NIC*CORE_TO_HOST) + " pkt/sec");
    //logfile.write("# buffer = " + ntoa((double) (queues_na_ni[0][1]->_maxsize) / ((double) pktsize)) + " pkt");
    
    // GO!
    cout << "Starting simulation" << endl;
    while (eventlist.doNextEvent()) {
    }

    cout << "Done" << endl;
    int new_pkts = 0, rtx_pkts = 0, bounce_pkts = 0, ack_pkts = 0, nack_pkts = 0, sleek_pkts = 0;
    for (size_t ix = 0; ix < uec_srcs.size(); ix++) {
        const struct SingSrc::Stats& s = uec_srcs[ix]->stats();
        new_pkts += s.new_pkts_sent;
        rtx_pkts += s.rtx_pkts_sent;
        bounce_pkts += s.bounces_received;
        ack_pkts += s.acks_received;
        nack_pkts += s.nacks_received;
        sleek_pkts += s._sleek_counter;
    }
    cout << "New: " << new_pkts << " Rtx: " << rtx_pkts << " Bounced: " << bounce_pkts << " ACKs: " << ack_pkts << " NACKs: " << nack_pkts << " sleek_pkts: " << sleek_pkts << endl;
    
    // Print maximum concurrent flows per node
    cout << "\n=== Concurrent Flows Statistics Per Node ===" << endl;
    simtime_picosec simulation_end_time = eventlist.now();
    
    for (size_t node_id = 0; node_id < nics.size(); node_id++) {
        uint32_t max_flows = nics[node_id]->getMaxConcurrentFlows();
        simtime_picosec max_time = nics[node_id]->getMaxConcurrentFlowsTime();
        uint32_t current_flows = nics[node_id]->getActiveFlowCount();
        double avg_flows = nics[node_id]->getAverageConcurrentFlows(simulation_end_time);
        
        if (max_flows > 0) {
            cout << "Node " << node_id 
                 << ": max_concurrent_flows=" << max_flows 
                 << " at " << timeAsUs(max_time) << " us"
                 << ", avg_concurrent_flows=" << fixed << setprecision(2) << avg_flows
                 << " (current: " << current_flows << ")"
                 << endl;
        }
    }
    cout << "=============================================" << endl;
    
    /*
    list <const Route*>::iterator rt_i;
    int counts[10]; int hop;
    for (int i = 0; i < 10; i++)
        counts[i] = 0;
    cout << "route count: " << routes.size() << endl;
    for (rt_i = routes.begin(); rt_i != routes.end(); rt_i++) {
        const Route* r = (*rt_i);
        //print_route(*r);
#ifdef PRINTPATHS
        cout << "Path:" << endl;
#endif
        hop = 0;
        for (int i = 0; i < r->size(); i++) {
            PacketSink *ps = r->at(i); 
            CompositeQueue *q = dynamic_cast<CompositeQueue*>(ps);
            if (q == 0) {
#ifdef PRINTPATHS
                cout << ps->nodename() << endl;
#endif
            } else {
#ifdef PRINTPATHS
                cout << q->nodename() << " " << q->num_packets() << "pkts " 
                     << q->num_headers() << "hdrs " << q->num_acks() << "acks " << q->num_nacks() << "nacks " << q->num_stripped() << "stripped"
                     << endl;
#endif
                counts[hop] += q->num_stripped();
                hop++;
            }
        } 
#ifdef PRINTPATHS
        cout << endl;
#endif
    }
    for (int i = 0; i < 10; i++)
        cout << "Hop " << i << " Count " << counts[i] << endl;
    */  

    return EXIT_SUCCESS;
}
