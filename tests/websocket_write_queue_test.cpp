#include "websocket_write_queue.hpp"

#include <cassert>
#include <deque>

using Queue = opennow::websocket::WriteQueue;
using Status = Queue::WriteStatus;
using Result = Queue::DrainResult;

struct FakeWriter
{
    std::deque<Queue::WriteResult> results;
    std::vector<uint8_t> output;
    size_t calls = 0;
    const uint8_t* blocked_data = nullptr;
    size_t blocked_length = 0;

    Queue::WriteResult operator()(const uint8_t* data, size_t length)
    {
        ++calls;
        assert(length <= Queue::MaximumWriteBytes);
        if (blocked_data) {
            assert(data == blocked_data);
            assert(length == blocked_length);
        }
        Queue::WriteResult result{Status::Progress, length};
        if (!results.empty()) {
            result = results.front();
            results.pop_front();
        }
        blocked_data = result.status == Status::Again ? data : nullptr;
        blocked_length = length;
        if (result.status == Status::Progress && result.bytes <= length)
            output.insert(output.end(), data, data + result.bytes);
        return result;
    }
};

int main()
{
    Queue::Clock::time_point time{};
    const auto now = [&] { return time; };

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1, 2, 3, 4}, time));
        assert(queue.enqueue({5, 6}, time));
        writer.results = {{Status::Progress, 2}, {Status::Again, 0}};
        assert(queue.drain(writer, now) == Result::Pending);
        assert(writer.calls == 2);
        assert((writer.output == std::vector<uint8_t>{1, 2}));
        writer.results = {{Status::Again, 0}};
        assert(queue.drain(writer, now) == Result::Pending);
        assert(writer.calls == 3);
        assert(queue.enqueue({7, 8}, time));
        assert(queue.drain(writer, now) == Result::Complete);
        assert((writer.output == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}));
    }

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue(std::vector<uint8_t>(Queue::MaximumBytes, 9), time));
        assert(!queue.can_enqueue(1));
        assert(!queue.enqueue({1}, time));
        assert(queue.drain(writer, now) == Result::Pending);
        assert(writer.calls == Queue::MaximumWritesPerPoll);
        assert(writer.output.size() == Queue::MaximumWritesPerPoll * Queue::MaximumWriteBytes);
        assert(!queue.can_enqueue(1));
        const auto started = time;
        Result result;
        do {
            time += std::chrono::milliseconds(17);
            result = queue.drain(writer, now);
        } while (result == Result::Pending);
        assert(result == Result::Complete);
        assert(time - started < Queue::SendTimeout);
        assert(writer.output == std::vector<uint8_t>(Queue::MaximumBytes, 9));
        assert(queue.can_enqueue(Queue::MaximumBytes));
        assert(!queue.can_enqueue(Queue::MaximumBytes + 1));
    }

    {
        Queue queue;
        FakeWriter writer;
        for (size_t i = 0; i < Queue::MaximumFrames; ++i)
            assert(queue.enqueue({static_cast<uint8_t>(i)}, time));
        assert(!queue.enqueue({65}, time));
        assert(queue.drain(writer, now) == Result::Pending);
        assert(writer.calls == Queue::MaximumWritesPerPoll);
        assert(queue.can_enqueue(1));
        assert(!queue.can_enqueue(0));
        assert(!queue.enqueue({}, time));
    }

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1, 2, 3}, time));
        writer.results = {{Status::Progress, 1}, {Status::Again, 0}};
        assert(queue.drain(writer, now) == Result::Pending);
        time += std::chrono::milliseconds(999);
        writer.results = {{Status::Progress, 1}, {Status::Again, 0}};
        assert(queue.drain(writer, now) == Result::Pending);
        time += std::chrono::milliseconds(1);
        const auto calls = writer.calls;
        assert(queue.drain(writer, now) == Result::TimedOut);
        assert(writer.calls == calls);
        assert((writer.output == std::vector<uint8_t>{1, 2}));
        queue.reset();
        writer = {};
        assert(queue.enqueue({4, 5}, time));
        assert(queue.drain(writer, now) == Result::Complete);
        assert((writer.output == std::vector<uint8_t>{4, 5}));
    }

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1}, time));
        assert(queue.enqueue({2}, time));
        time += Queue::SendTimeout - std::chrono::milliseconds(1);
        auto advancing_writer = [&](const uint8_t* data, size_t length) {
            const auto result = writer(data, length);
            time += std::chrono::milliseconds(1);
            return result;
        };
        assert(queue.drain(advancing_writer, now) == Result::TimedOut);
        assert(writer.calls == 1);
        assert((writer.output == std::vector<uint8_t>{1}));
    }

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1}, time));
        time += Queue::SendTimeout;
        assert(queue.drain(writer, now) == Result::TimedOut);
        assert(writer.calls == 0);
    }

    for (const auto status : {Status::Error, Status::Progress}) {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1, 2}, time));
        assert(queue.enqueue({3}, time));
        writer.results = {{status, 0}};
        assert(queue.drain(writer, now) ==
               (status == Status::Error ? Result::WriteError : Result::Stalled));
        assert(writer.calls == 1);
        assert(writer.output.empty());
        queue.reset();
        assert(queue.can_enqueue(Queue::MaximumBytes));
        assert(queue.drain(writer, now) == Result::Complete);
        assert(writer.calls == 1);
    }

    {
        Queue queue;
        FakeWriter writer;
        assert(queue.enqueue({1}, time));
        writer.results = {{Status::Progress, 2}};
        assert(queue.drain(writer, now) == Result::WriteError);
    }
}
