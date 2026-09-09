#include "webrtc/diagnostic_writer.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace
{

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

template <typename Predicate>
void WaitUntil(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

int main()
{
    using opennow::webrtc::diagnostics::DiagnosticFile;
    using opennow::webrtc::diagnostics::DiagnosticWriter;
    using opennow::webrtc::diagnostics::DiagnosticWriterLimits;

    std::string directory = (std::filesystem::temp_directory_path() /
                             "opennow-diagnostic-writer-XXXXXX").string();
    assert(mkdtemp(directory.data()));
    const std::filesystem::path root(directory);

    const auto count_directory = root / "count-overflow";
    std::filesystem::create_directories(count_directory);
    DiagnosticWriter count_writer(
        count_directory.string(), DiagnosticWriterLimits {100, 2});
    assert(count_writer.try_append(DiagnosticFile::Input, "first\n"));
    assert(count_writer.try_append(DiagnosticFile::Input, "second\n"));
    assert(!count_writer.try_append(DiagnosticFile::Input, "dropped\n"));
    assert(count_writer.dropped_record_count() == 1);
    assert(count_writer.start());
    count_writer.stop();
    assert(ReadFile(count_directory / "input.log") == "first\nsecond\n");
    assert(!count_writer.try_append(DiagnosticFile::Input, "after stop\n"));

    const auto byte_directory = root / "byte-overflow";
    std::filesystem::create_directories(byte_directory);
    DiagnosticWriter byte_writer(
        byte_directory.string(), DiagnosticWriterLimits {5, 10});
    assert(!byte_writer.try_append_trace_block("123456"));
    assert(byte_writer.try_append(DiagnosticFile::Trace, "12345"));
    assert(byte_writer.start());
    byte_writer.stop();
    assert(ReadFile(byte_directory / "stream_trace.log") == "12345");

    const auto reset_directory = root / "reset-order";
    std::filesystem::create_directories(reset_directory);
    WriteFile(reset_directory / "input.log", "old input\n");
    WriteFile(reset_directory / "signaling.log", "old signaling\n");
    WriteFile(reset_directory / "stream_trace.log", "old trace\n");
    DiagnosticWriter reset_writer(reset_directory.string());
    assert(reset_writer.try_append(DiagnosticFile::Input, "before input\n"));
    assert(reset_writer.try_append_stream("before signaling\n", "before trace\n"));
    assert(reset_writer.reset_stream_logs());
    assert(reset_writer.dropped_record_count() == 2);
    assert(reset_writer.try_append_stream("after signaling\n", "after trace\n"));
    assert(reset_writer.try_append(DiagnosticFile::Input, "after input\n"));
    assert(reset_writer.try_append_trace_block("BEGIN\nbody\nEND\n"));
    assert(reset_writer.start());
    reset_writer.stop();

    const std::string input = ReadFile(reset_directory / "input.log");
    const std::string signaling = ReadFile(reset_directory / "signaling.log");
    const std::string trace = ReadFile(reset_directory / "stream_trace.log");
    assert(input.starts_with("SwitchNOW input flight recorder\n"));
    assert(input.ends_with("after input\n"));
    assert(input.find("old input") == std::string::npos);
    assert(input.find("before input") == std::string::npos);
    assert(signaling.starts_with("SwitchNOW signaling and media log\n"));
    assert(signaling.ends_with("after signaling\n"));
    assert(signaling.find("old signaling") == std::string::npos);
    assert(signaling.find("before signaling") == std::string::npos);
    assert(trace.starts_with("SwitchNOW stream trace\n"));
    assert(trace.ends_with("after trace\nBEGIN\nbody\nEND\n"));
    assert(trace.find("old trace") == std::string::npos);
    assert(trace.find("before trace") == std::string::npos);

    const auto lifecycle_directory = root / "lifecycle";
    std::filesystem::create_directories(lifecycle_directory);
    {
        DiagnosticWriter writer(lifecycle_directory.string());
        assert(writer.try_append(DiagnosticFile::Signaling, "drained by destructor\n"));
        assert(writer.start());
    }
    assert(ReadFile(lifecycle_directory / "signaling.log") ==
           "drained by destructor\n");

    const auto live_directory = root / "live-output";
    std::filesystem::create_directories(live_directory);
    DiagnosticWriter live_writer(live_directory.string());
    assert(live_writer.try_append(DiagnosticFile::Input, "visible before shutdown\n"));
    assert(live_writer.start());
    WaitUntil([&] {
        return ReadFile(live_directory / "input.log") == "visible before shutdown\n";
    });
    assert(live_writer.reset_stream_logs());
    WaitUntil([&] {
        return ReadFile(live_directory / "input.log").starts_with("SwitchNOW input flight recorder\n");
    });
    WaitUntil([&] {
        return live_writer.try_append(DiagnosticFile::Input, "after live reset\n");
    });
    WaitUntil([&] {
        const auto text = ReadFile(live_directory / "input.log");
        return text.ends_with("after live reset\n") &&
               text.find("visible before shutdown") == std::string::npos;
    });
    live_writer.stop();

    std::filesystem::remove_all(root);
    return 0;
}
