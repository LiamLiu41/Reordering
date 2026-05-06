
#include "sing_mp.h"

#include <iostream>

using namespace std;

void SingMultipath::setPathSelectionPolicy(mem_b granularity, IntraGranularityBehavior behavior) {
    _path_selection_granularity = granularity;
    _intra_granularity_behavior = behavior;
    _bytes_sent_since_last_path_update = 0;
    _current_pathid = 0;
    _path_initialized = false;
}

SingMultipath::PathSelectionResult SingMultipath::selectPathForPacket(
    mem_b pkt_size, uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    if (_path_selection_granularity == 0) {
        uint16_t path_id = nextEntropy(seq_sent, cur_cwnd_in_pkts);
        return {path_id, false};
    }

    if (!_path_initialized) {
        _current_pathid = nextEntropy(seq_sent, cur_cwnd_in_pkts);
        _path_initialized = true;
        _bytes_sent_since_last_path_update = 0;
    }

    uint16_t path_id = 0;
    if (_intra_granularity_behavior == RANDOM_PATH) {
        path_id = randomEntropy();
    } else {
        path_id = _current_pathid;
    }

    bool path_sel_end = (_bytes_sent_since_last_path_update + pkt_size >=
                         _path_selection_granularity);
    _bytes_sent_since_last_path_update += pkt_size;

    if (path_sel_end) {
        _bytes_sent_since_last_path_update = 0;
        _current_pathid = nextEntropy(seq_sent, cur_cwnd_in_pkts);
    }

    return {path_id, path_sel_end};
}


SingMpOblivious::SingMpOblivious(uint16_t no_of_paths,
                               bool debug)
    : SingMultipath(debug),
      _no_of_paths(no_of_paths),
      _current_ev_index(0)
      {

    _path_random = rand() % UINT16_MAX;  // random upper bits of EV
    _path_xor = rand() % _no_of_paths;

    if (_debug)
        cout << "Multipath"
             << " Oblivious"
             << " _no_of_paths " << _no_of_paths
             << " _path_random " << _path_random
             << " _path_xor " << _path_xor
             << endl;
}

void SingMpOblivious::processEv(uint16_t path_id, PathFeedback feedback) {
    return;
}

uint16_t SingMpOblivious::nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    // _no_of_paths must be a power of 2
    uint16_t mask = _no_of_paths - 1;
    uint16_t entropy = (_current_ev_index ^ _path_xor) & mask;

    // set things for next time
    _current_ev_index++;
    if (_current_ev_index == _no_of_paths) {
        _current_ev_index = 0;
        _path_xor = rand() & mask;
    }

    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}

uint16_t SingMpOblivious::randomEntropy() {
    // Purely random path selection
    uint16_t mask = _no_of_paths - 1;
    uint16_t entropy = rand() & mask;
    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}


SingMpBitmap::SingMpBitmap(uint16_t no_of_paths, bool debug, uint8_t ecn_penalty)
    : SingMultipath(debug),
      _no_of_paths(no_of_paths),
      _current_ev_index(0),
      _ev_skip_bitmap(),
      _ev_skip_count(0),
      _ecn_penalty(ecn_penalty)
      {

    _max_penalty = 15;

    _path_random = rand() % 0xffff;  // random upper bits of EV
    _path_xor = rand() % _no_of_paths;

    _ev_skip_bitmap.resize(_no_of_paths);
    for (uint32_t i = 0; i < _no_of_paths; i++) {
        _ev_skip_bitmap[i] = 0;
    }

    if (_debug)
        cout << "Multipath"
            << " Bitmap"
            << " _no_of_paths " << _no_of_paths
            << " _path_random " << _path_random
            << " _path_xor " << _path_xor
            << " _max_penalty " << (uint32_t)_max_penalty
            << " _ecn_penalty " << (uint32_t)_ecn_penalty
            << endl;
}

