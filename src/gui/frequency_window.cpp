#include "skywave_link/frequency_window.hpp"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace skylink {

namespace {

std::string slotLabel(int slot) {
    return (slot >= 0 ? "+" : "") + std::to_string(slot);
}

struct BandRecommendation {
    std::string band;
    float score = -99.0f;
    float avg_snr = -30.0f;
    float link_success = 0.0f;  // 0..1
    float occupancy = 0.0f;     // 0..1
    float propagation_score = 0.0f;
    float path_km = 0.0f;
    float luf_mhz = 0.0f;
    float muf_mhz = 0.0f;
};

std::vector<BandRecommendation> computeBandRecommendations(MessageStore* store,
                                                           FrequencyManager* freq_mgr,
                                                           const AppConfig* config) {
    std::vector<BandRecommendation> ranked;
    if (!store || !store->isOpen() || !freq_mgr || !config) {
        return ranked;
    }

    struct Accum {
        double weighted_snr = 0.0;
        double snr_weight = 0.0;
        double weighted_ping_success = 0.0;
        double ping_weight = 0.0;
        double occupancy_weight = 0.0;
        double propagation_weighted = 0.0;
        double propagation_weight = 0.0;
        double distance_weighted = 0.0;
        double luf_weighted = 0.0;
        double muf_weighted = 0.0;
    };
    std::map<std::string, Accum> by_band;

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::time_t now_t = static_cast<std::time_t>(now);
    const std::tm now_tm = *std::localtime(&now_t);
    const int now_hour = now_tm.tm_hour;

    auto beacons = store->loadRecentBeacons(500);
    for (const auto& b : beacons) {
        if (b.band.empty()) {
            continue;
        }

        const int64_t age_sec = std::max<int64_t>(0, now - b.timestamp);
        const double age_hours = static_cast<double>(age_sec) / 3600.0;
        const std::time_t ts = static_cast<std::time_t>(b.timestamp);
        const std::tm tm = *std::localtime(&ts);
        int hour_diff = std::abs(now_hour - tm.tm_hour);
        hour_diff = std::min(hour_diff, 24 - hour_diff);

        const double time_weight = 1.0 / (1.0 + static_cast<double>(hour_diff));
        const double decay_weight = std::exp(-age_hours / 24.0);
        const double weight = time_weight * decay_weight;

        auto& acc = by_band[b.band];
        acc.weighted_snr += static_cast<double>(b.snr_db) * weight;
        acc.snr_weight += weight;
        acc.occupancy_weight += weight;

        if (!config->station.grid_square.empty() && !b.grid_square.empty()) {
            auto estimate = FrequencyManager::estimatePropagationWindow(
                config->station.grid_square,
                b.grid_square,
                now_hour);
            if (estimate.valid) {
                const double prop_score = freq_mgr->scoreBandForPropagation(b.band, estimate);
                acc.propagation_weighted += prop_score * weight;
                acc.propagation_weight += weight;
                acc.distance_weighted += estimate.distance_km * weight;
                acc.luf_weighted += estimate.luf_mhz * weight;
                acc.muf_weighted += estimate.muf_mhz * weight;
            }
        }
    }

    auto pings = store->loadRecentPings(500);
    for (const auto& p : pings) {
        if (p.band.empty()) {
            continue;
        }

        const int64_t age_sec = std::max<int64_t>(0, now - p.timestamp);
        const double age_hours = static_cast<double>(age_sec) / 3600.0;
        const std::time_t ts = static_cast<std::time_t>(p.timestamp);
        const std::tm tm = *std::localtime(&ts);
        int hour_diff = std::abs(now_hour - tm.tm_hour);
        hour_diff = std::min(hour_diff, 24 - hour_diff);

        const double time_weight = 1.0 / (1.0 + static_cast<double>(hour_diff));
        const double decay_weight = std::exp(-age_hours / 12.0);
        const double weight = 0.8 * time_weight * decay_weight;

        auto& acc = by_band[p.band];
        acc.weighted_ping_success += (p.online ? 1.0 : 0.0) * weight;
        acc.ping_weight += weight;
        acc.occupancy_weight += 0.5 * weight;

        const double ping_snr = p.online ? static_cast<double>(p.snr_db) : -22.0;
        acc.weighted_snr += ping_snr * (0.5 * weight);
        acc.snr_weight += (0.5 * weight);
    }

    double max_occupancy = 0.0;
    for (const auto& entry : by_band) {
        max_occupancy = std::max(max_occupancy, entry.second.occupancy_weight);
    }

    for (const auto& entry : by_band) {
        const auto& band = entry.first;
        const auto& acc = entry.second;

        BandRecommendation rec;
        rec.band = band;
        rec.avg_snr = (acc.snr_weight > 0.0)
            ? static_cast<float>(acc.weighted_snr / acc.snr_weight)
            : -20.0f;
        rec.link_success = (acc.ping_weight > 0.0)
            ? static_cast<float>(acc.weighted_ping_success / acc.ping_weight)
            : 0.5f;
        rec.occupancy = (max_occupancy > 0.0)
            ? static_cast<float>(acc.occupancy_weight / max_occupancy)
            : 0.0f;
        rec.propagation_score = (acc.propagation_weight > 0.0)
            ? static_cast<float>(acc.propagation_weighted / acc.propagation_weight)
            : 0.0f;
        rec.path_km = (acc.propagation_weight > 0.0)
            ? static_cast<float>(acc.distance_weighted / acc.propagation_weight)
            : 0.0f;
        rec.luf_mhz = (acc.propagation_weight > 0.0)
            ? static_cast<float>(acc.luf_weighted / acc.propagation_weight)
            : 0.0f;
        rec.muf_mhz = (acc.propagation_weight > 0.0)
            ? static_cast<float>(acc.muf_weighted / acc.propagation_weight)
            : 0.0f;

        // Composite score favors high-SNR, high success-rate, lower occupancy bands.
        rec.score = rec.avg_snr +
                    ((rec.link_success - 0.5f) * 18.0f) -
                    (rec.occupancy * 6.0f) +
                    (rec.propagation_score * 0.85f);
        ranked.push_back(rec);
    }

    std::sort(ranked.begin(), ranked.end(), [](const BandRecommendation& a, const BandRecommendation& b) {
        return a.score > b.score;
    });
    return ranked;
}

float bandScoreFor(const std::vector<BandRecommendation>& recs, const std::string& band) {
    for (const auto& rec : recs) {
        if (rec.band == band) {
            return rec.score;
        }
    }
    return -5.0f;
}

struct SlotRecommendation {
    int slot = 0;
    uint64_t frequency_hz = 0;
    float score = -99.0f;
    bool busy = false;
};

std::vector<SlotRecommendation> computeSlotRecommendationsForBand(
    const std::string& band,
    FrequencyManager* freq_mgr,
    const std::vector<std::pair<int, bool>>* sniffer_status,
    float band_score) {
    std::vector<SlotRecommendation> recs;
    if (!freq_mgr) {
        return recs;
    }

    std::map<int, bool> sniffer_busy;
    if (sniffer_status) {
        for (const auto& st : *sniffer_status) {
            sniffer_busy[st.first] = st.second;
        }
    }

    auto slots = freq_mgr->getSlots(band);
    for (const auto& slot : slots) {
        SlotRecommendation rec;
        rec.slot = slot.slot_number;
        rec.frequency_hz = slot.frequency_hz;

        auto it = sniffer_busy.find(slot.slot_number);
        if (it != sniffer_busy.end()) {
            rec.busy = it->second;
        } else {
            rec.busy = slot.status == SlotStatus::InUse;
        }

        rec.score = band_score;
        rec.score += rec.busy ? -12.0f : 2.5f;
        rec.score -= std::abs(slot.slot_number) * 0.35f;
        if (slot.slot_number == 0) {
            rec.score -= 2.0f;
        }
        if (slot.status == SlotStatus::MySlot) {
            rec.score += 2.0f;
        }

        recs.push_back(rec);
    }

    std::sort(recs.begin(), recs.end(), [](const SlotRecommendation& a, const SlotRecommendation& b) {
        return a.score > b.score;
    });
    return recs;
}

}  // namespace

