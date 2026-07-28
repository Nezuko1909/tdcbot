#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

TEST_CASE("sample test", "[core]") {
    REQUIRE(1 + 1 == 2);
}
