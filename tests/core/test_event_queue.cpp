#include "Core/EventQueue.h"
#include "helpers/EventCapture.h"
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <vector>

using ofs::EventQueue;
using ofs::test::EventCapture;

TEST_CASE("EventQueue delivers a pushed event to a single handler after drain") {
    EventQueue eq;
    int received = 0;
    eq.on<int>([&](const int &v) { received = v; });
    eq.freeze();

    eq.push(42);
    CHECK(received == 0); // not yet delivered
    eq.drain();
    CHECK(received == 42);
}

TEST_CASE("EventQueue fires two handlers for the same type in registration order") {
    EventQueue eq;
    std::vector<int> order;
    eq.on<int>([&](const int &v) { order.push_back(v * 10); });
    eq.on<int>([&](const int &v) { order.push_back(v * 100); });
    eq.freeze();

    eq.push(1);
    eq.drain();

    REQUIRE(order.size() == 2);
    CHECK(order[0] == 10);
    CHECK(order[1] == 100);
}

TEST_CASE("EventQueue drain delivers multiple distinct event types") {
    EventQueue eq;
    int intVal = 0;
    float floatVal = 0.0f;
    eq.on<int>([&](const int &v) { intVal = v; });
    eq.on<float>([&](const float &v) { floatVal = v; });
    eq.freeze();

    eq.push(7);
    eq.push(3.14f);
    eq.drain();

    CHECK(intVal == 7);
    CHECK(floatVal == doctest::Approx(3.14f));
}

TEST_CASE("EventQueue push from a worker thread is delivered on drain") {
    EventQueue eq;
    int received = 0;
    eq.on<int>([&](const int &v) { received = v; });
    eq.freeze();

    std::thread worker([&]() { eq.push(99); });
    worker.join();

    CHECK(received == 0); // not yet delivered
    eq.drain();
    CHECK(received == 99);
}

TEST_CASE("EventQueue drain is idempotent when queue is empty") {
    EventQueue eq;
    int count = 0;
    eq.on<int>([&](const int &) { ++count; });
    eq.freeze();

    eq.drain();
    eq.drain();
    CHECK(count == 0);
}

// Note: freeze() followed by on<E>() triggers assert() (abort). This invariant is
// enforced at runtime in debug builds via the assert in EventQueue::on(). A death
// test would require subprocess-based infrastructure not available in doctest.

TEST_CASE("EventQueue silently drops an event type with no registered handler") {
    EventQueue eq;
    int received = 0;
    eq.on<int>([&](const int &v) { received += v; });
    eq.freeze();

    // No handler for std::string → push is a no-op (not buffered, no crash).
    eq.push(std::string("unsubscribed"));
    eq.push(7);
    eq.drain();

    CHECK(received == 7);
}

TEST_CASE("EventQueue processes a re-entrant push within the same drain, after the current event") {
    EventQueue eq;
    std::vector<std::string> order;
    // The int handler pushes a string mid-drain; it must be handled in the same drain pass,
    // FIFO after the event currently being processed.
    eq.on<int>([&](const int &v) {
        order.push_back("int:" + std::to_string(v));
        eq.push(std::string("from-int"));
    });
    eq.on<std::string>([&](const std::string &s) { order.push_back("str:" + s); });
    eq.freeze();

    eq.push(5);
    eq.drain();

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "int:5");
    CHECK(order[1] == "str:from-int");
}

TEST_CASE("EventQueue drains a re-entrant chain to completion in one pass") {
    EventQueue eq;
    std::vector<int> seen;
    // Each event re-pushes the next; drain() loops until the queue is empty, so the whole
    // chain resolves in a single drain (there is no per-drain depth boundary).
    eq.on<int>([&](const int &v) {
        seen.push_back(v);
        if (v < 3)
            eq.push(v + 1);
    });
    eq.freeze();

    eq.push(0);
    eq.drain();

    REQUIRE(seen.size() == 4);
    CHECK(seen == std::vector<int>{0, 1, 2, 3});
}