FrequencyWindow::FrequencyWindow(SharedAppState& state)
    : WindowBase(state) {}

bool FrequencyWindow::requestSlotChange(const std::string& band, int slot) {
    if (!state_.freq_mgr || !state_.radio) {
        return false;
    }

    std::string target_band = band;
    if (target_band.empty() && state_.current_band) {
        target_band = *state_.current_band;
    }
    if (target_band.empty()) {
        return false;
    }

    int target_slot = std::clamp(slot, -120, 120);
    if (target_slot == 0) {
        target_slot = 1;
    }

    const ModemStatus st = state_.modem->getStatus();
    const bool station_link = (st.link_state == LinkState::Connected);
    if (station_link && onSendQsySuggestion) {
        const uint64_t freq_hz = state_.freq_mgr->getSlotFrequency(target_band, target_slot);
        onSendQsySuggestion(target_band, target_slot, static_cast<uint32_t>(freq_hz));
        return true;
    }

    if (state_.current_band) {
        *state_.current_band = target_band;
    }
    if (state_.current_slot) {
        *state_.current_slot = target_slot;
    }
    const bool ok = state_.freq_mgr->qsyToSlot(*state_.radio, target_band, target_slot);
    if (ok && state_.current_frequency_hz) {
        *state_.current_frequency_hz = static_cast<uint32_t>(
            state_.freq_mgr->getSlotFrequency(target_band, target_slot));
    }
    return ok;
}

