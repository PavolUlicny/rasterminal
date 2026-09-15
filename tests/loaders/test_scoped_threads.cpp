#include "src/loaders/scoped_threads.h"
#include "tests/test.h"

#include <atomic>
#include <stdexcept>

TEST(scoped_threads, joins_started_workers_during_unwinding)
{
    std::atomic<bool> finished{ false };
    bool caught = false;

    try
    {
        loader_detail::ScopedThreads threads(2);
        threads.launch([&finished] { finished.store(true, std::memory_order_release); });
        // Model a later std::thread constructor failing after this worker became joinable.
        throw std::runtime_error("launch failed");
    }
    catch (const std::runtime_error &)
    {
        caught = true;
    }

    ASSERT_TRUE(caught);
    ASSERT_TRUE(finished.load(std::memory_order_acquire));
}