void SingMpBitmap::processEv(uint16_t path_id, PathFeedback feedback) {
    // _no_of_paths must be a power of 2
    uint16_t mask = _no_of_paths - 1;
    path_id &= mask;  // only take the relevant bits for an index

    if (feedback != PathFeedback::PATH_GOOD && !_ev_skip_bitmap[path_id])
        _ev_skip_count++;

    uint8_t penalty = 0;

    if (feedback == PathFeedback::PATH_ECN)
        penalty = _ecn_penalty;
    else if (feedback == PathFeedback::PATH_NACK)
        penalty = 4;
    else if (feedback == PathFeedback::PATH_TIMEOUT)
        penalty = _max_penalty;

    _ev_skip_bitmap[path_id] += penalty;
    if (_ev_skip_bitmap[path_id] > _max_penalty) {
        _ev_skip_bitmap[path_id] = _max_penalty;
    }
}

uint16_t SingMpBitmap::nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    // _no_of_paths must be a power of 2
    uint16_t mask = _no_of_paths - 1;
    uint16_t entropy = (_current_ev_index ^ _path_xor) & mask;
    bool flag = false;
    int counter = 0;
    while (_ev_skip_bitmap[entropy] > 0) {
        if (flag == false){
            _ev_skip_bitmap[entropy]--;
            if (!_ev_skip_bitmap[entropy]){
                assert(_ev_skip_count>0);
                _ev_skip_count--;
            }
        }

        flag = true;
        counter ++;
        if (counter > _no_of_paths){
            break;
        }
        _current_ev_index++;
        if (_current_ev_index == _no_of_paths) {
            _current_ev_index = 0;
            _path_xor = rand() & mask;
        }
        entropy = (_current_ev_index ^ _path_xor) & mask;
    }

    // set things for next time
    _current_ev_index++;
    if (_current_ev_index == _no_of_paths) {
        _current_ev_index = 0;
        _path_xor = rand() & mask;
    }

    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}

uint16_t SingMpBitmap::randomEntropy() {
    // Purely random path selection
    uint16_t mask = _no_of_paths - 1;
    uint16_t entropy = rand() & mask;
    entropy |= _path_random ^ (_path_random & mask);  // set upper bits
    return entropy;
}

SingMpReps::SingMpReps(uint16_t no_of_paths, bool debug, bool is_trimming_enabled)
    : SingMultipath(debug),
      _no_of_paths(no_of_paths),
      _crt_path(0),
      _is_trimming_enabled(is_trimming_enabled) {

    circular_buffer_reps = new CircularBufferREPS<uint16_t>(CircularBufferREPS<uint16_t>::repsBufferSize);

    if (_debug)
        cout << "Multipath"
            << " REPS"
            << " _no_of_paths " << _no_of_paths
            << endl;
}

void SingMpReps::processEv(uint16_t path_id, PathFeedback feedback) {

    if ((feedback == PATH_TIMEOUT) && !circular_buffer_reps->isFrozenMode() && circular_buffer_reps->explore_counter == 0) {
        if (_is_trimming_enabled) { // If we have trimming enabled
            circular_buffer_reps->setFrozenMode(true);
            circular_buffer_reps->can_exit_frozen_mode = EventList::getTheEventList().now() +  circular_buffer_reps->exit_freeze_after;
        } else {
            cout << timeAsUs(EventList::getTheEventList().now()) << "REPS currently requires trimming in this implementation." << endl;
            exit(EXIT_FAILURE); // If we reach this point, it means we are trying to enter freezing mode without trimming enabled.
        } // In this version of REPS, we do not enter freezing mode without trimming enabled. Check the REPS paper to implement it also without trimming.
    }

    if (circular_buffer_reps->isFrozenMode() && EventList::getTheEventList().now() > circular_buffer_reps->can_exit_frozen_mode) {
        circular_buffer_reps->setFrozenMode(false);
        circular_buffer_reps->resetBuffer();
        circular_buffer_reps->explore_counter = 16;
    }

    if ((feedback == PATH_GOOD) && !circular_buffer_reps->isFrozenMode()) {
        circular_buffer_reps->add(path_id);
    } else if (circular_buffer_reps->isFrozenMode() && (feedback == PATH_GOOD)) {
        circular_buffer_reps->add(path_id);
    }
}

