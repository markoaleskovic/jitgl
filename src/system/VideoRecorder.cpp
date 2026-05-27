#include "system/VideoRecorder.h"

#include <glad/gl.h>

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
// Recording requires fork/exec/pipe semantics. Windows would need a
// CreateProcess + named-anonymous-pipe variant; punt for now.
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

VideoRecorder::VideoRecorder() = default;

VideoRecorder::~VideoRecorder() {
    if (active_.load()) {
        Stop();
    }
}

#if defined(_WIN32)

bool VideoRecorder::Start(const Config& /*config*/, std::string* errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = "Video recording is not yet implemented on Windows.";
    }
    return false;
}

void VideoRecorder::CaptureFrame(unsigned int, int, int, double) {}

void VideoRecorder::Stop() {}

VideoRecorder::Status VideoRecorder::SnapshotStatus() { return Status{}; }

void VideoRecorder::WriterThreadMain() {}
void VideoRecorder::TeardownFfmpegLocked() {}

#else

bool VideoRecorder::Start(const Config& config, std::string* errorMessage) {
    const auto fail = [&](const std::string& msg) -> bool {
        if (errorMessage != nullptr) {
            *errorMessage = msg;
        }
        return false;
    };

    if (active_.load()) {
        return fail("Already recording.");
    }
    if (config.outputPath.empty()) {
        return fail("Output path is empty.");
    }
    if (config.width <= 0 || config.height <= 0) {
        return fail("Recording width/height must be positive.");
    }
    if (config.fps <= 0) {
        return fail("Recording fps must be positive.");
    }

    // Build the ffmpeg argv. We invoke it as "ffmpeg ..." and rely on
    // execvp's PATH search.
    const std::string sizeArg =
        std::to_string(config.width) + "x" + std::to_string(config.height);
    const std::string fpsArg = std::to_string(config.fps);
    const std::string crfArg = std::to_string(config.crf);

    std::vector<std::string> args = {
        "ffmpeg",
        "-y",                            // overwrite output if present
        "-loglevel", "error",            // keep stderr noise low
        "-f", "rawvideo",
        "-pix_fmt", "rgba",
        "-s", sizeArg,
        "-framerate", fpsArg,
        "-i", "-",                       // stdin
        // OpenGL framebuffers are bottom-up; flip on the way into the encoder.
        "-vf", "vflip",
        "-c:v", "libx264",
        "-pix_fmt", "yuv420p",
        "-preset", "veryfast",
        "-crf", crfArg,
        config.outputPath,
    };

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    int stdinPipe[2] = { -1, -1 };
    int stderrPipe[2] = { -1, -1 };
    if (pipe(stdinPipe) != 0) {
        return fail(std::string("pipe(stdin) failed: ") + std::strerror(errno));
    }
    if (pipe(stderrPipe) != 0) {
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        return fail(std::string("pipe(stderr) failed: ") + std::strerror(errno));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        return fail(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // Child: rewire stdin/stderr to our pipes, then exec ffmpeg.
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        // SIGPIPE on the child would already be ignored as a parent-process
        // matter; just exec.
        execvp("ffmpeg", argv.data());
        // Reserved fallback code: parent treats 127 as "ffmpeg unavailable".
        _exit(127);
    }

    ::close(stdinPipe[0]);
    ::close(stderrPipe[1]);

    // Don't let SIGPIPE kill the host process if ffmpeg dies mid-recording.
    // (Once set, it stays set; calling it repeatedly is harmless.)
    signal(SIGPIPE, SIG_IGN);

    // Make stderr nonblocking so Stop() can drain whatever's available
    // without hanging on EOF.
    const int flags = fcntl(stderrPipe[0], F_GETFL, 0);
    if (flags != -1) {
        fcntl(stderrPipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    ffmpegStdinFd_ = stdinPipe[1];
    ffmpegStderrFd_ = stderrPipe[0];
    ffmpegPid_ = pid;

    config_ = config;
    framesCaptured_ = 0;
    framesDropped_ = 0;
    stopRequested_ = false;
    elapsedSecondsAtom_.store(0.0);
    startTimeSeconds_ = 0.0;
    nextCaptureTimeSeconds_ = 0.0;
    frameIntervalSeconds_ = 1.0 / static_cast<double>(config.fps);

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        lastError_.clear();
        finishedFlag_ = false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        readyFrames_.clear();
        freeFrames_.clear();
        // Pre-allocate the buffer pool so steady-state recording never
        // allocates on the GL thread.
        const std::size_t bytesPerFrame =
            static_cast<std::size_t>(config.width) *
            static_cast<std::size_t>(config.height) * 4u;
        for (std::size_t i = 0; i < maxQueueFrames_; ++i) {
            freeFrames_.emplace_back(bytesPerFrame, 0u);
        }
    }

    active_ = true;
    writerThread_ = std::thread(&VideoRecorder::WriterThreadMain, this);
    return true;
}

void VideoRecorder::CaptureFrame(unsigned int sourceTexture,
                                 int sourceWidth,
                                 int sourceHeight,
                                 double nowSeconds) {
    if (!active_.load() || stopRequested_.load()) {
        return;
    }
    if (sourceTexture == 0) {
        return;
    }
    // Lock to the resolution we negotiated with ffmpeg. If the renderer
    // panel was resized mid-recording the frame can't be encoded into the
    // existing stream.
    if (sourceWidth != config_.width || sourceHeight != config_.height) {
        framesDropped_.fetch_add(1);
        return;
    }

    if (startTimeSeconds_ == 0.0) {
        startTimeSeconds_ = nowSeconds;
        nextCaptureTimeSeconds_ = nowSeconds;
    }
    elapsedSecondsAtom_.store(nowSeconds - startTimeSeconds_);

    // Skip frames if the engine is rendering faster than the requested
    // recording fps. This is what keeps the playback speed correct when
    // recording 60fps from a 144fps engine.
    if (nowSeconds + 1e-6 < nextCaptureTimeSeconds_) {
        return;
    }

    std::vector<unsigned char> buffer;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (freeFrames_.empty()) {
            // Writer thread is behind. Drop this frame rather than blocking
            // the renderer; ffmpeg backpressure usually clears in a few frames.
            framesDropped_.fetch_add(1);
            return;
        }
        buffer = std::move(freeFrames_.back());
        freeFrames_.pop_back();
    }

    const std::size_t bytesPerFrame =
        static_cast<std::size_t>(config_.width) *
        static_cast<std::size_t>(config_.height) * 4u;
    if (buffer.size() != bytesPerFrame) {
        buffer.assign(bytesPerFrame, 0u);
    }

    GLint prevPackAlign = 4;
    GLint prevTex = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlign);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, sourceTexture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlign);

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        readyFrames_.push_back(std::move(buffer));
    }
    queueCv_.notify_one();
    framesCaptured_.fetch_add(1);

    // Schedule the next capture at the configured fps tempo. If the engine
    // is currently slower than fps this catches up to "now" so we don't
    // accumulate a debt that produces a long fast-forward later.
    nextCaptureTimeSeconds_ += frameIntervalSeconds_;
    if (nextCaptureTimeSeconds_ < nowSeconds) {
        nextCaptureTimeSeconds_ = nowSeconds + frameIntervalSeconds_;
    }
}

