#include "Core/FunscriptMetadata.h"
#include <doctest/doctest.h>
#include <map>
#include <nlohmann/json.hpp>

TEST_CASE("FunscriptMetadata round-trips through JSON") {
    ofs::FunscriptMetadata m;
    m.title = "t";
    m.creator = "c";
    m.scriptUrl = "su";
    m.videoUrl = "vu";
    m.description = "d";
    m.notes = "n";
    m.tags = {"a", "b"};
    m.performers = {"p"};
    m.license = "Free";

    nlohmann::json j;
    to_json(j, m);
    ofs::FunscriptMetadata out;
    from_json(j, out);

    CHECK(out.title == "t");
    CHECK(out.scriptUrl == "su");
    CHECK(out.videoUrl == "vu");
    REQUIRE(out.tags.size() == 2);
    CHECK(out.license == "Free");
}

TEST_CASE("FunscriptMetadata from_json on an empty object yields empty defaults") {
    ofs::FunscriptMetadata out;
    from_json(nlohmann::json::object(), out);
    CHECK(out.title.empty());
    CHECK(out.tags.empty());
}

TEST_CASE("FunscriptMetadata serializes camelCase url keys") {
    ofs::FunscriptMetadata m;
    m.scriptUrl = "x";
    m.videoUrl = "y";
    nlohmann::json j;
    to_json(j, m);
    CHECK(j.contains("scriptUrl"));
    CHECK(j.contains("videoUrl"));
}

TEST_CASE("custom fields of every JSON type round-trip inline through FunscriptMetadata JSON") {
    ofs::FunscriptMetadata m;
    m.title = "t";
    m.customFields = {{.key = "nul", .value = nullptr},
                      {.key = "flag", .value = true},
                      {.key = "count", .value = 42},
                      {.key = "ratio", .value = 1.5},
                      {.key = "label", .value = "hi"},
                      {.key = "list", .value = nlohmann::json::array({1, "two", 3})},
                      {.key = "nested", .value = nlohmann::json{{"min", 0}, {"max", 100}}}};

    nlohmann::json j;
    to_json(j, m);
    // Written inline at the top level, not nested under a wrapper.
    CHECK(j["count"] == 42);
    CHECK(j["nested"]["max"] == 100);

    ofs::FunscriptMetadata out;
    from_json(j, out);
    CHECK(out.title == "t");
    // The on-disk json object sorts its keys (default nlohmann::json is std::map-backed), so custom
    // field order is not preserved — compare as a key->value map. Values must match verbatim,
    // preserving JSON type (int 42 stays integral, 1.5 stays float, nested object stays nested).
    auto toMap = [](const std::vector<ofs::CustomMetadataField> &fields) {
        std::map<std::string, nlohmann::json> map;
        for (const auto &f : fields)
            map[f.key] = f.value;
        return map;
    };
    CHECK(toMap(out.customFields) == toMap(m.customFields));
    REQUIRE(out.customFields.size() == m.customFields.size());
}

TEST_CASE("standard keys are never captured as custom fields") {
    nlohmann::json j = {{"title", "t"}, {"scriptUrl", "su"}, {"weird", 7}};
    ofs::FunscriptMetadata out;
    from_json(j, out);
    REQUIRE(out.customFields.size() == 1);
    CHECK(out.customFields[0].key == "weird");
    CHECK(out.customFields[0].value == 7);
}

TEST_CASE("a custom field colliding with a standard key (or empty) is not written") {
    ofs::FunscriptMetadata m;
    m.title = "real-title";
    m.customFields = {{.key = "title", .value = "shadow"}, {.key = "", .value = 1}};
    nlohmann::json j;
    to_json(j, m);
    CHECK(j["title"] == "real-title"); // standard field wins
    CHECK_FALSE(j.contains(""));
}
