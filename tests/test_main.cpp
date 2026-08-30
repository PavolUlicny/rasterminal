#include "tests/test.h"

#include <cstring>

#ifdef _WIN32
namespace platform_test
{
    int run_windows_console_control_helper(int argc, char *argv[]);
}
#endif

#ifdef _WIN32
int main(int argc, char *argv[])
{
    if (argc >= 2 && std::strcmp(argv[1], "--windows-console-control-helper") == 0)
    {
        return platform_test::run_windows_console_control_helper(argc, argv);
    }
    return testing::run_all_tests();
}
#else
int main()
{
    return testing::run_all_tests();
}
#endif
