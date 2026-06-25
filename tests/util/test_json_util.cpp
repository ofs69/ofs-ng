// Unit tests for the forward-tolerant JSON read helpers in Util/JsonUtil.h. The whole point of these
// helpers is that a missing key or a present-but-wrong-typed value yields the fallback instead of
// throwing, so the load paths survive foreign / legacy / hand-edited documents.
#include "Util/JsonUtil.h"
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using ofs::util::jsonArrayIf;
using ofs::util::jsonObjectIf;
using ofs::util::jsonValueOr;
using ofs::util::loadJsonFile;
using ofs::util::parseJsonFile;

namespace {
std::filesystem::path tempPath(const char *name) {
    return std::filesystem::temp_directory_path() / name;
}
void writeText(const std::filesystem::path &p, const std::string &s) {
    std::ofstream os(p, std::ios::binary);
    os << s;
}
} // namespace

TEST_CASE("jsonValueOr returns the value when present and well-typed") {
    nlohmann::json j = {{"n", 42}, {"s", "hi"}, {"f", 1.5}};
    CHECK(jsonValueOr(j, "n", 0) == 42);
    CHECK(jsonValueOr(j, "s", std::string{}) == "hi");
    CHECK(jsonValueOr(j, "f", 0.0) == doctest::Approx(1.5));
}

TEST_CASE("jsonValueOr falls back for a missing key") {
    nlohmann::json j = {{"n", 42}};
    CHECK(jsonValueOr(j, "absent", 7) == 7);
    CHECK(jsonValueOr(j, "absent", std::string("def")) == "def");
}

TEST_CASE("jsonValueOr falls back for a present-but-mismatched type") {
    nlohmann::json j = {{"n", "not a number"}, {"obj", nlohmann::json::object()}};
    // A string where an int is wanted, and an object where a string is wanted, both fall back
    // rather than throwing (nlohmann's own j.value() would throw here).
    CHECK(jsonValueOr(j, "n", 99) == 99);
    CHECK(jsonValueOr(j, "obj", std::string("fallback")) == "fallback");
}

TEST_CASE("jsonValueOr falls back for an explicit null") {
    nlohmann::json j = {{"x", nullptr}};
    CHECK(jsonValueOr(j, "x", 5) == 5);
}

TEST_CASE("jsonArrayIf returns the array only when present and an array") {
    nlohmann::json j = {{"arr", {1, 2, 3}}, {"notArr", 7}};
    const auto *a = jsonArrayIf(j, "arr");
    REQUIRE(a != nullptr);
    CHECK(a->size() == 3);
    CHECK(jsonArrayIf(j, "notArr") == nullptr);
    CHECK(jsonArrayIf(j, "missing") == nullptr);
}

TEST_CASE("jsonObjectIf returns the object only when present and an object") {
    nlohmann::json j = {{"obj", {{"k", 1}}}, {"notObj", {1, 2}}};
    const auto *o = jsonObjectIf(j, "obj");
    REQUIRE(o != nullptr);
    CHECK(o->contains("k"));
    CHECK(jsonObjectIf(j, "notObj") == nullptr); // an array is not an object
    CHECK(jsonObjectIf(j, "missing") == nullptr);
}

TEST_CASE("parseJsonFile reads a valid document") {
    const auto path = tempPath("ofs_test_jsonutil_valid.json");
    writeText(path, R"({"a":1,"b":[2,3]})");
    auto j = parseJsonFile(path, "test");
    REQUIRE(j.has_value());
    CHECK((*j)["a"] == 1);
    std::filesystem::remove(path);
}

TEST_CASE("parseJsonFile returns nullopt for a missing file") {
    CHECK_FALSE(parseJsonFile("no_such_jsonutil_file.json", "test").has_value());
}

TEST_CASE("parseJsonFile returns nullopt for malformed JSON") {
    const auto path = tempPath("ofs_test_jsonutil_bad.json");
    writeText(path, "{not valid json");
    CHECK_FALSE(parseJsonFile(path, "test").has_value());
    std::filesystem::remove(path);
}

TEST_CASE("loadJsonFile deserializes to a typed value") {
    const auto path = tempPath("ofs_test_jsonutil_vec.json");
    writeText(path, "[1,2,3,4]");
    auto v = loadJsonFile<std::vector<int>>(path, "test");
    REQUIRE(v.has_value());
    CHECK(*v == std::vector<int>{1, 2, 3, 4});
    std::filesystem::remove(path);
}

TEST_CASE("loadJsonFile returns nullopt when the document can't convert to T") {
    const auto path = tempPath("ofs_test_jsonutil_mismatch.json");
    writeText(path, R"({"not":"an array"})");
    CHECK_FALSE(loadJsonFile<std::vector<int>>(path, "test").has_value());
    std::filesystem::remove(path);
}
