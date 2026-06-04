#pragma once

// Thesis-instrumentation surface. Owns the four operationalized metrics:
//   - Cold start: process launch -> first rendered frame
//   - Recompilation time: clang Parse() -> JitProgram ready
//   - Edit-to-display latency: file change detected -> first frame from new program
//   - Peak/current memory: /proc/self/status VmPeak / VmRSS
//
// All record/observe methods are thread-safe so the watcher thread, JIT compile
// thread, and main render thread can all push samples without coordination.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class MetricsRegistry {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    struct LatencySample {
        // Wall-clock milliseconds since epoch — used as the x value when
        // exporting CSV; ImGui graphs use sample index instead.
        std::uint64_t timestampMs = 0;
        // Time from watcher/editor edit detection to the start of the
        // compile worker's work on this job. Includes debounce + queueing.
        double detectionDelayMs = 0.0;
        // Parse() -> JitProgram ready. Matches the "recompilation time"
        // metric defined in the thesis.
        double compileMs = 0.0;
        // Compile completion to first renderFrame call returning. Picks up
        // main-thread queue drain + first-frame init cost.
        double installToRenderMs = 0.0;
        // Sum of the above three. Edit-to-display end-to-end.
        double totalMs = 0.0;
        std::string workspaceName;
    };

    struct RecompileSample {
        std::uint64_t timestampMs = 0;
        double parseToReadyMs = 0.0;
        std::string workspaceName;
    };

    struct Stats {
        std::size_t count = 0;
        double min = 0.0;
        double max = 0.0;
        double mean = 0.0;
        double p50 = 0.0;
        double p95 = 0.0;
        double last = 0.0;
    };

    struct MemorySnapshot {
        // All values are kilobytes as reported by /proc/self/status.
        std::size_t vmPeakKb = 0;   // peak virtual size
        std::size_t vmSizeKb = 0;   // current virtual size
        std::size_t vmHwmKb = 0;    // peak resident set size
        std::size_t vmRssKb = 0;    // current resident set size
        std::uint64_t timestampMs = 0;
        bool valid = false;
    };

    static constexpr std::size_t kMaxSamples = 512;

    MetricsRegistry();

    // Process launch anchor. Captured once from main() before Engine init so
    // cold-start measurements include window/GL/UI setup. Calling twice is a
    // no-op; the first set wins.
    static void SetProcessStartTime(time_point t);
    static time_point GetProcessStartTime();

    // Cold start: invoked from the render loop once, the moment we know the
    // first frame from the default workspace finished swapping. Subsequent
    // calls are ignored so reloads don't reset the value — use
    // ReArmColdStart() if you want to measure another window.
    void RecordFirstFrameRendered();
    bool HasColdStart() const;
    double GetColdStartMs() const;
    // Re-anchor the cold-start clock to `newAnchor` and clear the captured
    // flag. The next RecordFirstFrameRendered() will measure from there.
    // Used by the metrics window's "Simulate Cold Start" button.
    void ReArmColdStart(time_point newAnchor);

    // Recompilation time. Pushed from the JIT worker each successful
    // compile. Stored independently of LatencySample so unrelated compiles
    // (no preceding edit event) still contribute to the recompile stats.
    void RecordRecompile(const std::string& workspaceName, double parseToReadyMs);
    // Pass workspaceFilter to restrict to a single workspace; nullptr
    // aggregates across all workspaces.
    Stats GetRecompileStats(const std::string* workspaceFilter = nullptr) const;
    std::vector<float> GetRecompileHistory(const std::string* workspaceFilter = nullptr) const;

    // Edit-to-display latency end-to-end. Pushed from the render thread once
    // the new program's first renderFrame returns.
    void RecordLatencySample(const LatencySample& sample);
    Stats GetLatencyStats(const std::string* workspaceFilter = nullptr) const;
    Stats GetDetectionDelayStats(const std::string* workspaceFilter = nullptr) const;
    Stats GetCompileBreakdownStats(const std::string* workspaceFilter = nullptr) const;
    Stats GetInstallToRenderStats(const std::string* workspaceFilter = nullptr) const;
    std::vector<float> GetLatencyHistory(const std::string* workspaceFilter = nullptr) const;
    std::vector<LatencySample> GetRecentLatencySamples(std::size_t maxSamples,
                                                       const std::string* workspaceFilter = nullptr) const;

    // Memory. Cheap enough to refresh every render frame; we still throttle
    // internally to ~250ms intervals so /proc reads don't dominate flamegraphs.
    void RefreshMemorySnapshot();
    MemorySnapshot GetMemorySnapshot() const;

    // Wipe all samples (cold start, latency, recompile, memory peak shadow).
    // Useful when collecting fresh data for a thesis run without restarting
    // the process.
    void Reset();
    // Drop only the samples belonging to `workspaceName`. Cold start +
    // memory stay untouched since they're process-wide.
    void ResetWorkspace(const std::string& workspaceName);

    // Dump all collected samples to CSV. Returns false on file open failure.
    bool ExportToCsv(const std::string& path) const;

private:
    static Stats ComputeStats(const std::vector<double>& samples);
    static std::vector<float> ToFloatVector(const std::vector<double>& samples);

    mutable std::mutex mu_;

    bool coldStartCaptured_ = false;
    double coldStartMs_ = 0.0;

    std::deque<RecompileSample> recompileSamples_;
    std::deque<LatencySample> latencySamples_;

    MemorySnapshot memorySnapshot_{};
    double lastMemoryRefreshSeconds_ = -1.0;
};
