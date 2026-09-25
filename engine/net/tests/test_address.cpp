// Address parsing, formatting and conversions.

#include <doctest/doctest.h>

#include <unordered_set>

#include <netcode.h>

#include "helios/net/address.h"
#include "netcode_glue.h"

using namespace helios;
using namespace helios::net;

TEST_SUITE("net.address") {
    TEST_CASE("IPv4 parse and format") {
        auto a = Address::parse("127.0.0.1:40000");
        REQUIRE(a);
        CHECK(a->isIpv4());
        CHECK(a->port() == 40000);
        CHECK(a->toString() == "127.0.0.1:40000");
        CHECK(a->isLoopback());
        CHECK(*a == Address::ipv4(127, 0, 0, 1, 40000));

        auto b = Address::parse("10.0.0.5");
        REQUIRE(b);
        CHECK(b->port() == 0);
        CHECK(b->toString() == "10.0.0.5");
        CHECK(Address::parse("0.0.0.0:0")->isUnspecified());
        CHECK(Address::parse("255.255.255.255:65535")->port() == 65535);
    }

    TEST_CASE("IPv6 parse and RFC 5952 format") {
        struct Case {
            const char* in;
            const char* out;
        };
        const Case cases[] = {
            {"[::1]:40001", "[::1]:40001"},
            {"::1", "::1"},
            {"[::]:7777", "[::]:7777"},
            {"[2001:db8:85a3::8a2e:370:7334]:65535", "[2001:db8:85a3::8a2e:370:7334]:65535"},
            {"[2001:0db8:0000:0000:0000:0000:0000:0001]:1", "[2001:db8::1]:1"},
            {"[2001:db8:0:0:1:0:0:1]:2", "[2001:db8::1:0:0:1]:2"}, // first longest run wins
            {"[2001:db8:0:1:1:1:1:1]:3", "[2001:db8:0:1:1:1:1:1]:3"}, // single zero is not compressed
            {"[fd00::3]:7778", "[fd00::3]:7778"},
            {"[::ffff:1.2.3.4]:5", "[::ffff:1.2.3.4]:5"},
            {"[FE80::ABCD]:9", "[fe80::abcd]:9"},
            {"[1:2:3:4:5:6:7:8]:9", "[1:2:3:4:5:6:7:8]:9"},
            {"[1::]:9", "[1::]:9"},
        };
        for (const Case& c : cases) {
            INFO(c.in);
            auto a = Address::parse(c.in);
            REQUIRE(a);
            CHECK(a->isIpv6());
            CHECK(a->toString() == c.out);
            // Round trip.
            auto b = Address::parse(a->toString());
            REQUIRE(b);
            CHECK(*b == *a);
        }
        CHECK(Address::parse("[::1]:5")->isLoopback());
        CHECK(Address::parse("[::]:5")->isUnspecified());
    }

    TEST_CASE("rejects malformed addresses") {
        const char* bad[] = {"",           "1.2.3",       "1.2.3.4.5",   "256.1.1.1",    "01.2.3.4",   "1.2.3.4:",
                             ":80",        "1.2.3.4:65536", "1.2.3.4:-1", "1.2.3.4:8a",   "[::1",       "[::1]x",
                             "[::1]:",     "::1::",       ":::",         "1::2::3",      "1:2:3:4:5:6:7:8:9",
                             "[12345::]:1", "fe80::1%eth0", "[g::1]:1",  "1:2:3:4:5:6:7", " 1.2.3.4",   "1.2.3.4 ",
                             "[::1.2.3]:1", "[1:2:3:4:5:6:7:1.2.3.4]:1", "[::ffff:1.2.3.4:5]:1"};
        for (const char* s : bad) {
            INFO(s);
            CHECK_FALSE(Address::parse(s).has_value());
        }
    }

    TEST_CASE("mapped addresses, host comparison and hashing") {
        const Address v4 = Address::ipv4(192, 168, 1, 20, 7777);
        const Address mapped = v4.toV4Mapped();
        CHECK(mapped.isIpv6());
        CHECK(mapped.isV4Mapped());
        CHECK(mapped.unmapped() == v4);
        CHECK(mapped != v4);
        CHECK(v4.unmapped() == v4);
        CHECK(v4.sameHost(v4.withPort(1)));
        CHECK_FALSE(v4.sameHost(Address::ipv4(192, 168, 1, 21, 7777)));
        CHECK(v4.hostHash() == v4.withPort(1).hostHash());
        CHECK(v4.hash() != v4.withPort(1).hash());
        std::unordered_set<Address> set{v4, v4.withPort(1), mapped};
        CHECK(set.size() == 3);
        CHECK(set.count(Address::parse("192.168.1.20:7777").value()) == 1);
        CHECK(Address().toString() == "NONE");
        CHECK_FALSE(Address().isValid());
        CHECK(std::format("{}", v4) == "192.168.1.20:7777");
    }

    TEST_CASE("netcode conversions agree with netcode_parse_address") {
        const char* inputs[] = {"127.0.0.1:40000", "[::1]:40001", "10.0.0.5:7777", "[2001:db8:85a3::8a2e:370:7334]:65535",
                                "[fd00::3]:7778"};
        for (const char* s : inputs) {
            INFO(s);
            netcode_address_t nc;
            REQUIRE(netcode_parse_address(s, &nc) == NETCODE_OK);
            const Address ours = Address::parse(s).value();
            CHECK(detail::fromNetcode(nc) == ours);
            netcode_address_t back = detail::toNetcode(ours);
            CHECK(netcode_address_equal(&back, &nc) == 1);
        }
        // IPv4-mapped addresses reach netcode as plain IPv4 (tokens list plain IPv4).
        const netcode_address_t nc = detail::toNetcode(Address::ipv4(1, 2, 3, 4, 5).toV4Mapped());
        CHECK(nc.type == NETCODE_ADDRESS_IPV4);
        CHECK(nc.port == 5);
    }
}
