#include "system/MetricsRegistry.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <numeric>
#include <string_view>

namespace {
    using clock = MetricsRegistry::clock;
    using time_point = MetricsRegistry::time_point;

    // Anchor captured at process start (or at first registry construction,
    // whichever happens first). Held in a function-local static so the
    // initialization order across translation units is well-defined.
    struct ProcessAnchor {
        std::atomic<bool> set{false};
        time_point start{};
    };

    ProcessAnchor& anchor() {
        static ProcessAnchor instance{};
        return instance;
    }

    std::uint64_t NowUnixMs() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    double NowMonotonicSeconds() {
        using namespace std::chrono;
        return duration<double>(clock::now().time_since_epoch()).count();
    }

    template <typename T>
    void TrimDeque(std::deque<T>& d) {
        while (d.size() > MetricsRegistry::kMaxSamples) {
            d.pop_front();
        }
    }

    bool MatchesFilter(const std::string& sampleWorkspace,
                       const std::string* workspaceFilter) {
        return workspaceFilter == nullptr || *workspaceFilter == sampleWorkspace;
    }

    bool ParseKbField(std::string_view line, std::size_t* outKb) {
        // Lines from /proc/self/status look like:  "VmPeak:\t   12345 kB"
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        auto rest = line.substr(colon + 1);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
            rest.remove_prefix(1);
        }
        std::size_t value = 0;
        const auto* begin = rest.data();
        const auto* end = begin + rest.size();
        const auto parseResult = std::from_chars(begin, end, value);
        if (parseResult.ec != std::errc()) {
            return false;
        }
        *outKb = value;
        return true;
    }
}

void MetricsRegistry::SetProcessStartTime(time_point t) {
    auto& a = anchor();
    bool expected = false;
    if (a.set.compare_exchange_strong(expected, true)) {
        a.start = t;
    }
}

MetricsRegistry::time_point MetricsRegistry::GetProcessStartTime() {
    auto& a = anchor();
    if (!a.set.load()) {
        SetProcessStartTime(clock::now());
    }
    return a.start;
}

namespace {
    // Force-set the process anchor, bypassing the set-once gate. Used by
    // ReArmColdStart so the user can run repeated cold-start measurements
    // without restarting the process.
    void ResetProcessStartTime(MetricsRegistry::time_point t) {
        auto& a = anchor();
        a.start = t;
        a.set.store(true);
    }
}

MetricsRegistry::MetricsRegistry() {
    // Lazy-init the anchor in case nothing else has set it yet. This catches
    // tests that instantiate MetricsRegistry directly without going through
    // main().
    (void)GetProcessStartTime();
}

void MetricsRegistry::RecordFirstFrameRendered() {
    std::scoped_lock lock(mu_);
    if (coldStartCaptured_) {
        return;
    }
    using namespace std::chrono;
    const auto delta = clock::now() - GetProcessStartTime();
    coldStartMs_ = duration<double, std::milli>(delta).count();
    coldStartCaptured_ = true;
}

bool MetricsRegistry::HasColdStart() const {
    std::scoped_lock lock(mu_);
    return coldStartCaptured_;
}

double MetricsRegistry::GetColdStartMs() const {
    std::scoped_lock lock(mu_);
    return coldStartMs_;
}

void MetricsRegistry::RecordRecompile(const std::string& workspaceName, double parseToReadyMs) {
    if (parseToReadyMs < 0.0) {
        return;
    }
    std::scoped_lock lock(mu_);
    RecompileSample sample;
    sample.timestampMs = NowUnixMs();
    sample.parseToReadyMs = parseToReadyMs;
    sample.workspaceName = workspaceName;
    recompileSamples_.push_back(std::move(sample));
    TrimDeque(recompileSamples_);
}

MetricsRegistry::Stats MetricsRegistry::GetRecompileStats(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    std::vector<double> filtered;
    filtered.reserve(recompileSamples_.size());
    for (const auto& s : recompileSamples_) {
        if (MatchesFilter(s.workspaceName, workspaceFilter)) {
            filtered.push_back(s.parseToReadyMs);
        }
    }
    return ComputeStats(filtered);
}

std::vector<float> MetricsRegistry::GetRecompileHistory(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    std::vector<double> filtered;
    filtered.reserve(recompileSamples_.size());
    for (const auto& s : recompileSamples_) {
        if (MatchesFilter(s.workspaceName, workspaceFilter)) {
            filtered.push_back(s.parseToReadyMs);
        }
    }
    return ToFloatVector(filtered);
}

void MetricsRegistry::RecordLatencySample(const LatencySample& sample) {
    std::scoped_lock lock(mu_);
    latencySamples_.push_back(sample);
    TrimDeque(latencySamples_);
}

