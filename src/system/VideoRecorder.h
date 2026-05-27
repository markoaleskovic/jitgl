#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// VideoRecorder pipes rendered frames to a background ffmpeg process to
// produce an mp4 (or whatever container the user picks). The intended use
// is "click record, get a shareable artifact" — the implementation is
// deliberately simple: synchronous glGetTexImage on the captured texture,
// a bounded ring of frame buffers, and a writer thread that copes with
// ffmpeg backpressure so the GL thread is never blocked on the pipe.
class VideoRecorder {
public:
    struct Config {
        std::string outputPath;
        int width = 0;
        int height = 0;
        int fps = 60;
        // 0 = record until Stop() is called manually.
        double durationSeconds = 0.0;
        // libx264 CRF; 18 is visually near-lossless, 23 is the libx264 default.
        int crf = 18;
    };

    struct Status {
        bool active = false;
        int framesCaptured = 0;
        int framesDropped = 0;
        int width = 0;
        int height = 0;
        int fps = 60;
        double durationSeconds = 0.0;
        // Wall-clock since Start(), set by the engine via the most recent
        // CaptureFrame timestamp. The engine drives time so the recorder
        // doesn't need its own clock.
        double elapsedSeconds = 0.0;
        std::string outputPath;
        // Empty unless ffmpeg refused to start or died during recording.
        std::string lastError;
        // Set to true exactly once after Stop() completes (or after an
        // error tears the session down). Cleared after the first read so
        // the UI can latch "show toast" without re-firing every frame.
        bool finishedSinceLastQuery = false;
    };

    VideoRecorder();
    ~VideoRecorder();

    bool Start(const Config& config, std::string* errorMessage);
    bool IsActive() const { return active_.load(); }

    // Pull the given GL_TEXTURE_2D color texture into a frame buffer and
    // hand it to the writer thread. `nowSeconds` is the wall clock used
    // for elapsed-time reporting and auto-stop. If the texture dimensions
    // don't match the configured (width, height) the frame is dropped.
    void CaptureFrame(unsigned int sourceTexture,
                      int sourceWidth,
                      int sourceHeight,
                      double nowSeconds);

    // Block until the writer thread drains the remaining frames and ffmpeg
    // exits. Safe to call multiple times.
    void Stop();

    Status SnapshotStatus();

private:
    void WriterThreadMain();
    void TeardownFfmpegLocked();

    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<int> framesCaptured_{0};
    std::atomic<int> framesDropped_{0};

    Config config_{};
    double startTimeSeconds_ = 0.0;
    double nextCaptureTimeSeconds_ = 0.0;
    double frameIntervalSeconds_ = 1.0 / 60.0;
    std::atomic<double> elapsedSecondsAtom_{0.0};

#if defined(_WIN32)
    // Stub: Start() returns an error on Windows so the rest of the build
    // remains functional. Real implementation would use CreateProcess +
    // anonymous pipes.
#else
    int ffmpegStdinFd_ = -1;
    int ffmpegStderrFd_ = -1;
    int ffmpegPid_ = -1;
#endif

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<std::vector<unsigned char>> readyFrames_;
    std::vector<std::vector<unsigned char>> freeFrames_;
    std::size_t maxQueueFrames_ = 4;

    std::thread writerThread_;

    mutable std::mutex statusMutex_;
    std::string lastError_;
    bool finishedFlag_ = false;
};
