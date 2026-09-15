#pragma once

#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace loader_detail
{
    // Joins every successfully launched thread when a loading phase leaves its scope.
    class ScopedThreads
    {
      public:
        explicit ScopedThreads(size_t capacity) { m_threads.reserve(capacity); }

        ~ScopedThreads()
        {
            for (auto &thread : m_threads)
            {
                thread.join();
            }
        }

        ScopedThreads(const ScopedThreads &) = delete;
        ScopedThreads &operator=(const ScopedThreads &) = delete;
        ScopedThreads(ScopedThreads &&) = delete;
        ScopedThreads &operator=(ScopedThreads &&) = delete;

        template <class Function, class... Args> void launch(Function &&function, Args &&...args)
        {
            m_threads.emplace_back(std::forward<Function>(function), std::forward<Args>(args)...);
        }

      private:
        std::vector<std::thread> m_threads;
    };
} // namespace loader_detail
