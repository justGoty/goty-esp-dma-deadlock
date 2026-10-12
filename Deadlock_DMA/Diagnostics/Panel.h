#pragma once
#include "Diagnostics.h"
#include "Deadlock/FieldOffsets.h"
#include "imgui.h"
#include <cfloat>
#include <format>

namespace DiagnosticsPanel {
// UTF-8 bytes keep these labels independent of the compiler's source code page.
inline constexpr char GameLabel[] = "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0"; // Game
inline constexpr char VisibleLabel[] = "\xD0\x92\xD0\xB8\xD0\xB4\xD0\xB8\xD1\x82"; // Visible
inline constexpr char HiddenLabel[] = "\xD0\x9D\xD0\xB5 \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB8\xD1\x82"; // Not visible
inline constexpr char DmaConnecting[] = "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82\xD1\x81\xD1\x8F\x2E\x2E\x2E";
inline constexpr char DmaConnected[] = "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB0";
inline constexpr char DmaDisconnected[] = "\xD0\x9D\xD0\xB5\x20\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB0";
inline constexpr char DmaFailed[] = "\xD0\x9E\xD1\x88\xD0\xB8\xD0\xB1\xD0\xBA\xD0\xB0";
inline constexpr char DmaInactive[] = "\xD0\x9E\xD1\x82\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB0";

inline const char* DescribeStage(const Diagnostics::Snapshot& snapshot) {
    if (snapshot.stage == "starting") return "\xD0\x97\xD0\xB0\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA";
    if (snapshot.stage == "connecting_dma") return "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5\x20\x44\x4D\x41";
    if (snapshot.stage == "waiting_process") return "\xD0\x9F\xD0\xBE\xD0\xB8\xD1\x81\xD0\xBA\x20\x44\x65\x61\x64\x6C\x6F\x63\x6B";
    if (snapshot.stage == "waiting_modules") return "\xD0\x98\xD0\xB3\xD1\x80\xD0\xB0\x20\xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD\xD0\xB0";
    if (snapshot.stage == "resolving_offsets") return "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xB3\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBA\xD0\xB0\x20\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD1\x85";
    if (snapshot.stage == "running") return "\xD0\xA0\xD0\xB0\xD0\xB1\xD0\xBE\xD1\x82\xD0\xB0\xD0\xB5\xD1\x82";
    if (snapshot.stage == "failed") return DmaFailed;
    if (snapshot.stage == "stopped") return "\xD0\x9E\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xBE";
    return "\xD0\x9E\xD0\xB6\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5";
}

struct ConnectionStatus {
    const char* dma{};
    bool dmaConnected{}, gameVisible{}, connecting{};
    uint32_t pid{};
};

inline ConnectionStatus DescribeConnection(const Diagnostics::Snapshot& snapshot) {
    const bool terminal = snapshot.stage == "failed" || snapshot.stage == "stopped";
    const bool connecting = !terminal && snapshot.stage == "connecting_dma";
    const bool connected = !terminal && !connecting && snapshot.stage != "starting" && snapshot.dmaConnected;
    const bool visible = connected && Diagnostics::HasFreshGameConnection(snapshot);
    return { terminal ? (snapshot.stage == "failed" ? DmaFailed : DmaInactive)
                      : connecting ? DmaConnecting : connected ? DmaConnected : DmaDisconnected,
             connected, visible, connecting, visible ? snapshot.targetPid : 0 };
}

inline ImVec4 StateColor(bool active, bool waiting = false) {
    return active ? ImVec4(0.38f, 0.86f, 0.56f, 1.0f)
                  : waiting ? ImVec4(1.0f, 0.78f, 0.32f, 1.0f)
                            : ImVec4(0.95f, 0.43f, 0.43f, 1.0f);
}

inline void RenderConnectionStatus(const Diagnostics::Snapshot& snapshot) {
    const auto status = DescribeConnection(snapshot);
    ImGui::TextColored(StateColor(status.dmaConnected, status.connecting), "DMA: %s", status.dma);
    ImGui::SameLine(0.0f, 24.0f);
    if (status.gameVisible)
        ImGui::TextColored(StateColor(true), "%s: %s  (PID %u)", GameLabel, VisibleLabel, status.pid);
    else
        ImGui::TextColored(StateColor(false), "%s: %s", GameLabel, HiddenLabel);

    // Resolver/worker readiness is separate from remote process visibility.
    ImGui::TextDisabled("\xD0\xAD\xD1\x82\xD0\xB0\xD0\xBF\x3A\x20\x25\x73\x20\x7C\x20\x25\x2E\x31\x66\x20\xD1\x81", DescribeStage(snapshot), snapshot.stageAgeSeconds);
}

inline void RenderDetails(const Diagnostics::Snapshot& snapshot) {
    if (ImGui::BeginTable("DiagnosticsMetrics", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        const auto row = [](const char* name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(value.c_str());
        };
        row("Version", Diagnostics::Version);
        row("Expected layout", Offsets::LayoutTag);
        row("Schema source date", Offsets::SchemaDate);
        row("Stage", std::format("{} / {:.1f} s", snapshot.stage, snapshot.stageAgeSeconds));
        row("Target PID / Uptime", std::format("{} / {:.1f} s", snapshot.targetPid, snapshot.uptimeSeconds));
        row("Worker / Frame age", std::format("{:.2f} / {:.2f} s", snapshot.workerAgeSeconds, snapshot.frameAgeSeconds));
        row("Reads / Failed or short", std::format("{} / {}", snapshot.reads, snapshot.failedReads));
        row("Read / Requested bytes", std::format("{} / {}", snapshot.readBytes, snapshot.requestedBytes));
        row("Batches / Incomplete / Open", std::format("{} / {} / {}", snapshot.batches, snapshot.failedBatches, snapshot.openScatter));
        row("Slow loops / Max loop", std::format("{} / {:.2f} ms", snapshot.slowLoops, snapshot.maxLoopMs));
        row("Frame interval avg / max (120)", std::format("{:.2f} / {:.2f} ms", snapshot.frameWindowAverageMs, snapshot.frameWindowMaxMs));
        row("Frames >50 ms / Max interval", std::format("{} / {:.2f} ms", snapshot.slowFrames, snapshot.maxFrameMs));
        row("Pawn publish age / Max gap", std::format("{:.3f} s / {:.2f} ms", snapshot.pawnPublishAgeSeconds, snapshot.maxPawnPublishGapMs));
        for (size_t i = 0; i < snapshot.timingLaneCount; ++i) {
            const auto& lane = snapshot.timingLanes[i];
            row(lane.name.data(), std::format("{:.2f} / {:.2f} ms | {} calls / {} failed", lane.lastMs, lane.maxMs, lane.calls, lane.failed));
        }
        row("Pawns / Controllers", std::format("{} / {}", snapshot.pawns, snapshot.controllers));
        row("Bone read requests", std::format("{}", snapshot.boneReadRequests));
        row("Troopers / Camps / Sinners", std::format("{} / {} / {}", snapshot.troopers, snapshot.camps, snapshot.sinners));
        ImGui::EndTable();
    }
    if (!snapshot.lastError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(StateColor(false), "Last error");
        ImGui::TextWrapped("%s", snapshot.lastError.c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Logs");
    ImGui::TextWrapped("%s", snapshot.logDirectory.c_str());
    ImGui::TextDisabled("Entity counts describe the cache.");
}
}

// Draw directly into Fuser; no extra diagnostics window or docking node.
inline void DrawConnectionStatusBadge(ImVec2 position) {
    const auto status = DiagnosticsPanel::DescribeConnection(Diagnostics::GetSnapshot());
    const auto text = status.gameVisible
        ? std::format("DMA: {} | {}: {} ({})", status.dma, DiagnosticsPanel::GameLabel, DiagnosticsPanel::VisibleLabel, status.pid)
        : std::format("DMA: {} | {}: {}", status.dma, DiagnosticsPanel::GameLabel, DiagnosticsPanel::HiddenLabel);
    constexpr float fontSize = 14.0f;
    const auto size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(ImVec2(position.x - 8.0f, position.y - 5.0f),
                           ImVec2(position.x + size.x + 8.0f, position.y + size.y + 5.0f),
                           IM_COL32(18, 22, 28, 225), 6.0f);
    const auto color = DiagnosticsPanel::StateColor(status.gameVisible, status.connecting || status.dmaConnected);
    drawList->AddText(ImGui::GetFont(), fontSize, position, ImGui::ColorConvertFloat4ToU32(color), text.c_str());
}