uint16_t SingMpReps::nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    if (circular_buffer_reps->explore_counter > 0) {
        circular_buffer_reps->explore_counter--;
        return rand() % _no_of_paths;
    }

    if (circular_buffer_reps->isFrozenMode()) {
        if (circular_buffer_reps->isEmpty()) {
            return rand() % _no_of_paths;
        } else {
            return circular_buffer_reps->remove_frozen();
        }
    } else {
        if (circular_buffer_reps->isEmpty() || circular_buffer_reps->getNumberFreshEntropies() == 0) {
            return _crt_path = rand() % _no_of_paths;
        } else {
            return circular_buffer_reps->remove_earliest_fresh();
        }
    }
}

uint16_t SingMpReps::randomEntropy() {
    // Purely random path selection
    return rand() % _no_of_paths;
}


SingMpRepsLegacy::SingMpRepsLegacy(uint16_t no_of_paths, bool debug)
    : SingMultipath(debug),
      _no_of_paths(no_of_paths),
      _crt_path(0) {

    if (_debug)
        cout << "Multipath"
            << " REPS"
            << " _no_of_paths " << _no_of_paths
            << endl;
}

void SingMpRepsLegacy::processEv(uint16_t path_id, PathFeedback feedback) {
    if (feedback == PATH_GOOD){
        _next_pathid.push_back(path_id);
        if (_debug){
            cout << timeAsUs(EventList::getTheEventList().now()) << " " << _debug_tag << " REPS Add " << path_id << " " << _next_pathid.size() << endl;
        }
    }
}

uint16_t SingMpRepsLegacy::nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    if (seq_sent < min(cur_cwnd_in_pkts, (uint64_t)_no_of_paths)) {
        _crt_path++;
        if (_crt_path == _no_of_paths) {
            _crt_path = 0;
        }

        if (_debug) 
            cout << timeAsUs(EventList::getTheEventList().now()) << " " << _debug_tag << " REPS FirstWindow " << _crt_path << endl;

    } else {
        if (_next_pathid.empty()) {
            assert(_no_of_paths > 0);
		    _crt_path = random() % _no_of_paths;

            if (_debug) 
                cout << timeAsUs(EventList::getTheEventList().now()) << " " << _debug_tag << " REPS Steady " << _crt_path << endl;

        } else {
            _crt_path = _next_pathid.front();
            _next_pathid.pop_front();

            if (_debug) 
                cout << timeAsUs(EventList::getTheEventList().now()) << " " << _debug_tag << " REPS Recycle " << _crt_path << " " << _next_pathid.size() << endl;

        }
    }
    return _crt_path;
}

optional<uint16_t> SingMpRepsLegacy::nextEntropyRecycle() {
    if (_next_pathid.empty()) {
        return {};
    } else {
        _crt_path = _next_pathid.front();
        _next_pathid.pop_front();

        if (_debug) 
            cout << timeAsUs(EventList::getTheEventList().now()) << " " << _debug_tag << " MIXED Recycle " << _crt_path << " " << _next_pathid.size() << endl;
        return { _crt_path };
    }
}

uint16_t SingMpRepsLegacy::randomEntropy() {
    // Purely random path selection
    return rand() % _no_of_paths;
}


SingMpMixed::SingMpMixed(uint16_t no_of_paths, bool debug, uint8_t ecn_penalty)
    : SingMultipath(debug),
      _bitmap(SingMpBitmap(no_of_paths, debug, ecn_penalty)),
      _reps_legacy(SingMpRepsLegacy(no_of_paths, debug))
      {
}

void SingMpMixed::set_debug_tag(string debug_tag) {
    _bitmap.set_debug_tag(debug_tag);
    _reps_legacy.set_debug_tag(debug_tag);
}

void SingMpMixed::processEv(uint16_t path_id, PathFeedback feedback) {
    _bitmap.processEv(path_id, feedback);
    _reps_legacy.processEv(path_id, feedback);
}

uint16_t SingMpMixed::nextEntropy(uint64_t seq_sent, uint64_t cur_cwnd_in_pkts) {
    auto reps_val = _reps_legacy.nextEntropyRecycle();
    if (reps_val.has_value()) {
        return reps_val.value();
    } else {
        return _bitmap.nextEntropy(seq_sent, cur_cwnd_in_pkts);
    }
}

uint16_t SingMpMixed::randomEntropy() {
    // Purely random path selection (delegate to bitmap)
    return _bitmap.randomEntropy();
}
