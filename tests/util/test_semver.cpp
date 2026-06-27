#include <doctest/doctest.h>

#include "Util/SemVer.h"

using ofs::util::compareSemVer;
using ofs::util::isNewer;
using ofs::util::parseSemVer;

TEST_CASE("parse plain vX.Y.Z") {
    auto v = parseSemVer("v1.2.3");
    REQUIRE(v.has_value());
    CHECK(v->major == 1);
    CHECK(v->minor == 2);
    CHECK(v->patch == 3);
    CHECK(v->prerelease.empty());
    CHECK(v->commitsAhead == 0);
    CHECK_FALSE(v->dirty);
}

TEST_CASE("leading v is optional and case-insensitive") {
    CHECK(parseSemVer("1.2.3").has_value());
    CHECK(parseSemVer("V1.2.3").has_value());
    CHECK(parseSemVer("1.2.3")->patch == 3);
}

TEST_CASE("git describe suffix becomes commitsAhead, not a prerelease") {
    auto v = parseSemVer("v1.0.0-5-gabc1234");
    REQUIRE(v.has_value());
    CHECK(v->major == 1);
    CHECK(v->minor == 0);
    CHECK(v->patch == 0);
    CHECK(v->prerelease.empty()); // the -N-gHASH tail is a dev marker, not semver prerelease
    CHECK(v->commitsAhead == 5);
    CHECK_FALSE(v->dirty);
}

TEST_CASE("dirty marker is parsed without a prerelease or ahead count") {
    auto v = parseSemVer("v1.0.0-dirty");
    REQUIRE(v.has_value());
    CHECK(v->dirty);
    CHECK(v->commitsAhead == 0);
    CHECK(v->prerelease.empty());
}

TEST_CASE("describe suffix and dirty marker combine") {
    auto v = parseSemVer("v1.0.0-5-gabc1234-dirty");
    REQUIRE(v.has_value());
    CHECK(v->commitsAhead == 5);
    CHECK(v->dirty);
    CHECK(v->prerelease.empty());
}

TEST_CASE("semver prerelease label is retained") {
    auto v = parseSemVer("v1.2.0-beta.1");
    REQUIRE(v.has_value());
    CHECK(v->prerelease == "beta.1");
    CHECK(v->commitsAhead == 0);
    CHECK_FALSE(v->dirty);
}

TEST_CASE("a prerelease build can still carry a describe/dirty tail") {
    auto v = parseSemVer("v1.2.0-beta.1-3-gdeadbee-dirty");
    REQUIRE(v.has_value());
    CHECK(v->prerelease == "beta.1");
    CHECK(v->commitsAhead == 3);
    CHECK(v->dirty);
}

TEST_CASE("malformed versions return nullopt") {
    CHECK_FALSE(parseSemVer("").has_value());
    CHECK_FALSE(parseSemVer("v1.2").has_value()); // missing patch
    CHECK_FALSE(parseSemVer("garbage").has_value());
    CHECK_FALSE(parseSemVer("v1.x.0").has_value());
    CHECK_FALSE(parseSemVer("v.1.2.3").has_value());
}

TEST_CASE("numeric core ordering") {
    CHECK(isNewer(*parseSemVer("1.0.1"), *parseSemVer("1.0.0")));
    CHECK(isNewer(*parseSemVer("1.1.0"), *parseSemVer("1.0.9")));
    CHECK(isNewer(*parseSemVer("2.0.0"), *parseSemVer("1.9.9")));
    CHECK_FALSE(isNewer(*parseSemVer("1.0.0"), *parseSemVer("1.0.0")));
    CHECK_FALSE(isNewer(*parseSemVer("1.0.0"), *parseSemVer("1.0.1")));
}

TEST_CASE("components compare numerically, not lexically") {
    CHECK(isNewer(*parseSemVer("1.0.10"), *parseSemVer("1.0.9")));
    CHECK(isNewer(*parseSemVer("1.10.0"), *parseSemVer("1.9.0")));
}

TEST_CASE("a release outranks its own prerelease") {
    CHECK(isNewer(*parseSemVer("1.0.0"), *parseSemVer("1.0.0-beta.1")));
    CHECK_FALSE(isNewer(*parseSemVer("1.0.0-beta.1"), *parseSemVer("1.0.0")));
}

TEST_CASE("prerelease identifier precedence") {
    CHECK(isNewer(*parseSemVer("1.0.0-beta.2"), *parseSemVer("1.0.0-beta.1")));
    CHECK(isNewer(*parseSemVer("1.0.0-beta.11"), *parseSemVer("1.0.0-beta.2"))); // numeric, not lexical
    CHECK(isNewer(*parseSemVer("1.0.0-rc.1"), *parseSemVer("1.0.0-beta.1")));    // rc > beta (ASCII)
    CHECK(isNewer(*parseSemVer("1.0.0-alpha.2"), *parseSemVer("1.0.0-alpha.1")));
    CHECK(isNewer(*parseSemVer("1.0.0-alpha.beta"), *parseSemVer("1.0.0-alpha.1"))); // alnum > numeric
}

TEST_CASE("a dev build ahead of a tag is newer than the clean tag") {
    // Running a dev build 5 commits past v1.0.0; the latest release is v1.0.0 -> do NOT offer it.
    CHECK_FALSE(isNewer(*parseSemVer("v1.0.0"), *parseSemVer("v1.0.0-5-gabc1234")));
    // A genuinely newer release IS offered to that same dev build.
    CHECK(isNewer(*parseSemVer("v1.0.1"), *parseSemVer("v1.0.0-5-gabc1234")));
}

TEST_CASE("compareSemVer sign contract") {
    CHECK(compareSemVer(*parseSemVer("1.2.3"), *parseSemVer("1.2.3")) == 0);
    CHECK(compareSemVer(*parseSemVer("1.2.4"), *parseSemVer("1.2.3")) > 0);
    CHECK(compareSemVer(*parseSemVer("1.2.2"), *parseSemVer("1.2.3")) < 0);
}
