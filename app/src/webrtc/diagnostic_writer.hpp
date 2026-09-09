#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace opennow::webrtc::diagnostics
{

enum class DiagnosticFile
{
    Input,
    Signaling,
    Trace,
};

struct DiagnosticWriterLimits
{
    size_t max_queued_bytes = 256 * 1024;
    size_t max_queued_records = 512;
};

class DiagnosticWriter
{
public:
    explicit DiagnosticWriter(
        std::string directory,
        DiagnosticWriterLimits limits = {});
    ~DiagnosticWriter();

    DiagnosticWriter(const DiagnosticWriter&) = delete;
    DiagnosticWriter& operator=(const DiagnosticWriter&) = delete;

    bool start();
    void stop();

    bool try_append(DiagnosticFile file, std::string text);
    bool try_append_stream(std::string signaling_text, std::string trace_text);
    bool try_append_trace_block(std::string text);
    bool reset_stream_logs();

    uint64_t dropped_record_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace opennow::webrtc::diagnostics