void VideoRecorder::WriterThreadMain() {
    while (true) {
        std::vector<unsigned char> buffer;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [&]() {
                return !readyFrames_.empty() || stopRequested_.load();
            });
            if (readyFrames_.empty()) {
                // Stop requested and queue drained.
                break;
            }
            buffer = std::move(readyFrames_.front());
            readyFrames_.pop_front();
        }

        const ssize_t total = static_cast<ssize_t>(buffer.size());
        ssize_t written = 0;
        while (written < total) {
            const ssize_t n = ::write(ffmpegStdinFd_,
                                      buffer.data() + written,
                                      static_cast<std::size_t>(total - written));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    if (lastError_.empty()) {
                        lastError_ =
                            std::string("ffmpeg pipe write failed: ") + std::strerror(errno);
                    }
                }
                stopRequested_.store(true);
                queueCv_.notify_all();
                break;
            }
            written += n;
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            freeFrames_.push_back(std::move(buffer));
        }
    }
}

void VideoRecorder::Stop() {
    if (!active_.load()) {
        return;
    }
    stopRequested_.store(true);
    queueCv_.notify_all();
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    TeardownFfmpegLocked();
    active_.store(false);
    std::lock_guard<std::mutex> lock(statusMutex_);
    finishedFlag_ = true;
}

void VideoRecorder::TeardownFfmpegLocked() {
    if (ffmpegStdinFd_ >= 0) {
        // Closing stdin is the signal for ffmpeg to flush and finalize.
        ::close(ffmpegStdinFd_);
        ffmpegStdinFd_ = -1;
    }

    std::string stderrTail;
    if (ffmpegStderrFd_ >= 0) {
        char buf[4096];
        while (true) {
            const ssize_t n = ::read(ffmpegStderrFd_, buf, sizeof(buf));
            if (n > 0) {
                stderrTail.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            // EAGAIN / EWOULDBLOCK or any other transient: done draining.
            break;
        }
        ::close(ffmpegStderrFd_);
        ffmpegStderrFd_ = -1;
    }

    if (ffmpegPid_ > 0) {
        int status = 0;
        // Brief wait for graceful exit, then escalate. ffmpeg usually
        // finalizes a few-second clip in well under a second after EOF.
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pid_t r = waitpid(ffmpegPid_, &status, WNOHANG);
            if (r == ffmpegPid_) {
                ffmpegPid_ = -1;
                break;
            }
            if (r < 0) {
                ffmpegPid_ = -1;
                break;
            }
            usleep(100 * 1000);  // 100ms
        }
        if (ffmpegPid_ > 0) {
            kill(ffmpegPid_, SIGTERM);
            waitpid(ffmpegPid_, &status, 0);
            ffmpegPid_ = -1;
        }
        if (WIFEXITED(status)) {
            const int code = WEXITSTATUS(status);
            if (code == 127) {
                std::lock_guard<std::mutex> lock(statusMutex_);
                if (lastError_.empty()) {
                    lastError_ = "ffmpeg not found in PATH.";
                }
            } else if (code != 0) {
                std::lock_guard<std::mutex> lock(statusMutex_);
                if (lastError_.empty()) {
                    std::string trimmed = stderrTail;
                    if (trimmed.size() > 2048) {
                        trimmed.erase(0, trimmed.size() - 2048);
                    }
                    lastError_ = "ffmpeg exited with status " + std::to_string(code);
                    if (!trimmed.empty()) {
                        lastError_ += ":\n" + trimmed;
                    }
                }
            }
        }
    }
}

VideoRecorder::Status VideoRecorder::SnapshotStatus() {
    Status s;
    s.active = active_.load();
    s.framesCaptured = framesCaptured_.load();
    s.framesDropped = framesDropped_.load();
    s.width = config_.width;
    s.height = config_.height;
    s.fps = config_.fps;
    s.durationSeconds = config_.durationSeconds;
    s.elapsedSeconds = elapsedSecondsAtom_.load();
    s.outputPath = config_.outputPath;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        s.lastError = lastError_;
        s.finishedSinceLastQuery = finishedFlag_;
        finishedFlag_ = false;
    }
    return s;
}

#endif
