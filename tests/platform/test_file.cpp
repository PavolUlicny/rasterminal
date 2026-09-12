#include "tests/test.h"
#include "tests/platform/test_util.h"
#include "src/platform/file.h"

#include <cstdint>
#include <cstdio>

using platform_test::ScopedTmpFile;

// Small fixtures use the same 64-bit seek and tell path as large files.

TEST(platform, file_size_reports_exact_byte_count)
{
    unsigned char data[259];
    for (size_t i = 0; i < sizeof(data); i++)
    {
        data[i] = static_cast<unsigned char>(i % 256);
    }
    ScopedTmpFile t("rasterminal_test_file_size.bin", data, sizeof(data));

    std::FILE *f = std::fopen(t.path.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    const int64_t size = platform::file_size(f);
    // file_size leaves the stream at EOF.
    const int next = std::fgetc(f);
    std::fclose(f);
    ASSERT_EQ(size, int64_t{ 259 });
    ASSERT_EQ(next, EOF);
}

TEST(platform, file_size_empty_file_is_zero)
{
    ScopedTmpFile t("rasterminal_test_file_size_empty.bin", "", 0);

    std::FILE *f = std::fopen(t.path.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    const int64_t size = platform::file_size(f);
    std::fclose(f);
    ASSERT_EQ(size, int64_t{ 0 });
}