void FrequencyWindow::render() {
    ImGui::Text("Frequency Channel Management");
    ImGui::Separator();

    // Band selection
    ImGui::Text("Band:");
    ImGui::SameLine();

    static const char* bands[] = {"80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m"};
    static int current_band_idx = 3;  // Default 20m

    // Find current band index
    for (int i = 0; i < 8; ++i) {
        if (*state_.current_band == bands[i]) {
            current_band_idx = i;
            break;
        }
    }

    if (ImGui::Combo("##BandSelect", &current_band_idx, bands, 8)) {
        *state_.current_band = bands[current_band_idx];
    }

    // Show calling frequency
    if (state_.freq_mgr) {
        uint64_t calling_freq = state_.freq_mgr->getCallingFrequency(*state_.current_band);
        ImGui::Text("Calling Frequency: %.3f MHz", calling_freq / 1000000.0);

        // QSY to calling frequency button
        if (ImGui::Button("QSY to Calling Freq")) {
            if (state_.radio) {
                if (state_.freq_mgr->qsyToCallingFrequency(*state_.radio, *state_.current_band) &&
                    state_.current_frequency_hz) {
                    *state_.current_frequency_hz = static_cast<uint32_t>(calling_freq);
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Working Slots (500 Hz spacing):");

    // Slot grid
    if (state_.freq_mgr) {
        auto slots = state_.freq_mgr->getSlots(*state_.current_band);

        if (ImGui::BeginChild("SlotGrid", ImVec2(-1, 200), true)) {
            ImGui::Columns(4, "SlotColumns");

            for (const auto& slot : slots) {
                const std::string label = slotLabel(slot.slot_number);

                // Color based on status
                ImVec4 color;
                switch (slot.status) {
                    case SlotStatus::Available:
                        color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
                        break;
                    case SlotStatus::InUse:
                        color = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);
                        break;
                    case SlotStatus::MySlot:
                        color = ImVec4(0.2f, 0.2f, 0.8f, 1.0f);
                        break;
                    default:
                        color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                        break;
                }

                ImGui::PushStyleColor(ImGuiCol_Button, color);
                if (ImGui::Button(label.c_str(), ImVec2(60, 30))) {
                    requestSlotChange(state_.current_band ? *state_.current_band : std::string{},
                                      slot.slot_number);
                }
                ImGui::PopStyleColor();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%.3f MHz\nStatus: %s%s",
                                      slot.frequency_hz / 1000000.0,
                                      slot.status == SlotStatus::Available ? "Available" :
                                      slot.status == SlotStatus::InUse ? "Busy" :
                                      slot.status == SlotStatus::MySlot ? "My Slot" : "Unknown",
                                      slot.occupant.empty() ? "" : ("\nOccupant: " + slot.occupant).c_str());
                }

                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Channel sniffer
    ImGui::Text("Channel Sniffer:");
    if (*state_.channel_sniffer_active) {
        if (ImGui::Button("Stop Sniffer")) {
            if (onStopSniffer) onStopSniffer();
        }
        ImGui::SameLine();
        std::string scanning_slot = "?";
        if (state_.slot_status && *state_.sniffer_current_slot >= 0 &&
            *state_.sniffer_current_slot < static_cast<int>(state_.slot_status->size())) {
            scanning_slot = slotLabel((*state_.slot_status)[*state_.sniffer_current_slot].first);
        }
        ImGui::Text("Scanning slot %s...", scanning_slot.c_str());
    } else {
        if (ImGui::Button("Start Sniffer")) {
            if (onStartSniffer) onStartSniffer();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Scans all slots in current band to detect activity");
    }

    // Find best slot button
    if (ImGui::Button("Find Best Slot")) {
        if (state_.freq_mgr) {
            int best = state_.freq_mgr->findBestSlot(*state_.current_band);
            *state_.current_slot = best;
        }
    }

    // Sniffer results
    renderSnifferResults();

    // Slot legend
    ImGui::Separator();
    ImGui::Text("Legend:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Available");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Busy");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.2f, 0.2f, 0.8f, 1.0f), "My Slot");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Unknown");

    renderRecommendations();
}

void FrequencyWindow::renderSnifferResults() {
    if (!state_.slot_status || state_.slot_status->empty()) return;

    ImGui::Text("Channel Status:");
    for (const auto& status : *state_.slot_status) {
        const std::string label = slotLabel(status.first);
        if (status.second) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  Slot %s: BUSY", label.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "  Slot %s: FREE", label.c_str());
        }
    }
}

void FrequencyWindow::renderRecommendations() {
    ImGui::Separator();
    ImGui::Text("Band/Slot Recommendations:");

    auto band_recs = computeBandRecommendations(state_.store, state_.freq_mgr, state_.config);
    if (band_recs.empty()) {
        ImGui::TextDisabled("No beacon/ping history yet. Hear stations to build recommendations.");
        return;
    }

    int shown = 0;
    for (const auto& rec : band_recs) {
        if (shown++ >= 3) {
            break;
        }
        ImGui::Text("%s  score %.1f  link %.0f%%  occ %.0f%%  prop %.1f",
                    rec.band.c_str(),
                    rec.score,
                    rec.link_success * 100.0f,
                    rec.occupancy * 100.0f,
                    rec.propagation_score);
        if (rec.path_km > 0.0f) {
            ImGui::TextDisabled("Path %.0f km  LUF %.1f MHz  MUF %.1f MHz",
                                rec.path_km,
                                rec.luf_mhz,
                                rec.muf_mhz);
        }
    }

    if (state_.freq_mgr && state_.modem && ImGui::Button("QSY Best Band+Slot")) {
        const std::string& best_band = band_recs.front().band;
        const auto slot_recs = computeSlotRecommendationsForBand(
            best_band,
            state_.freq_mgr,
            state_.slot_status,
            band_recs.front().score);

        int best_slot = 0;
        if (!slot_recs.empty()) {
            best_slot = slot_recs.front().slot;
        } else {
            best_slot = state_.freq_mgr->findBestSlot(best_band);
        }

        requestSlotChange(best_band, best_slot);
    }

    if (state_.freq_mgr && state_.modem && onSendQsySuggestion) {
        const std::string& best_band = band_recs.front().band;
        const auto best_slot_recs = computeSlotRecommendationsForBand(
            best_band,
            state_.freq_mgr,
            state_.slot_status,
            band_recs.front().score);
        int best_slot = best_slot_recs.empty() ? state_.freq_mgr->findBestSlot(best_band)
                                               : best_slot_recs.front().slot;
        const uint32_t best_freq_hz = static_cast<uint32_t>(state_.freq_mgr->getSlotFrequency(best_band, best_slot));
        ModemStatus st = state_.modem->getStatus();
        const bool station_link = (st.link_state == LinkState::Connected);
        if (!station_link) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Suggest Best QSY to Peer")) {
            onSendQsySuggestion(best_band, best_slot, best_freq_hz);
        }
        if (!station_link) {
            ImGui::EndDisabled();
            ImGui::TextDisabled("Connect to a station to send QSY coordination.");
        }
    }

    if (state_.qsy_suggestion_pending && *state_.qsy_suggestion_pending &&
        state_.qsy_suggestion_from && state_.qsy_suggestion_band &&
        state_.qsy_suggestion_slot && state_.qsy_suggestion_frequency_hz &&
        onRespondQsySuggestion) {
        ImGui::Separator();
        ImGui::Text("Incoming QSY Suggestion:");
        ImGui::Text("%s suggests %s slot %+d (%.3f MHz)",
                    state_.qsy_suggestion_from->c_str(),
                    state_.qsy_suggestion_band->c_str(),
                    *state_.qsy_suggestion_slot,
                    static_cast<double>(*state_.qsy_suggestion_frequency_hz) / 1000000.0);
        if (ImGui::Button("Accept and QSY")) {
            onRespondQsySuggestion(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline")) {
            onRespondQsySuggestion(false);
        }
    }

    if (!state_.freq_mgr) {
        return;
    }

    const float current_band_score = bandScoreFor(band_recs, *state_.current_band);
    auto slot_recs = computeSlotRecommendationsForBand(
        *state_.current_band,
        state_.freq_mgr,
        state_.slot_status,
        current_band_score);

    if (slot_recs.empty()) {
        ImGui::TextDisabled("No slot data for %s", state_.current_band->c_str());
        return;
    }

    ImGui::TextDisabled("Current band %s recommendations:", state_.current_band->c_str());
    int limit = 0;
    for (const auto& rec : slot_recs) {
        if (limit++ >= 5) {
            break;
        }

        const std::string slot_id = slotLabel(rec.slot);
        ImGui::Text("Slot %s  %.3f MHz  score %.1f  %s",
                    slot_id.c_str(),
                    rec.frequency_hz / 1000000.0,
                    rec.score,
                    rec.busy ? "BUSY" : "CLEAR");
        if (state_.modem && ImGui::SmallButton(("QSY##rec_slot_" + slot_id).c_str())) {
            requestSlotChange(state_.current_band ? *state_.current_band : std::string{},
                              rec.slot);
        }
        if (limit < 5) {
            ImGui::Separator();
        }
    }
}

} // namespace skylink
