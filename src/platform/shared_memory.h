#pragma once

#include <cstddef>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Windows defines near and far as macros.
#undef near
#undef far
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace platform
{
#ifndef _WIN32
    // Linux writes to fd; other POSIX systems fill map. Copies do not duplicate handles.
    struct ShmFrame
    {
        int fd = -1;
        unsigned char *map = nullptr;
        size_t size = 0;
        size_t at = 0;

        [[nodiscard]] bool valid() const noexcept { return fd >= 0 || map != nullptr; }
    };

    inline ShmFrame shm_frame_open(const char *name, size_t size)
    {
        ShmFrame f;
        shm_unlink(name);
        const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
        {
            return f;
        }
        if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        {
            close(fd);
            shm_unlink(name);
            return f;
        }
        f.size = size;
#ifdef __linux__
        // Linux write() reports a full /dev/shm as ENOSPC instead of SIGBUS.
        f.fd = fd;
        return f;
#else
        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd); // the mapping keeps the object alive
        if (p == MAP_FAILED || p == nullptr)
        {
            shm_unlink(name);
            // Do not return a size without a valid sink.
            return {};
        }
        f.map = static_cast<unsigned char *>(p);
        return f;
#endif
    }

    // Return false when the caller must fall back to direct transport.
    inline bool shm_frame_append(ShmFrame &f, const unsigned char *data, size_t len)
    {
        if (len > f.size - f.at)
        {
            return false;
        }
        if (f.map != nullptr)
        {
            std::memcpy(f.map + f.at, data, len);
            f.at += len;
            return true;
        }
        while (len > 0)
        {
            const ssize_t w = write(f.fd, data, len);
            if (w <= 0)
            {
                if (w < 0 && errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            len -= static_cast<size_t>(w);
            data += static_cast<size_t>(w);
            f.at += static_cast<size_t>(w);
        }
        return true;
    }

    inline void shm_frame_close(ShmFrame &f)
    {
        if (f.map != nullptr)
        {
            munmap(f.map, f.size);
            f.map = nullptr;
        }
        if (f.fd >= 0)
        {
            close(f.fd);
            f.fd = -1;
        }
    }

    inline void shm_frame_remove(const char *name)
    {
        shm_unlink(name);
    }

    inline unsigned long process_id()
    {
        // Avoid implementation-defined DWORD narrowing on LLP64.
        return static_cast<unsigned long>(getpid());
    }
#else
    // Windows uses kitty direct mode.
    struct ShmFrame
    {
        [[nodiscard]] bool valid() const noexcept { return false; }
    };

    inline ShmFrame shm_frame_open(const char * /*name*/, size_t /*size*/)
    {
        return {};
    }

    inline bool shm_frame_append(ShmFrame & /*f*/, const unsigned char * /*data*/, size_t /*len*/)
    {
        return false;
    }

    inline void shm_frame_close(ShmFrame & /*f*/) {}

    inline void shm_frame_remove(const char * /*name*/) {}

    inline unsigned long process_id()
    {
        return GetCurrentProcessId();
    }
#endif

} // namespace platform
