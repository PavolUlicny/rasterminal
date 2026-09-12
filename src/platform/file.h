#pragma once

#include <cstdint>
#include <cstdio>

namespace platform
{
    // Return the stream size at EOF, or -1 on failure.
    inline int64_t file_size(std::FILE *f)
    {
#ifdef _WIN32
        if (_fseeki64(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return _ftelli64(f);
#else
        static_assert(sizeof(off_t) == 8, "64-bit off_t required; build with -D_FILE_OFFSET_BITS=64");
        if (fseeko(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return ftello(f);
#endif
    }

} // namespace platform
