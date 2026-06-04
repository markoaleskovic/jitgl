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

    void TrimDeque(std::deque<double>& d) {
        while (d.size() > MetricsRegistry::kMaxSamples) {
            d.pop_front();
        }
    }

    void TrimSamples(std::deque<MetricsRegistry::LatencySample>& d) {
        while (d.size() > MetricsRegistry::kMaxSamples) {
            d.pop_front();
        }
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

void MetricsRegistry::RecordRecompile(double parseToReadyMs) {
    if (parseToReadyMs < 0.0) {
        return;
    }
    std::scoped_lock lock(mu_);
    recompileSamplesMs_.push_back(parseToReadyMs);
    TrimDeque(recompileSamplesMs_);
}

MetricsRegistry::Stats MetricsRegistry::GetRecompileStats() const {
    std::scoped_lock lock(mu_);
    return ComputeStats(recompileSamplesMs_);
}

std::vector<float> MetricsRegistry::GetRecompileHistory() const {
    std::scoped_lock lock(mu_);
    return SnapshotHistory(recompileSamplesMs_);
}

void MetricsRegistry::RecordLatencySample(const LatencySample& sample) {
    std::scoped_lock lock(mu_);
    latencySamples_.push_back(sample);
    latencyTotalMs_.push_back(sample.totalMs);
    latencyDetectionMs_.push_back(sample.detectionDelayMs);
    latencyCompileMs_.push_back(sample.compileMs);
    latencyInstallMs_.push_back(sample.installToRenderMs);
    TrimSamples(latencySamples_);
    TrimDeque(latencyTotalMs_);
    TrimDeque(latencyDetectionMs_);
    TrimDeque(latencyCompileMs_);
    TrimDeque(latencyInstallMs_);
}

MetricsRegistry::Stats MetricsRegistry::GetLatencyStats() const {
    std::scoped_lock lock(mu_);
    return ComputeStats(latencyTotalMs_);
}

MetricsRegistry::Stats MetricsRegistry::GetDetectionDelayStats() const {
    std::scoped_lock lock(mu_);
    return ComputeStats(latencyDetectionMs_);
}

MetricsRegistry::Stats MetricsRegistry::GetCompileBreakdownStats() const {
    std::scoped_lock lock(mu_);
    return ComputeStats(latencyCompileMs_);
}

MetricsRegistry::Stats MetricsRegistry::GetInstallToRenderStats() const {
    std::scoped_lock lock(mu_);
    return ComputeStats(latencyInstallMs_);
}

std::vector<float> MetricsRegistry::GetLatencyHistory() const {
    std::scoped_lock lock(mu_);
    return SnapshotHistory(latencyTotalMs_);
}

std::vector<MetricsRegistry::LatencySample>
MetricsRegistry::GetRecentLatencySamples(std::size_t maxSamples) const {
    std::scoped_lock lock(mu_);
    std::vector<LatencySample> out;
    const std::size_t count = std::min(maxSamples, latencySamples_.size());
    out.reserve(count);
    auto it = latencySamples_.end();
    for (std::size_t i = 0; i < count; ++i) {
        --it;
        out.push_back(*it);
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
    recompileSamplesMs_.clear();
    latencySamples_.clear();
    latencyTotalMs_.clear();
    latencyDetectionMs_.clear();
    latencyCompileMs_.clear();
    latencyInstallMs_.clear();
    memorySnapshot_ = MemorySnapshot{};
    lastMemoryRefreshSeconds_ = -1.0;
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
    std::vector<double> recompileRows;
    MemorySnapshot memory;
    bool coldStartValid = false;
    double coldStart = 0.0;

    {
        std::scoped_lock lock(mu_);
        recompile = ComputeStats(recompileSamplesMs_);
        latency = ComputeStats(latencyTotalMs_);
        detection = ComputeStats(latencyDetectionMs_);
        compile = ComputeStats(latencyCompileMs_);
        install = ComputeStats(latencyInstallMs_);
        latencyRows.assign(latencySamples_.begin(), latencySamples_.end());
        recompileRows.assign(recompileSamplesMs_.begin(), recompileSamplesMs_.end());
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
    out << "recompile_index,recompile_ms\n";
    for (std::size_t i = 0; i < recompileRows.size(); ++i) {
        out << i << ',' << recompileRows[i] << '\n';
    }

    return static_cast<bool>(out);
}

MetricsRegistry::Stats MetricsRegistry::ComputeStats(const std::deque<double>& samples) {
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

std::vector<float> MetricsRegistry::SnapshotHistory(const std::deque<double>& samples) {
    std::vector<float> out;
    out.reserve(samples.size());
    for (double v : samples) {
        out.push_back(static_cast<float>(v));
    }
    return out;
}