namespace {
    template <typename Projector>
    std::vector<double> ProjectFiltered(const std::deque<MetricsRegistry::LatencySample>& samples,
                                        const std::string* workspaceFilter,
                                        Projector project) {
        std::vector<double> out;
        out.reserve(samples.size());
        for (const auto& s : samples) {
            if (MatchesFilter(s.workspaceName, workspaceFilter)) {
                out.push_back(project(s));
            }
        }
        return out;
    }
}

MetricsRegistry::Stats MetricsRegistry::GetLatencyStats(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    return ComputeStats(ProjectFiltered(latencySamples_, workspaceFilter,
                                        [](const LatencySample& s) { return s.totalMs; }));
}

MetricsRegistry::Stats MetricsRegistry::GetDetectionDelayStats(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    return ComputeStats(ProjectFiltered(latencySamples_, workspaceFilter,
                                        [](const LatencySample& s) { return s.detectionDelayMs; }));
}

MetricsRegistry::Stats MetricsRegistry::GetCompileBreakdownStats(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    return ComputeStats(ProjectFiltered(latencySamples_, workspaceFilter,
                                        [](const LatencySample& s) { return s.compileMs; }));
}

MetricsRegistry::Stats MetricsRegistry::GetInstallToRenderStats(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    return ComputeStats(ProjectFiltered(latencySamples_, workspaceFilter,
                                        [](const LatencySample& s) { return s.installToRenderMs; }));
}

std::vector<float> MetricsRegistry::GetLatencyHistory(const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    return ToFloatVector(ProjectFiltered(latencySamples_, workspaceFilter,
                                         [](const LatencySample& s) { return s.totalMs; }));
}

