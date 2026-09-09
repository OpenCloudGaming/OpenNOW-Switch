#include "diagnostic_writer.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace opennow::webrtc::diagnostics
{
namespace
{

constexpr const char* kInputHeader =
    "SwitchNOW input flight recorder\n"
    "Controller samples -> Xbox encoding -> DataChannel -> SCTP result.\n"
    "===============================================================\n";
constexpr const char* kSignalingHeader =
    "SwitchNOW signaling and media log\n"
    "One file per stream attempt.\n"
    "======================================\n";
constexpr const char* kTraceHeader =
    "SwitchNOW stream trace\n"
    "One file per stream attempt. Redact session data before sharing.\n"
    "=======================================================\n";

struct Write
{
    DiagnosticFile file;
    std::string text;
};

struct Record
{
    bool reset = false;
    size_t bytes = 0;
    std::vector<Write> writes;
};

std::string FilePath(const std::string& directory, const char* name)
{
    if (!directory.empty() && directory.back() == '/')
        return directory + name;
    return directory + "/" + name;
}

} // namespace

struct DiagnosticWriter::Impl
{
    Impl(std::string directory_value, DiagnosticWriterLimits limits_value)
        : directory(std::move(directory_value)), limits(limits_value)
    {
    }

    std::string directory;
    DiagnosticWriterLimits limits;
    std::mutex mutex;
    std::mutex stop_mutex;
    std::condition_variable cv;
    std::deque<Record> records;
    size_t queued_bytes = 0;
    bool accepting = true;
    bool started = false;
    bool stopping = false;
    std::thread worker;
    std::atomic<uint64_t> dropped_records {0};

    bool enqueue(Record record, bool ordered_reset)
    {
        std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
        if (ordered_reset) {
            lock.lock();
        } else if (!lock.try_lock()) {
            dropped_records.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (!accepting || limits.max_queued_records == 0 ||
            record.bytes > limits.max_queued_bytes) {
            dropped_records.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (ordered_reset) {
            dropped_records.fetch_add(records.size(), std::memory_order_relaxed);
            records.clear();
            queued_bytes = 0;
        } else if (records.size() >= limits.max_queued_records ||
                   record.bytes > limits.max_queued_bytes - queued_bytes) {
            dropped_records.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        try {
            records.push_back(std::move(record));
        } catch (...) {
            dropped_records.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        queued_bytes += records.back().bytes;
        lock.unlock();
        cv.notify_one();
        return true;
    }

    struct Files
    {
        explicit Files(const std::string& directory)
            : input_path(FilePath(directory, "input.log")),
              signaling_path(FilePath(directory, "signaling.log")),
              trace_path(FilePath(directory, "stream_trace.log"))
        {
        }

        std::string input_path;
        std::string signaling_path;
        std::string trace_path;
        std::ofstream input;
        std::ofstream signaling;
        std::ofstream trace;

        std::ofstream& stream(DiagnosticFile file)
        {
            switch (file) {
            case DiagnosticFile::Input:
                return input;
            case DiagnosticFile::Signaling:
                return signaling;
            case DiagnosticFile::Trace:
                return trace;
            }
            return trace;
        }

        const std::string& path(DiagnosticFile file) const
        {
            switch (file) {
            case DiagnosticFile::Input:
                return input_path;
            case DiagnosticFile::Signaling:
                return signaling_path;
            case DiagnosticFile::Trace:
                return trace_path;
            }
            return trace_path;
        }

        void close()
        {
            input.close();
            signaling.close();
            trace.close();
        }

        void write(DiagnosticFile file, const std::string& text)
        {
            std::ofstream& output = stream(file);
            if (!output.is_open())
                output.open(path(file), std::ios::binary | std::ios::app);
            if (!output.is_open())
                return;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output.good())
                output.close();
        }

        void reset()
        {
            close();
            input.open(input_path, std::ios::binary | std::ios::trunc);
            signaling.open(signaling_path, std::ios::binary | std::ios::trunc);
            trace.open(trace_path, std::ios::binary | std::ios::trunc);
            if (input.is_open())
                input << kInputHeader;
            if (signaling.is_open())
                signaling << kSignalingHeader;
            if (trace.is_open())
                trace << kTraceHeader;
            if (input.is_open())
                input.flush();
            if (signaling.is_open())
                signaling.flush();
            if (trace.is_open())
                trace.flush();
        }
    };

    void run()
    {
        Files files(directory);
        for (;;) {
            Record record;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this]() { return stopping || !records.empty(); });
                if (records.empty())
                    break;
                record = std::move(records.front());
                records.pop_front();
                queued_bytes -= record.bytes;
            }

            if (record.reset) {
                files.reset();
                continue;
            }
            for (const Write& write : record.writes)
                files.write(write.file, write.text);
        }
    }
};

DiagnosticWriter::DiagnosticWriter(
    std::string directory,
    DiagnosticWriterLimits limits)
    : impl_(std::make_unique<Impl>(std::move(directory), limits))
{
}

DiagnosticWriter::~DiagnosticWriter()
{
    stop();
}

bool DiagnosticWriter::start()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started || impl_->stopping || !impl_->accepting)
        return false;
    try {
        impl_->worker = std::thread(&Impl::run, impl_.get());
    } catch (...) {
        impl_->accepting = false;
        impl_->dropped_records.fetch_add(impl_->records.size(), std::memory_order_relaxed);
        impl_->records.clear();
        impl_->queued_bytes = 0;
        return false;
    }
    impl_->started = true;
    return true;
}

void DiagnosticWriter::stop()
{
    std::lock_guard<std::mutex> stop_lock(impl_->stop_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->accepting && !impl_->started)
            return;
        impl_->accepting = false;
        if (!impl_->started) {
            impl_->dropped_records.fetch_add(impl_->records.size(), std::memory_order_relaxed);
            impl_->records.clear();
            impl_->queued_bytes = 0;
            return;
        }
        impl_->stopping = true;
    }
    impl_->cv.notify_one();
    if (impl_->worker.joinable())
        impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->started = false;
}

bool DiagnosticWriter::try_append(DiagnosticFile file, std::string text)
{
    try {
        Record record;
        record.bytes = text.size();
        record.writes.push_back({file, std::move(text)});
        return impl_->enqueue(std::move(record), false);
    } catch (...) {
        impl_->dropped_records.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool DiagnosticWriter::try_append_stream(
    std::string signaling_text,
    std::string trace_text)
{
    if (signaling_text.size() > std::numeric_limits<size_t>::max() - trace_text.size()) {
        impl_->dropped_records.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    try {
        Record record;
        record.bytes = signaling_text.size() + trace_text.size();
        record.writes.push_back({DiagnosticFile::Signaling, std::move(signaling_text)});
        record.writes.push_back({DiagnosticFile::Trace, std::move(trace_text)});
        return impl_->enqueue(std::move(record), false);
    } catch (...) {
        impl_->dropped_records.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool DiagnosticWriter::try_append_trace_block(std::string text)
{
    return try_append(DiagnosticFile::Trace, std::move(text));
}

bool DiagnosticWriter::reset_stream_logs()
{
    Record record;
    record.reset = true;
    record.bytes = std::char_traits<char>::length(kInputHeader) +
                   std::char_traits<char>::length(kSignalingHeader) +
                   std::char_traits<char>::length(kTraceHeader);
    return impl_->enqueue(std::move(record), true);
}

uint64_t DiagnosticWriter::dropped_record_count() const
{
    return impl_->dropped_records.load(std::memory_order_relaxed);
}

} // namespace opennow::webrtc::diagnostics
