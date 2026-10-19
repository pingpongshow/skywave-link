#include "skywave_link/frequency_window.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modern_style.hpp"
#include "skywave_link/radio_controller.hpp"
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

using BandRecommendation = FrequencyWindow::BandRecommendation;
using gui::slotLabel;

constexpr double kRecommendationRefreshSec = 10.0;

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
        if (std::isfinite(b.snr_db)) {  // unmeasured beacons carry no SNR
            acc.weighted_snr += static_cast<double>(b.snr_db) * weight;
            acc.snr_weight += weight;
        }
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

        if (!p.online || std::isfinite(p.snr_db)) {  // a reply without SNR adds none
            const double ping_snr = p.online ? static_cast<double>(p.snr_db) : -22.0;
            acc.weighted_snr += ping_snr * (0.5 * weight);
            acc.snr_weight += (0.5 * weight);
        }
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

const std::vector<BandRecommendation>& FrequencyWindow::bandRecommendations() {
    const double now = ImGui::GetTime();
    if (recs_computed_at_ < 0.0 || now - recs_computed_at_ >= kRecommendationRefreshSec) {
        recs_cache_ = computeBandRecommendations(state_.store, state_.freq_mgr, state_.config);
        recs_computed_at_ = now;
    }
    return recs_cache_;
}

bool FrequencyWindow::tuneToSlot(const std::string& band, int slot) {
    if (!state_.freq_mgr) {
        postStatus("Frequency manager unavailable", StatusKind::Error);
        return false;
    }
    const uint64_t freq_hz = state_.freq_mgr->getSlotFrequency(band, slot);
    if (freq_hz == 0) {
        postStatus("Slot " + slotLabel(slot) + " does not exist on " + band, StatusKind::Error);
        return false;
    }

    bool ok = false;
    std::string why;
    if (onTuneFrequency) {
        ok = onTuneFrequency(freq_hz);
    } else if (state_.radio && state_.radio->isConnected()) {
        ok = state_.radio->setFrequency(freq_hz);
    } else {
        why = "the radio is not connected (Settings > Radio)";
    }
    if (!ok && why.empty()) {
        why = state_.radio ? state_.radio->getStatus().last_error : std::string();
        if (why.empty()) why = "the radio did not accept the frequency";
    }
    const std::string where = gui::formatDialFrequency(freq_hz) + " (" + band + " " +
                              (slot == 0 ? std::string("calling") : "slot " + slotLabel(slot)) + ")";
    if (!ok) {
        postStatus("Not tuned to " + where + ": " + why, StatusKind::Error);
        return false;
    }

    // Moved: now record where we are.
    if (state_.current_band && state_.current_slot && *state_.current_band == band &&
        *state_.current_slot != 0 && *state_.current_slot != slot) {
        const auto old = state_.freq_mgr->getSlot(band, *state_.current_slot);
        if (old && old->status == SlotStatus::MySlot) {
            state_.freq_mgr->updateSlotStatus(band, *state_.current_slot, SlotStatus::Unknown);
        }
    }
    if (slot != 0) {
        state_.freq_mgr->updateSlotStatus(band, slot, SlotStatus::MySlot);
    }
    if (state_.current_band) *state_.current_band = band;
    if (state_.current_slot) *state_.current_slot = slot;
    if (state_.current_frequency_hz) *state_.current_frequency_hz = static_cast<uint32_t>(freq_hz);

    if (state_.radio && state_.radio->isDryRun()) {
        postStatus("Dry-run radio: " + where + " recorded, but no rig was tuned",
                   StatusKind::Warning);
    } else {
        postStatus("Tuned to " + where, StatusKind::Success);
    }
    return true;
}

void FrequencyWindow::requestSlotChange(const std::string& band, int slot) {
    if (band.empty()) {
        postStatus("Choose a band first", StatusKind::Error);
        return;
    }
    if (gui::isStationLinked(state_.modem)) {
        // Moving alone would drop the link: ask the peer to move with us.
        if (!onSendQsySuggestion || !state_.freq_mgr) {
            postStatus("Linked: QSY coordination is not available, so the rig was not moved",
                       StatusKind::Error);
            return;
        }
        const uint64_t freq_hz = state_.freq_mgr->getSlotFrequency(band, slot);
        if (freq_hz == 0) {
            postStatus("Slot " + slotLabel(slot) + " does not exist on " + band, StatusKind::Error);
            return;
        }
        const size_t log_mark = state_.chatHistorySize();
        onSendQsySuggestion(band, slot, static_cast<uint32_t>(freq_hz));
        const std::string logged = state_.systemMessageSince(log_mark);
        const std::string peer = state_.modem->getStatus().remote_callsign;
        postStatus(logged.empty() ? "QSY to " + band + " " + slotLabel(slot) + " suggested to " +
                                        peer + "; both stations move once it accepts"
                                  : logged,
                   StatusKind::Info);
        return;
    }
    (void)tuneToSlot(band, slot);
}

