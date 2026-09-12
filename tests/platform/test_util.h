#pragma once

#include "tests/test.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace platform_test
{
    struct ScopedFd
    {
        int fd;
        explicit ScopedFd(int f) : fd(f) {}
        ~ScopedFd()
        {
            if (fd >= 0)
            {
                test_close(fd);
            }
        }
        ScopedFd(const ScopedFd &) = delete;
        ScopedFd &operator=(const ScopedFd &) = delete;
        ScopedFd(ScopedFd &&) = delete;
        ScopedFd &operator=(ScopedFd &&) = delete;
    };

    struct ScopedStdoutCapture
    {
        std::FILE *capture = nullptr;
        int saved = -1;
        bool valid = false;

        ScopedStdoutCapture() : capture(std::tmpfile()), saved(test_dup(TEST_STDOUT))
        {
            std::fflush(stdout);
            if (capture != nullptr && saved >= 0)
            {
                valid = test_dup2(test_fileno(capture), TEST_STDOUT) >= 0;
            }
        }

        ~ScopedStdoutCapture()
        {
            std::fflush(stdout);
            if (saved >= 0)
            {
                test_dup2(saved, TEST_STDOUT);
                test_close(saved);
            }
            if (capture != nullptr)
            {
                std::fclose(capture);
            }
        }

        [[nodiscard]] std::string read() const
        {
            std::fflush(stdout);
            if (capture == nullptr || std::fseek(capture, 0, SEEK_SET) != 0)
            {
                return {};
            }
            std::string text;
            char buffer[512];
            for (;;)
            {
                const size_t count = std::fread(buffer, 1, sizeof buffer, capture);
                text.append(buffer, count);
                if (count < sizeof buffer)
                {
                    break;
                }
            }
            return text;
        }

        ScopedStdoutCapture(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture &operator=(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture(ScopedStdoutCapture &&) = delete;
        ScopedStdoutCapture &operator=(ScopedStdoutCapture &&) = delete;
    };

    struct ScopedTmpFile
    {
        std::string path;
        ScopedTmpFile(const char *name, const void *data, size_t n)
            : path((std::filesystem::temp_directory_path() / name).string())
        {
            // A constructor failure must remove the partial file itself.
            try
            {
                std::FILE *f = std::fopen(path.c_str(), "wb");
                ASSERT_TRUE(f != nullptr);
                const size_t written = std::fwrite(data, 1, n, f);
                // fclose can report deferred write failures such as ENOSPC.
                const int closed = std::fclose(f);
                ASSERT_EQ(written, n);
                ASSERT_EQ(closed, 0);
            }
            catch (...)
            {
                std::remove(path.c_str());
                throw;
            }
        }
        ~ScopedTmpFile() { std::remove(path.c_str()); }
        ScopedTmpFile(const ScopedTmpFile &) = delete;
        ScopedTmpFile &operator=(const ScopedTmpFile &) = delete;
        ScopedTmpFile(ScopedTmpFile &&) = delete;
        ScopedTmpFile &operator=(ScopedTmpFile &&) = delete;
    };

} // namespace platform_test
