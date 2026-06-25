#include "Core/Events.h"
#include "Format/Project.h"
#include "helpers/FixtureCompare.h"
#include "helpers/PmFixture.h"
#include <doctest/doctest.h>

using ofs::test::PmFixture;
using Meta = ofs::FunscriptMetadata;

TEST_CASE("Text setters mutate project.metadata") {
    PmFixture f;
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.title = "T"; }});
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.creator = "C"; }});
    CHECK(f.project().metadata.title == "T");
    CHECK(f.project().metadata.creator == "C");
}

TEST_CASE("Tags add/remove; out-of-range remove (caller-guarded) is a no-op") {
    PmFixture f;
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.tags.push_back("x"); }});
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.tags.push_back("y"); }});
    // The handler can't validate an opaque mutator, so the bounds-check is the caller's (mirrors UI).
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) {
        if (42 < static_cast<int>(m.tags.size()))
            m.tags.erase(m.tags.begin() + 42);
    }});
    CHECK(f.project().metadata.tags.size() == 2);
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.tags.erase(m.tags.begin()); }});
    REQUIRE(f.project().metadata.tags.size() == 1);
    CHECK(f.project().metadata.tags[0] == "y");
}

TEST_CASE("Resetting metadata clears every field") {
    PmFixture f;
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.title = "T"; }});
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m.tags.push_back("x"); }});
    f.send(ofs::ModifyEvent<Meta>{[](Meta &m) { m = Meta{}; }});
    CHECK(f.project().metadata.title.empty());
    CHECK(f.project().metadata.tags.empty());
}

TEST_CASE("Loading a metadata preset replaces metadata wholesale") {
    PmFixture f;
    Meta m;
    m.title = "Preset";
    m.performers = {"p1"};
    f.send(ofs::ModifyEvent<Meta>{[m](Meta &dst) { dst = m; }});
    CHECK(f.project().metadata.title == "Preset");
    REQUIRE(f.project().metadata.performers.size() == 1);
}

// basic.ofp carries a known metadata block (tools/gen_test_fixtures.py). This pins that the
// Python-generated CBOR decodes through Project::from_json into the expected fields — the same
// metadata the UI test (tests/ui/suite_metadata.cpp) reads back from the loaded fixture.
TEST_CASE("basic.ofp fixture deserializes its metadata block") {
    auto proj = ofs::Project::load(ofs::test::fixturePath("basic.ofp"));
    REQUIRE(proj.has_value());
    const auto &m = proj->metadata;
    CHECK(m.title == "Fixture Project");
    CHECK(m.creator == "OFS Test Suite");
    CHECK(m.scriptUrl == "https://example.com/script.funscript");
    CHECK(m.videoUrl == "https://example.com/video.mp4");
    CHECK(m.license == "Free");
    REQUIRE(m.tags.size() == 3);
    CHECK(m.tags[0] == "alpha");
    CHECK(m.tags[2] == "gamma");
    REQUIRE(m.performers.size() == 2);
    CHECK(m.performers[0] == "Performer One");
}