void FrequencyWindow::render() {
    const ui::Palette& pal = ui::palette();
    if (!state_.current_band) return;

    ImGui::Text("Frequency Channel Management");
    ImGui::Separator();
    ui::StatusLine(status_);

    // Where the rig is (changes only after a successful tune).
    {
        const uint32_t hz = state_.current_frequency_hz ? *state_.current_frequency_hz : 0;
        const int slot = state_.current_slot ? *state_.current_slot : 0;
        ImGui::Text("Tuned: %s %s  %s", state_.current_band->c_str(),
                    slot == 0 ? "calling" : ("slot " + slotLabel(slot)).c_str(),
                    hz ? gui::formatDialFrequency(hz).c_str() : "(frequency unknown)");
        if (state_.radio && state_.radio->isDryRun()) {
            ui::WarningText("Dry-run radio: tuning is simulated, the rig does not move.");
        } else if (!onTuneFrequency && !(state_.radio && state_.radio->isConnected())) {
            ImGui::TextDisabled("No radio control: set up Settings > Radio to tune from here.");
        }
    }

    // Band whose slots are shown (viewing does not tune).
    if (view_band_.empty()) view_band_ = *state_.current_band;
    std::vector<BandConfig> bands;
    if (state_.freq_mgr) bands = gui::enabledBands(*state_.freq_mgr);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Show band:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::BeginCombo("##BandSelect", view_band_.c_str())) {
        for (const auto& band : bands) {
            if (ImGui::Selectable(band.name.c_str(), band.name == view_band_)) {
                view_band_ = band.name;
            }
        }
        ImGui::EndCombo();
    }

    // Calling frequency of the shown band
    if (state_.freq_mgr) {
        const uint64_t calling_freq = state_.freq_mgr->getCallingFrequency(view_band_);
        ImGui::SameLine();
        ImGui::Text("Calling: %s", gui::formatDialFrequency(calling_freq).c_str());
        ImGui::SameLine();
        if (ImGui::Button("QSY to Calling Freq")) {
            if (gui::isStationLinked(state_.modem)) {
                postStatus("Linked to " + state_.modem->getStatus().remote_callsign +
                               ": disconnect first, or the link drops when the rig moves",
                           StatusKind::Error);
            } else {
                (void)tuneToSlot(view_band_, 0);
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Working Slots (500 Hz spacing):");

    // Slot grid
    if (state_.freq_mgr) {
        auto slots = state_.freq_mgr->getSlots(view_band_);

        if (ImGui::BeginChild("SlotGrid", ImVec2(-1, ImGui::GetFontSize() * 14.0f), ImGuiChildFlags_Borders)) {
            ImGui::Columns(4, "SlotColumns", false);

            for (const auto& slot : slots) {
                const std::string label = slot.slot_number == 0 ? std::string("0 (call)")
                                                                : slotLabel(slot.slot_number);

                // Color based on status (theme palette; unknown keeps the
                // theme's neutral button colour)
                ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Button);
                switch (slot.status) {
                    case SlotStatus::Available:
                        color = pal.success;
                        break;
                    case SlotStatus::InUse:
                        color = pal.danger;
                        break;
                    case SlotStatus::MySlot:
                        color = pal.accent;
                        break;
                    default:
                        break;
                }

                ui::PushButtonColor(color);
                if (ImGui::Button(label.c_str(),
                                  ImVec2(ImGui::GetFontSize() * 4.5f, ImGui::GetFrameHeight() * 1.15f))) {
                    requestSlotChange(view_band_, slot.slot_number);
                }
                ui::PopButtonColor();

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nStatus: %s%s",
                                      gui::formatDialFrequency(slot.frequency_hz).c_str(),
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

    // Slot legend
    ImGui::TextDisabled("Legend:");
    ImGui::SameLine();
    ImGui::TextColored(pal.success, "Available");
    ImGui::SameLine();
    ImGui::TextColored(pal.danger, "Busy");
    ImGui::SameLine();
    ImGui::TextColored(pal.accent, "My Slot");
    ImGui::SameLine();
    ImGui::TextColored(pal.muted, "Unknown");

    ImGui::Separator();

    // Channel sniffer (scans the tuned band)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Channel Sniffer:");
    ImGui::SameLine();
    if (state_.channel_sniffer_active && *state_.channel_sniffer_active) {
        if (ImGui::Button("Stop Sniffer")) {
            if (onStopSniffer) onStopSniffer();
            postStatus("Sniffer stopped", StatusKind::Info);
        }
        ImGui::SameLine();
        std::string scanning_slot = "?";
        if (state_.slot_status && state_.sniffer_current_slot && *state_.sniffer_current_slot >= 0 &&
            *state_.sniffer_current_slot < static_cast<int>(state_.slot_status->size())) {
            scanning_slot = slotLabel((*state_.slot_status)[*state_.sniffer_current_slot].first);
        }
        ImGui::Text("Scanning slot %s...", scanning_slot.c_str());
    } else {
        if (ImGui::Button("Start Sniffer")) {
            if (!onStartSniffer) {
                postStatus("Channel sniffer not available", StatusKind::Error);
            } else {
                const size_t log_mark = state_.chatHistorySize();
                onStartSniffer();
                if (state_.channel_sniffer_active && *state_.channel_sniffer_active) {
                    postStatus("Sniffer scanning " + *state_.current_band, StatusKind::Info);
                } else {
                    const std::string why = state_.systemMessageSince(log_mark);
                    postStatus(why.empty() ? std::string("Sniffer did not start (needs the modem "
                                                         "and radio control)")
                                           : why,
                               StatusKind::Error);
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Steps the rig through the tuned band (%s) to find busy slots",
                            state_.current_band->c_str());
    }

    // Find best slot: QSY to it (a suggestion to the peer while linked).
    if (ImGui::Button("QSY to Best Slot")) {
        if (state_.freq_mgr) {
            const int best = state_.freq_mgr->findBestSlot(view_band_);
            const auto info = state_.freq_mgr->getSlot(view_band_, best);
            requestSlotChange(view_band_, best);
            if (info && info->status != SlotStatus::Available && status_.kind != StatusKind::Error) {
                postStatus(status_.text + " (no slot is known to be clear yet; run the sniffer "
                                          "to check)",
                           StatusKind::Warning);
            }
        }
    }
    ImGui::SetItemTooltip("Picks the best slot on the shown band and moves there.");

    // Sniffer results
    renderSnifferResults();

    renderRecommendations();
}

void FrequencyWindow::renderSnifferResults() {
    if (!state_.slot_status || state_.slot_status->empty()) return;

    ImGui::Text("Channel Status:");
    for (const auto& status : *state_.slot_status) {
        const std::string label = slotLabel(status.first);
        if (status.second) {
            ImGui::TextColored(ui::palette().danger, "  Slot %s: BUSY", label.c_str());
        } else {
            ImGui::TextColored(ui::palette().success, "  Slot %s: FREE", label.c_str());
        }
    }
}

void FrequencyWindow::renderRecommendations() {
    ImGui::Separator();
    ImGui::Text("Band/Slot Recommendations:");

    const auto& band_recs = bandRecommendations();
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

    const std::string best_band = band_recs.front().band;
    int best_slot = 0;
    if (state_.freq_mgr) {
        const auto slot_recs = computeSlotRecommendationsForBand(
            best_band, state_.freq_mgr, state_.slot_status, band_recs.front().score);
        best_slot = slot_recs.empty() ? state_.freq_mgr->findBestSlot(best_band)
                                      : slot_recs.front().slot;
    }
    const bool station_link = gui::isStationLinked(state_.modem);

    if (state_.freq_mgr && ImGui::Button(station_link ? "Suggest Best Band+Slot to Peer"
                                                      : "QSY Best Band+Slot")) {
        requestSlotChange(best_band, best_slot);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", best_band.c_str(), slotLabel(best_slot).c_str());

    if (state_.qsy_suggestion_pending && *state_.qsy_suggestion_pending &&
        state_.qsy_suggestion_from && state_.qsy_suggestion_band &&
        state_.qsy_suggestion_slot && state_.qsy_suggestion_frequency_hz &&
        onRespondQsySuggestion) {
        ImGui::Separator();
        ImGui::Text("Incoming QSY Suggestion:");
        ImGui::Text("%s suggests %s slot %s (%s)",
                    state_.qsy_suggestion_from->c_str(),
                    state_.qsy_suggestion_band->c_str(),
                    slotLabel(*state_.qsy_suggestion_slot).c_str(),
                    gui::formatDialFrequency(*state_.qsy_suggestion_frequency_hz).c_str());
        auto respond = [&](bool accept) {
            const size_t log_mark = state_.chatHistorySize();
            onRespondQsySuggestion(accept);
            const std::string logged = state_.systemMessageSince(log_mark);
            postStatus(logged.empty() ? (accept ? "QSY accepted" : "QSY declined") : logged,
                       StatusKind::Info);
        };
        if (ImGui::Button("Accept and QSY")) {
            respond(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline")) {
            respond(false);
        }
    }

    if (!state_.freq_mgr) {
        return;
    }

    const float view_band_score = bandScoreFor(band_recs, view_band_);
    auto slot_recs = computeSlotRecommendationsForBand(
        view_band_,
        state_.freq_mgr,
        // Sniffer results describe the tuned band only.
        view_band_ == *state_.current_band ? state_.slot_status : nullptr,
        view_band_score);

    if (slot_recs.empty()) {
        ImGui::TextDisabled("No slot data for %s", view_band_.c_str());
        return;
    }

    ImGui::TextDisabled("%s recommendations:", view_band_.c_str());
    int limit = 0;
    for (const auto& rec : slot_recs) {
        if (limit++ >= 5) {
            break;
        }

        const std::string slot_id = slotLabel(rec.slot);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Slot %s  %s  score %.1f  %s",
                    slot_id.c_str(),
                    gui::formatDialFrequency(rec.frequency_hz).c_str(),
                    rec.score,
                    rec.busy ? "BUSY" : "CLEAR");
        ImGui::SameLine();
        if (ImGui::SmallButton(("QSY##rec_slot_" + slot_id).c_str())) {
            requestSlotChange(view_band_, rec.slot);
        }
    }
}

} // namespace skylink