std::vector<MetricsRegistry::LatencySample>
MetricsRegistry::GetRecentLatencySamples(std::size_t maxSamples,
                                         const std::string* workspaceFilter) const {
    std::scoped_lock lock(mu_);
    std::vector<LatencySample> out;
    out.reserve(std::min(maxSamples, latencySamples_.size()));
    for (auto it = latencySamples_.rbegin(); it != latencySamples_.rend(); ++it) {
        if (!MatchesFilter(it->workspaceName, workspaceFilter)) {
            continue;
        }
        out.push_back(*it);
        if (out.size() >= maxSamples) {
            break;
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

void MetricsRegistry::RefreshMemorySnapshot() {
    const double now = NowMonotonicSeconds();
    {
        std::scoped_lock lock(mu_);
        if (lastMemoryRefreshSeconds_ >= 0.0 && (now - lastMemoryRefreshSeconds_) < 0.25) {
            // Throttle to ~4 Hz. /proc/self/status is cheap but not free.
            return;
        }
        lastMemoryRefreshSeconds_ = now;
    }

    MemorySnapshot snapshot;
    snapshot.timestampMs = NowUnixMs();
#if defined(__linux__)
    std::ifstream f("/proc/self/status");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            std::size_t kb = 0;
            std::string_view view(line);
            if (view.starts_with("VmPeak:")) {
                if (ParseKbField(view, &kb)) snapshot.vmPeakKb = kb;
            } else if (view.starts_with("VmSize:")) {
                if (ParseKbField(view, &kb)) snapshot.vmSizeKb = kb;
            } else if (view.starts_with("VmHWM:")) {
                if (ParseKbField(view, &kb)) snapshot.vmHwmKb = kb;
            } else if (view.starts_with("VmRSS:")) {
                if (ParseKbField(view, &kb)) snapshot.vmRssKb = kb;
            }
        }
        snapshot.valid = snapshot.vmPeakKb > 0 || snapshot.vmSizeKb > 0;
    }
#endif

    std::scoped_lock lock(mu_);
    // Keep the highest VmPeak we've ever seen even if /proc transiently
    // reports something lower — VmPeak is monotonic in the kernel but be
    // defensive in case the snapshot fails on one read.
    if (snapshot.vmPeakKb < memorySnapshot_.vmPeakKb) {
        snapshot.vmPeakKb = memorySnapshot_.vmPeakKb;
    }
    if (snapshot.vmHwmKb < memorySnapshot_.vmHwmKb) {
        snapshot.vmHwmKb = memorySnapshot_.vmHwmKb;
    }
    memorySnapshot_ = snapshot;
}

MetricsRegistry::MemorySnapshot MetricsRegistry::GetMemorySnapshot() const {
    std::scoped_lock lock(mu_);
    return memorySnapshot_;
}

void MetricsRegistry::Reset() {
    std::scoped_lock lock(mu_);
    coldStartCaptured_ = false;
    coldStartMs_ = 0.0;
    recompileSamples_.clear();
    latencySamples_.clear();
    memorySnapshot_ = MemorySnapshot{};
    lastMemoryRefreshSeconds_ = -1.0;
}

void MetricsRegistry::ResetWorkspace(const std::string& workspaceName) {
    std::scoped_lock lock(mu_);
    std::erase_if(recompileSamples_,
                  [&](const RecompileSample& s) { return s.workspaceName == workspaceName; });
    std::erase_if(latencySamples_,
                  [&](const LatencySample& s) { return s.workspaceName == workspaceName; });
}

void MetricsRegistry::ReArmColdStart(time_point newAnchor) {
    ResetProcessStartTime(newAnchor);
    std::scoped_lock lock(mu_);
    coldStartCaptured_ = false;
    coldStartMs_ = 0.0;
}

bool MetricsRegistry::ExportToCsv(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    Stats recompile;
    Stats latency;
    Stats detection;
    Stats compile;
    Stats install;
    std::vector<LatencySample> latencyRows;
    std::vector<RecompileSample> recompileRows;
    MemorySnapshot memory;
    bool coldStartValid = false;
    double coldStart = 0.0;

    {
        std::scoped_lock lock(mu_);
        std::vector<double> recompileMs;
        recompileMs.reserve(recompileSamples_.size());
        for (const auto& s : recompileSamples_) {
            recompileMs.push_back(s.parseToReadyMs);
        }
        recompile = ComputeStats(recompileMs);
        latency = ComputeStats(ProjectFiltered(latencySamples_, nullptr,
                                               [](const LatencySample& s) { return s.totalMs; }));
        detection = ComputeStats(ProjectFiltered(latencySamples_, nullptr,
                                                 [](const LatencySample& s) { return s.detectionDelayMs; }));
        compile = ComputeStats(ProjectFiltered(latencySamples_, nullptr,
                                               [](const LatencySample& s) { return s.compileMs; }));
        install = ComputeStats(ProjectFiltered(latencySamples_, nullptr,
                                               [](const LatencySample& s) { return s.installToRenderMs; }));
        latencyRows.assign(latencySamples_.begin(), latencySamples_.end());
        recompileRows.assign(recompileSamples_.begin(), recompileSamples_.end());
        memory = memorySnapshot_;
        coldStartValid = coldStartCaptured_;
        coldStart = coldStartMs_;
    }

    // Summary block — one logical section, terminated by a blank line so
    // downstream tools (pandas) can split on it.
    out << "metric,count,min_ms,mean_ms,max_ms,p50_ms,p95_ms,last_ms\n";
    auto writeStatsRow = [&](const char* name, const Stats& s) {
        out << name << ',' << s.count << ',' << s.min << ',' << s.mean << ','
            << s.max << ',' << s.p50 << ',' << s.p95 << ',' << s.last << '\n';
    };
    writeStatsRow("recompile_time", recompile);
    writeStatsRow("edit_to_display_total", latency);
    writeStatsRow("detection_delay", detection);
    writeStatsRow("compile_breakdown", compile);
    writeStatsRow("install_to_render", install);

    out << '\n';
    out << "cold_start_ms," << (coldStartValid ? coldStart : -1.0) << '\n';
    out << "vm_peak_kb," << memory.vmPeakKb << '\n';
    out << "vm_size_kb," << memory.vmSizeKb << '\n';
    out << "vm_hwm_kb," << memory.vmHwmKb << '\n';
    out << "vm_rss_kb," << memory.vmRssKb << '\n';

    out << '\n';
    out << "latency_timestamp_ms,workspace,detection_delay_ms,compile_ms,install_to_render_ms,total_ms\n";
    for (const auto& row : latencyRows) {
        out << row.timestampMs << ',' << row.workspaceName << ','
            << row.detectionDelayMs << ',' << row.compileMs << ','
            << row.installToRenderMs << ',' << row.totalMs << '\n';
    }

    out << '\n';
    out << "recompile_timestamp_ms,workspace,recompile_ms\n";
    for (const auto& row : recompileRows) {
        out << row.timestampMs << ',' << row.workspaceName << ',' << row.parseToReadyMs << '\n';
    }

    return static_cast<bool>(out);
}

MetricsRegistry::Stats MetricsRegistry::ComputeStats(const std::vector<double>& samples) {
    Stats stats{};
    if (samples.empty()) {
        return stats;
    }
    stats.count = samples.size();
    stats.last = samples.back();

    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    stats.min = sorted.front();
    stats.max = sorted.back();
    stats.mean = std::accumulate(sorted.begin(), sorted.end(), 0.0) /
                 static_cast<double>(sorted.size());

    auto pickPercentile = [&](double p) {
        if (sorted.size() == 1) return sorted.front();
        const double idx = p * static_cast<double>(sorted.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(idx);
        const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
        const double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    };
    stats.p50 = pickPercentile(0.50);
    stats.p95 = pickPercentile(0.95);
    return stats;
}

std::vector<float> MetricsRegistry::ToFloatVector(const std::vector<double>& samples) {
    std::vector<float> out;
    out.reserve(samples.size());
    for (double v : samples) {
        out.push_back(static_cast<float>(v));
    }
    return out;
}
