/*!
 * @file overlap.cpp
 *
 * @brief Overlap class source file
 */

#include "sequence.hpp"
#include "overlap.hpp"
#include "edlib.h"

namespace racon {

Overlap::Overlap(uint32_t a_id, uint32_t b_id, double, uint32_t,
    uint32_t a_rc, uint32_t a_begin, uint32_t a_end, uint32_t a_length,
    uint32_t b_rc, uint32_t b_begin, uint32_t b_end, uint32_t b_length)
        : q_name_(), q_id_(a_id - 1), q_begin_(a_begin), q_end_(a_end),
        q_length_(a_length), t_name_(), t_id_(b_id - 1), t_begin_(b_begin),
        t_end_(b_end), t_length_(b_length), strand_(a_rc ^ b_rc), length_(),
        error_(), cigar_(), is_valid_(true), is_transmuted_(false),
        breaking_points_() {

    length_ = std::max(q_end_ - q_begin_, t_end_ - t_begin_);
    error_ = 1 - std::min(q_end_ - q_begin_, t_end_ - t_begin_) /
        static_cast<double>(length_);
}

Overlap::Overlap(const char* q_name, uint32_t q_name_length, uint32_t q_length,
    uint32_t q_begin, uint32_t q_end, char orientation, const char* t_name,
    uint32_t t_name_length, uint32_t t_length, uint32_t t_begin,
    uint32_t t_end, uint32_t, uint32_t, uint32_t)
        : q_name_(q_name, q_name_length), q_id_(), q_begin_(q_begin),
        q_end_(q_end), q_length_(q_length), t_name_(t_name, t_name_length),
        t_id_(), t_begin_(t_begin), t_end_(t_end), t_length_(t_length),
        strand_(orientation == '-'), length_(), error_(), cigar_(),
        is_valid_(true), is_transmuted_(false), breaking_points_() {

    length_ = std::max(q_end_ - q_begin_, t_end_ - t_begin_);
    error_ = 1 - std::min(q_end_ - q_begin_, t_end_ - t_begin_) /
        static_cast<double>(length_);
}

Overlap::Overlap(const char* q_name, uint32_t q_name_length, uint32_t flag,
    const char* t_name, uint32_t t_name_length, uint32_t t_begin,
    uint32_t, const char* cigar, uint32_t cigar_length, const char*,
    uint32_t, uint32_t, uint32_t, const char*, uint32_t, const char*,
    uint32_t)
        : q_name_(q_name, q_name_length), q_id_(), q_begin_(0), q_end_(),
        q_length_(), t_name_(t_name, t_name_length), t_id_(), t_begin_(t_begin - 1),
        t_end_(), t_length_(), strand_(flag & 0x10), length_(), error_(),
        cigar_(cigar, cigar_length), is_valid_(!(flag & 0x4)),
        is_transmuted_(false), breaking_points_() {

    if (cigar_.size() > 1) {
        uint32_t q_alignment_length = 0, t_alignment_length = 0;
        for (uint32_t i = 0, j = 0; i < cigar_.size(); ++i) {
            if (cigar_[i] == 'M' || cigar_[i] == '=' || cigar_[i] == 'X') {
                auto num_bases = atoi(&cigar_[j]);
                j = i + 1;
                q_alignment_length += num_bases;
                t_alignment_length += num_bases;
            } else if (cigar_[i] == 'I') {
                auto num_bases = atoi(&cigar_[j]);
                j = i + 1;
                q_alignment_length += num_bases;
            } else if (cigar_[i] == 'D' || cigar_[i] == 'N') {
                auto num_bases = atoi(&cigar_[j]);
                j = i + 1;
                t_alignment_length += num_bases;
            } else if (cigar_[i] == 'S' || cigar_[i] == 'H' || cigar_[i] == 'P') {
                j = i + 1;
            }
        }
        t_end_ = t_begin_ + t_alignment_length;
        length_ = std::max(q_alignment_length, t_alignment_length);
        error_ = 1 - std::min(q_alignment_length, t_alignment_length) /
            static_cast<double>(length_);
    }
}

template<typename T>
bool transmuteId(const std::unordered_map<T, uint32_t>& hash, const T& key,
    uint32_t& id) {

    auto it = hash.find(key);
    if (it == hash.end()) {
        return false;
    }
    id = it->second;
    return true;
}

void Overlap::transmute(const std::unordered_map<std::string, uint32_t>& name_to_id,
    const std::unordered_map<uint32_t, uint32_t>& id_to_id) {

    if (is_transmuted_) {
         return;
    }

    if (!q_name_.empty()) {
        if (!transmuteId(name_to_id, q_name_ + "0", q_id_)) {
            fprintf(stderr, "racon::Overlap::transmute error: "
                "missing sequence with name %s!\n", q_name_.c_str());
            exit(1);
        }
    } else {
        if (!transmuteId(id_to_id, q_id_ << 1 | 0, q_id_)) {
            fprintf(stderr, "racon::Overlap::transmute error: "
                "missing sequence with id %d!\n", q_id_ >> 1);
            exit(1);
        }
    }
    if (!t_name_.empty()) {
        if (!transmuteId(name_to_id, t_name_ + "1", t_id_)) {
            fprintf(stderr, "racon::Overlap::transmute error: "
                "missing target sequence with name %s!\n", t_name_.c_str());
            exit(1);
        }
    } else {
        if (!transmuteId(id_to_id, t_id_ << 1 | 1, t_id_)) {
            fprintf(stderr, "racon::Overlap::transmute error: "
                "missing target sequence with id %d!\n", t_id_ >> 1);
            exit(1);
        }
    }

    is_transmuted_ = true;
}

void Overlap::find_breaking_points(const std::vector<std::unique_ptr<Sequence>>& sequences,
    uint32_t window_length) {

    if (!is_valid_ || !is_transmuted_) {
        fprintf(stderr, "racon::Overlap::find_breaking_points error: "
            "overlap not valid/transmuted!\n");
        exit(1);
    }

    if (!breaking_points_.empty()) {
        return;
    }

    q_length_ = sequences[q_id_]->data().size();
    t_length_ = sequences[t_id_]->data().size();

    int32_t q_ptr = -1, t_ptr = t_begin_ - 1;
    uint32_t clip_size = 0;

    if (cigar_.empty()) {
        // align overlaps with edlib
        const char* q = !strand_ ? &(sequences[q_id_]->data()[q_begin_]) :
            &(sequences[q_id_]->reverse_complement()[q_length_ - q_end_]);
        const char* t = &(sequences[t_id_]->data()[t_begin_]);

        EdlibAlignResult result = edlibAlign(q, q_end_ - q_begin_, t, t_end_ -
            t_begin_, edlibNewAlignConfig(-1, EDLIB_MODE_NW, EDLIB_TASK_PATH,
            nullptr, 0));

        if (result.status == EDLIB_STATUS_OK) {
            char* cigar = edlibAlignmentToCigar(result.alignment,
                result.alignmentLength, EDLIB_CIGAR_STANDARD);
            cigar_ = cigar;
            free(cigar);
        } else {
            fprintf(stderr, "racon::Overlap::find_breaking_points error: "
                "edlib unable to align sequences (%d x %d)!\n", q_id_, t_id_);
            exit(1);
        }

        edlibFreeAlignResult(result);

        q_ptr += strand_ ? q_length_ - q_end_ : q_begin_;
    } else {
        for (uint32_t i = 0, j = 0; i < cigar_.size(); ++i) {
            if (cigar_[i] == 'S' || cigar_[i] == 'H') {
                q_ptr += atoi(&cigar_[j]);
                clip_size = q_ptr + 1;
                break;
            } else if (cigar_[i] == 'M' || cigar_[i] == '=' || cigar_[i] == 'I' ||
                cigar_[i] == 'D' || cigar_[i] == 'N' || cigar_[i] == 'P' ||
                cigar_[i] == 'X') {
                break;
            }
        }
    }

    // find breaking points from cigar
    std::vector<int32_t> window_ends;
    for (uint32_t i = 0; i < t_end_; i += window_length) {
        if (i > t_begin_) {
            window_ends.emplace_back(i - 1);
        }
    }
    window_ends.emplace_back(t_end_ - 1);

    uint32_t w = 0;
    std::pair<uint32_t, uint32_t> first_match = {0, 0}, last_match = {0, 0};
    bool found_first_match = false;

    for (uint32_t i = clip_size, j = i; i < cigar_.size(); ++i) {
        if (cigar_[i] == 'M' || cigar_[i] == '=' || cigar_[i] == 'X') {
            uint32_t k = 0, num_bases = atoi(&cigar_[j]);
            j = i + 1;
            while (k < num_bases) {
                ++q_ptr;
                ++t_ptr;
                if (!found_first_match) {
                    found_first_match = true;
                    first_match.first = t_ptr;
                    first_match.second = q_ptr;
                }
                last_match.first = t_ptr + 1;
                last_match.second = q_ptr + 1;
                if (t_ptr == window_ends[w]) {
                    if (found_first_match) {
                        breaking_points_.emplace_back(first_match);
                        breaking_points_.emplace_back(last_match);
                    }
                    found_first_match = false;
                    ++w;
                }
                ++k;
            }
        } else if (cigar_[i] == 'I') {
            uint32_t num_bases = atoi(&cigar_[j]);
            j = i + 1;
            q_ptr += num_bases;
        } else if (cigar_[i] == 'D' || cigar_[i] == 'N') {
            uint32_t k = 0, num_bases = atoi(&cigar_[j]);
            j = i + 1;
            while (k < num_bases) {
                ++t_ptr;
                if (t_ptr == window_ends[w]) {
                    if (found_first_match) {
                        breaking_points_.emplace_back(first_match);
                        breaking_points_.emplace_back(last_match);
                    }
                    found_first_match = false;
                    ++w;
                }
                ++k;
            }
        } else if (cigar_[i] == 'S' || cigar_[i] == 'H' || cigar_[i] == 'P') {
            j = i + 1;
        }
    }

    std::string().swap(cigar_);
}

}
