#include <doctest/doctest.h>

#include "Services/CommandRegistry.h"
#include "helpers/TestProject.h"

using ofs::test::TestProject;

namespace {
ofs::Command noopCmd(const char *id) {
    return ofs::Command{.id = id, .group = "Test", .title = id, .run = [](ofs::EventQueue &) {}};
}
} // namespace

TEST_CASE("frecency is 0 for a command never used this session") {
    TestProject tp;
    ofs::CommandRegistry reg{tp.eq};
    reg.add(noopCmd("a"));
    CHECK(reg.frecency("a") == 0);
    CHECK(reg.frecency("missing") == 0);
}

TEST_CASE("more uses rank higher than fewer (frequency dominates)") {
    TestProject tp;
    ofs::CommandRegistry reg{tp.eq};
    reg.add(noopCmd("a"));
    reg.add(noopCmd("b"));
    reg.recordUse("a");
    reg.recordUse("b");
    reg.recordUse("a"); // a used twice, b once
    CHECK(reg.frecency("a") > reg.frecency("b"));
}

TEST_CASE("among equal use counts, the more recently used ranks higher") {
    TestProject tp;
    ofs::CommandRegistry reg{tp.eq};
    reg.add(noopCmd("a"));
    reg.add(noopCmd("b"));
    reg.recordUse("a");
    reg.recordUse("b"); // same count (1), b more recent
    CHECK(reg.frecency("b") > reg.frecency("a"));
}

TEST_CASE("run(id) records a use for frecency") {
    TestProject tp;
    ofs::CommandRegistry reg{tp.eq};
    reg.add(noopCmd("a"));
    CHECK(reg.frecency("a") == 0);
    reg.run("a");
    CHECK(reg.frecency("a") > 0);
}
