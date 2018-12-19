/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless requied by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define LOG_TAG "netd_test"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h> /* poll */
#include <resolv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>
#include <thread>

#include <android-base/stringprintf.h>
#include <cutils/sockets.h>
#include <gtest/gtest.h>
#include <openssl/base64.h>
#include <private/android_filesystem_config.h>
#include <utils/Log.h>

#include "NetdClient.h"
#include "netid_client.h"  // NETID_UNSET
#include "netd_resolv/params.h"  // MAX_NS

#include "dns_responder/dns_responder.h"
#include "dns_responder/dns_responder_client.h"
#include "dns_responder/dns_tls_frontend.h"

#include "NetdConstants.h"
#include "ResolverStats.h"

#include "android/net/INetd.h"
#include "android/net/metrics/INetdEventListener.h"
#include "binder/IServiceManager.h"
#include "netdutils/SocketOption.h"

// TODO: make this dynamic and stop depending on implementation details.
constexpr int TEST_NETID = 30;
constexpr int MAXPACKET = (8 * 1024);

// Semi-public Bionic hook used by the NDK (frameworks/base/native/android/net.c)
// Tested here for convenience.
extern "C" int android_getaddrinfofornet(const char* hostname, const char* servname,
                                         const addrinfo* hints, unsigned netid, unsigned mark,
                                         struct addrinfo** result);

using android::base::StringPrintf;
using android::net::ResolverStats;
using android::net::metrics::INetdEventListener;
using android::netdutils::enableSockopt;

// TODO: move into libnetdutils?
namespace {
ScopedAddrinfo safe_getaddrinfo(const char* node, const char* service,
                                const struct addrinfo* hints) {
    addrinfo* result = nullptr;
    if (getaddrinfo(node, service, hints, &result) != 0) {
        result = nullptr;  // Should already be the case, but...
    }
    return ScopedAddrinfo(result);
}
}  // namespace

// Emulates the behavior of UnorderedElementsAreArray, which currently cannot be used.
// TODO: Use UnorderedElementsAreArray, which depends on being able to compile libgmock_host,
// if that is not possible, improve this hacky algorithm, which is O(n**2)
template <class A, class B>
bool UnorderedCompareArray(const A& a, const B& b) {
    if (a.size() != b.size()) return false;
    for (const auto& a_elem : a) {
        size_t a_count = 0;
        for (const auto& a_elem2 : a) {
            if (a_elem == a_elem2) {
                ++a_count;
            }
        }
        size_t b_count = 0;
        for (const auto& b_elem : b) {
            if (a_elem == b_elem) ++b_count;
        }
        if (a_count != b_count) return false;
    }
    return true;
}

class ResolverTest : public ::testing::Test, public DnsResponderClient {
  protected:
    void SetUp() {
        // Ensure resolutions go via proxy.
        DnsResponderClient::SetUp();

        // If DNS reporting is off: turn it on so we run through everything.
        auto rv = mNetdSrv->getMetricsReportingLevel(&mOriginalMetricsLevel);
        ASSERT_TRUE(rv.isOk());
        if (mOriginalMetricsLevel != INetdEventListener::REPORTING_LEVEL_FULL) {
            rv = mNetdSrv->setMetricsReportingLevel(INetdEventListener::REPORTING_LEVEL_FULL);
            ASSERT_TRUE(rv.isOk());
        }
    }

    void TearDown() {
        if (mOriginalMetricsLevel != INetdEventListener::REPORTING_LEVEL_FULL) {
            auto rv = mNetdSrv->setMetricsReportingLevel(mOriginalMetricsLevel);
            ASSERT_TRUE(rv.isOk());
        }

        DnsResponderClient::TearDown();
    }

    bool GetResolverInfo(std::vector<std::string>* servers, std::vector<std::string>* domains,
                         std::vector<std::string>* tlsServers, __res_params* params,
                         std::vector<ResolverStats>* stats) {
        using android::net::INetd;
        std::vector<int32_t> params32;
        std::vector<int32_t> stats32;
        auto rv = mNetdSrv->getResolverInfo(TEST_NETID, servers, domains, tlsServers, &params32,
                                            &stats32);
        if (!rv.isOk() || params32.size() != INetd::RESOLVER_PARAMS_COUNT) {
            return false;
        }
        *params = __res_params {
            .sample_validity = static_cast<uint16_t>(
                    params32[INetd::RESOLVER_PARAMS_SAMPLE_VALIDITY]),
            .success_threshold = static_cast<uint8_t>(
                    params32[INetd::RESOLVER_PARAMS_SUCCESS_THRESHOLD]),
            .min_samples = static_cast<uint8_t>(
                    params32[INetd::RESOLVER_PARAMS_MIN_SAMPLES]),
            .max_samples = static_cast<uint8_t>(
                    params32[INetd::RESOLVER_PARAMS_MAX_SAMPLES]),
            .base_timeout_msec = params32[INetd::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC],
        };
        return ResolverStats::decodeAll(stats32, stats);
    }

    static std::string ToString(const hostent* he) {
        if (he == nullptr) return "<null>";
        char buffer[INET6_ADDRSTRLEN];
        if (!inet_ntop(he->h_addrtype, he->h_addr_list[0], buffer, sizeof(buffer))) {
            return "<invalid>";
        }
        return buffer;
    }

    static std::string ToString(const addrinfo* ai) {
        if (!ai)
            return "<null>";
        for (const auto* aip = ai ; aip != nullptr ; aip = aip->ai_next) {
            char host[NI_MAXHOST];
            int rv = getnameinfo(aip->ai_addr, aip->ai_addrlen, host, sizeof(host), nullptr, 0,
                    NI_NUMERICHOST);
            if (rv != 0)
                return gai_strerror(rv);
            return host;
        }
        return "<invalid>";
    }

    static std::string ToString(const ScopedAddrinfo& ai) { return ToString(ai.get()); }

    static std::vector<std::string> ToStrings(const addrinfo* ai) {
        std::vector<std::string> hosts;
        if (!ai) {
            hosts.push_back("<null>");
            return hosts;
        }
        for (const auto* aip = ai; aip != nullptr; aip = aip->ai_next) {
            char host[NI_MAXHOST];
            int rv = getnameinfo(aip->ai_addr, aip->ai_addrlen, host, sizeof(host), nullptr, 0,
                                 NI_NUMERICHOST);
            if (rv != 0) {
                hosts.clear();
                hosts.push_back(gai_strerror(rv));
                return hosts;
            } else {
                hosts.push_back(host);
            }
        }
        if (hosts.empty()) hosts.push_back("<invalid>");
        return hosts;
    }

    static std::vector<std::string> ToStrings(const ScopedAddrinfo& ai) {
        return ToStrings(ai.get());
    }

    size_t GetNumQueries(const test::DNSResponder& dns, const char* name) const {
        auto queries = dns.queries();
        size_t found = 0;
        for (const auto& p : queries) {
            if (p.first == name) {
                ++found;
            }
        }
        return found;
    }

    size_t GetNumQueriesForType(const test::DNSResponder& dns, ns_type type,
                                const char* name) const {
        auto queries = dns.queries();
        size_t found = 0;
        for (const auto& p : queries) {
            if (p.second == type && p.first == name) {
                ++found;
            }
        }
        return found;
    }

    bool WaitForPrefix64Detected(int netId, int timeoutMs) {
        constexpr int intervalMs = 2;
        const int limit = timeoutMs / intervalMs;
        for (int count = 0; count <= limit; ++count) {
            std::string prefix;
            auto rv = mNetdSrv->getPrefix64(netId, &prefix);
            if (rv.isOk()) {
                return true;
            }
            usleep(intervalMs * 1000);
        }
        return false;
    }

    void RunGetAddrInfoStressTest_Binder(unsigned num_hosts, unsigned num_threads,
            unsigned num_queries) {
        std::vector<std::string> domains = { "example.com" };
        std::vector<std::unique_ptr<test::DNSResponder>> dns;
        std::vector<std::string> servers;
        std::vector<DnsResponderClient::Mapping> mappings;
        ASSERT_NO_FATAL_FAILURE(SetupMappings(num_hosts, domains, &mappings));
        ASSERT_NO_FATAL_FAILURE(SetupDNSServers(MAXNS, mappings, &dns, &servers));

        ASSERT_TRUE(SetResolversForNetwork(servers, domains, mDefaultParams_Binder));

        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> threads(num_threads);
        for (std::thread& thread : threads) {
            thread = std::thread([&mappings, num_queries]() {
                for (unsigned i = 0 ; i < num_queries ; ++i) {
                    uint32_t ofs = arc4random_uniform(mappings.size());
                    auto& mapping = mappings[ofs];
                    addrinfo* result = nullptr;
                    int rv = getaddrinfo(mapping.host.c_str(), nullptr, nullptr, &result);
                    EXPECT_EQ(0, rv) << "error [" << rv << "] " << gai_strerror(rv);
                    if (rv == 0) {
                        std::string result_str = ToString(result);
                        EXPECT_TRUE(result_str == mapping.ip4 || result_str == mapping.ip6)
                            << "result='" << result_str << "', ip4='" << mapping.ip4
                            << "', ip6='" << mapping.ip6;
                    }
                    if (result) {
                        freeaddrinfo(result);
                        result = nullptr;
                    }
                }
            });
        }

        for (std::thread& thread : threads) {
            thread.join();
        }
        auto t1 = std::chrono::steady_clock::now();
        ALOGI("%u hosts, %u threads, %u queries, %Es", num_hosts, num_threads, num_queries,
                std::chrono::duration<double>(t1 - t0).count());
        ASSERT_NO_FATAL_FAILURE(ShutdownDNSServers(&dns));
    }

    const std::vector<std::string> mDefaultSearchDomains = { "example.com" };
    // <sample validity in s> <success threshold in percent> <min samples> <max samples>
    const std::vector<int> mDefaultParams_Binder = {
            300,     // SAMPLE_VALIDITY
            25,      // SUCCESS_THRESHOLD
            8,   8,  // {MIN,MAX}_SAMPLES
            100,     // BASE_TIMEOUT_MSEC
    };

  private:
    int mOriginalMetricsLevel;
};

TEST_F(ResolverTest, GetHostByName) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_srv = "53";
    const char* host_name = "hello.example.com.";
    const char *nonexistent_host_name = "nonexistent.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.3");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    const hostent* result;

    dns.clearQueries();
    result = gethostbyname("nonexistent");
    EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, nonexistent_host_name));
    ASSERT_TRUE(result == nullptr);
    ASSERT_EQ(HOST_NOT_FOUND, h_errno);

    dns.clearQueries();
    result = gethostbyname("hello");
    EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, host_name));
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ("1.2.3.3", ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_localhost) {
    constexpr char name[] = "localhost";
    constexpr char name_camelcase[] = "LocalHost";
    constexpr char addr[] = "127.0.0.1";
    constexpr char name_ip6[] = "ip6-localhost";
    constexpr char addr_ip6[] = "::1";
    constexpr char name_ip6_dot[] = "ip6-localhost.";
    constexpr char name_ip6_fqdn[] = "ip6-localhost.example.com.";

    // Add a dummy nameserver which shouldn't receive any queries
    constexpr char listen_addr[] = "127.0.0.3";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Expect no DNS queries; localhost is resolved via /etc/hosts
    const hostent* result = gethostbyname(name);
    EXPECT_TRUE(dns.queries().empty()) << dns.dumpQueries();
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(addr, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    // Ensure the hosts file resolver ignores case of hostnames
    result = gethostbyname(name_camelcase);
    EXPECT_TRUE(dns.queries().empty()) << dns.dumpQueries();
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(addr, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    // The hosts file also contains ip6-localhost, but gethostbyname() won't
    // return it unless the RES_USE_INET6 option is set. This would be easy to
    // change, but there's no point in changing the legacy behavior; new code
    // should be calling getaddrinfo() anyway.
    // So we check the legacy behavior, which results in amusing A-record
    // lookups for ip6-localhost, with and without search domains appended.
    dns.clearQueries();
    result = gethostbyname(name_ip6);
    EXPECT_EQ(2U, dns.queries().size()) << dns.dumpQueries();
    EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, name_ip6_dot)) << dns.dumpQueries();
    EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, name_ip6_fqdn)) << dns.dumpQueries();
    ASSERT_TRUE(result == nullptr);

    // Finally, use gethostbyname2() to resolve ip6-localhost to ::1 from
    // the hosts file.
    dns.clearQueries();
    result = gethostbyname2(name_ip6, AF_INET6);
    EXPECT_TRUE(dns.queries().empty()) << dns.dumpQueries();
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(16, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(addr_ip6, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_numeric) {
    // Add a dummy nameserver which shouldn't receive any queries
    constexpr char listen_addr[] = "127.0.0.3";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());
    ASSERT_TRUE(
            SetResolversForNetwork({listen_addr}, mDefaultSearchDomains, mDefaultParams_Binder));

    // Numeric v4 address: expect no DNS queries
    constexpr char numeric_v4[] = "192.168.0.1";
    dns.clearQueries();
    const hostent* result = gethostbyname(numeric_v4);
    EXPECT_EQ(0U, dns.queries().size());
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);  // v4
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(numeric_v4, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    // gethostbyname() recognizes a v6 address, and fails with no DNS queries
    constexpr char numeric_v6[] = "2001:db8::42";
    dns.clearQueries();
    result = gethostbyname(numeric_v6);
    EXPECT_EQ(0U, dns.queries().size());
    EXPECT_TRUE(result == nullptr);

    // Numeric v6 address with gethostbyname2(): succeeds with no DNS queries
    dns.clearQueries();
    result = gethostbyname2(numeric_v6, AF_INET6);
    EXPECT_EQ(0U, dns.queries().size());
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(16, result->h_length);  // v6
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(numeric_v6, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    // Numeric v6 address with scope work with getaddrinfo(),
    // but gethostbyname2() does not understand them; it issues two dns
    // queries, then fails. This hardly ever happens, there's no point
    // in fixing this. This test simply verifies the current (bogus)
    // behavior to avoid further regressions (like crashes, or leaks).
    constexpr char numeric_v6_scope[] = "fe80::1%lo";
    dns.clearQueries();
    result = gethostbyname2(numeric_v6_scope, AF_INET6);
    EXPECT_EQ(2U, dns.queries().size());  // OUCH!
    ASSERT_TRUE(result == nullptr);

    dns.stopServer();
}

TEST_F(ResolverTest, BinderSerialization) {
    using android::net::INetd;
    std::vector<int> params_offsets = {
        INetd::RESOLVER_PARAMS_SAMPLE_VALIDITY,
        INetd::RESOLVER_PARAMS_SUCCESS_THRESHOLD,
        INetd::RESOLVER_PARAMS_MIN_SAMPLES,
        INetd::RESOLVER_PARAMS_MAX_SAMPLES,
        INetd::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC,
    };
    int size = static_cast<int>(params_offsets.size());
    EXPECT_EQ(size, INetd::RESOLVER_PARAMS_COUNT);
    std::sort(params_offsets.begin(), params_offsets.end());
    for (int i = 0 ; i < size ; ++i) {
        EXPECT_EQ(params_offsets[i], i);
    }
}

TEST_F(ResolverTest, GetHostByName_Binder) {
    using android::net::INetd;

    std::vector<std::string> domains = { "example.com" };
    std::vector<std::unique_ptr<test::DNSResponder>> dns;
    std::vector<std::string> servers;
    std::vector<Mapping> mappings;
    ASSERT_NO_FATAL_FAILURE(SetupMappings(1, domains, &mappings));
    ASSERT_NO_FATAL_FAILURE(SetupDNSServers(4, mappings, &dns, &servers));
    ASSERT_EQ(1U, mappings.size());
    const Mapping& mapping = mappings[0];

    ASSERT_TRUE(SetResolversForNetwork(servers, domains, mDefaultParams_Binder));

    const hostent* result = gethostbyname(mapping.host.c_str());
    size_t total_queries = std::accumulate(dns.begin(), dns.end(), 0,
            [this, &mapping](size_t total, auto& d) {
                return total + GetNumQueriesForType(*d, ns_type::ns_t_a, mapping.entry.c_str());
            });

    EXPECT_LE(1U, total_queries);
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ(mapping.ip4, ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);

    std::vector<std::string> res_servers;
    std::vector<std::string> res_domains;
    std::vector<std::string> res_tls_servers;
    __res_params res_params;
    std::vector<ResolverStats> res_stats;
    ASSERT_TRUE(
            GetResolverInfo(&res_servers, &res_domains, &res_tls_servers, &res_params, &res_stats));
    EXPECT_EQ(servers.size(), res_servers.size());
    EXPECT_EQ(domains.size(), res_domains.size());
    EXPECT_EQ(0U, res_tls_servers.size());
    ASSERT_EQ(static_cast<size_t>(INetd::RESOLVER_PARAMS_COUNT), mDefaultParams_Binder.size());
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_SAMPLE_VALIDITY],
            res_params.sample_validity);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_SUCCESS_THRESHOLD],
            res_params.success_threshold);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_MIN_SAMPLES], res_params.min_samples);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_MAX_SAMPLES], res_params.max_samples);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC],
              res_params.base_timeout_msec);
    EXPECT_EQ(servers.size(), res_stats.size());

    EXPECT_TRUE(UnorderedCompareArray(res_servers, servers));
    EXPECT_TRUE(UnorderedCompareArray(res_domains, domains));

    ASSERT_NO_FATAL_FAILURE(ShutdownDNSServers(&dns));
}

TEST_F(ResolverTest, GetAddrInfo) {
    const char* listen_addr = "127.0.0.4";
    const char* listen_addr2 = "127.0.0.5";
    const char* listen_srv = "53";
    const char* host_name = "howdy.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    test::DNSResponder dns2(listen_addr2, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns2.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns2.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns2.startServer());

    std::vector<std::string> servers = { listen_addr };
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();
    dns2.clearQueries();

    ScopedAddrinfo result = safe_getaddrinfo("howdy", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    size_t found = GetNumQueries(dns, host_name);
    EXPECT_LE(1U, found);
    // Could be A or AAAA
    std::string result_str = ToString(result);
    EXPECT_TRUE(result_str == "1.2.3.4" || result_str == "::1.2.3.4")
        << ", result_str='" << result_str << "'";

    // Verify that the name is cached.
    size_t old_found = found;
    result = safe_getaddrinfo("howdy", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    found = GetNumQueries(dns, host_name);
    EXPECT_LE(1U, found);
    EXPECT_EQ(old_found, found);
    result_str = ToString(result);
    EXPECT_TRUE(result_str == "1.2.3.4" || result_str == "::1.2.3.4")
        << result_str;

    // Change the DNS resolver, ensure that queries are still cached.
    servers = { listen_addr2 };
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();
    dns2.clearQueries();

    result = safe_getaddrinfo("howdy", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    found = GetNumQueries(dns, host_name);
    size_t found2 = GetNumQueries(dns2, host_name);
    EXPECT_EQ(0U, found);
    EXPECT_LE(0U, found2);

    // Could be A or AAAA
    result_str = ToString(result);
    EXPECT_TRUE(result_str == "1.2.3.4" || result_str == "::1.2.3.4")
        << ", result_str='" << result_str << "'";

    dns.stopServer();
    dns2.stopServer();
}

TEST_F(ResolverTest, GetAddrInfoV4) {
    constexpr char listen_addr[] = "127.0.0.5";
    constexpr char listen_srv[] = "53";
    constexpr char host_name[] = "hola.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.5");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    addrinfo hints = {.ai_family = AF_INET};
    ScopedAddrinfo result = safe_getaddrinfo("hola", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(1U, GetNumQueries(dns, host_name));
    EXPECT_EQ("1.2.3.5", ToString(result));
}

TEST_F(ResolverTest, GetAddrInfo_localhost) {
    constexpr char name[] = "localhost";
    constexpr char addr[] = "127.0.0.1";
    constexpr char name_ip6[] = "ip6-localhost";
    constexpr char addr_ip6[] = "::1";

    // Add a dummy nameserver which shouldn't receive any queries
    constexpr char listen_addr[] = "127.0.0.5";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    ScopedAddrinfo result = safe_getaddrinfo(name, nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    // Expect no DNS queries; localhost is resolved via /etc/hosts
    EXPECT_TRUE(dns.queries().empty()) << dns.dumpQueries();
    EXPECT_EQ(addr, ToString(result));

    result = safe_getaddrinfo(name_ip6, nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    // Expect no DNS queries; ip6-localhost is resolved via /etc/hosts
    EXPECT_TRUE(dns.queries().empty()) << dns.dumpQueries();
    EXPECT_EQ(addr_ip6, ToString(result));
}

TEST_F(ResolverTest, MultidomainResolution) {
    std::vector<std::string> searchDomains = { "example1.com", "example2.com", "example3.com" };
    const char* listen_addr = "127.0.0.6";
    const char* listen_srv = "53";
    const char* host_name = "nihao.example2.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.3");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };
    ASSERT_TRUE(SetResolversForNetwork(servers, searchDomains, mDefaultParams_Binder));

    dns.clearQueries();
    const hostent* result = gethostbyname("nihao");
    EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, host_name));
    ASSERT_FALSE(result == nullptr);
    ASSERT_EQ(4, result->h_length);
    ASSERT_FALSE(result->h_addr_list[0] == nullptr);
    EXPECT_EQ("1.2.3.3", ToString(result));
    EXPECT_TRUE(result->h_addr_list[1] == nullptr);
    dns.stopServer();
}

TEST_F(ResolverTest, GetAddrInfoV6_numeric) {
    constexpr char listen_addr0[] = "127.0.0.7";
    constexpr char listen_srv[] = "53";
    constexpr char host_name[] = "ohayou.example.com.";
    constexpr char numeric_addr[] = "fe80::1%lo";

    test::DNSResponder dns(listen_addr0, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.setResponseProbability(0.0);
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::5");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr0};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    addrinfo hints = {.ai_family = AF_INET6};
    ScopedAddrinfo result = safe_getaddrinfo(numeric_addr, nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(numeric_addr, ToString(result));
    EXPECT_TRUE(dns.queries().empty());  // Ensure no DNS queries were sent out

    // Now try a non-numeric hostname query with the AI_NUMERICHOST flag set.
    // We should fail without sending out a DNS query.
    hints.ai_flags |= AI_NUMERICHOST;
    result = safe_getaddrinfo(host_name, nullptr, &hints);
    EXPECT_TRUE(result == nullptr);
    EXPECT_TRUE(dns.queries().empty());  // Ensure no DNS queries were sent out
}

TEST_F(ResolverTest, GetAddrInfoV6_failing) {
    const char* listen_addr0 = "127.0.0.7";
    const char* listen_addr1 = "127.0.0.8";
    const char* listen_srv = "53";
    const char* host_name = "ohayou.example.com.";
    test::DNSResponder dns0(listen_addr0, listen_srv, 250, ns_rcode::ns_r_servfail);
    test::DNSResponder dns1(listen_addr1, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns0.setResponseProbability(0.0);
    dns0.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::5");
    dns1.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::6");
    ASSERT_TRUE(dns0.startServer());
    ASSERT_TRUE(dns1.startServer());
    std::vector<std::string> servers = { listen_addr0, listen_addr1 };
    // <sample validity in s> <success threshold in percent> <min samples> <max samples>
    int sample_count = 8;
    const std::vector<int> params = { 300, 25, sample_count, sample_count };
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, params));

    // Repeatedly perform resolutions for non-existing domains until MAXNSSAMPLES resolutions have
    // reached the dns0, which is set to fail. No more requests should then arrive at that server
    // for the next sample_lifetime seconds.
    // TODO: This approach is implementation-dependent, change once metrics reporting is available.
    addrinfo hints = {.ai_family = AF_INET6};
    for (int i = 0 ; i < sample_count ; ++i) {
        std::string domain = StringPrintf("nonexistent%d", i);
        ScopedAddrinfo result = safe_getaddrinfo(domain.c_str(), nullptr, &hints);
    }
    // Due to 100% errors for all possible samples, the server should be ignored from now on and
    // only the second one used for all following queries, until NSSAMPLE_VALIDITY is reached.
    dns0.clearQueries();
    dns1.clearQueries();
    ScopedAddrinfo result = safe_getaddrinfo("ohayou", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(0U, GetNumQueries(dns0, host_name));
    EXPECT_EQ(1U, GetNumQueries(dns1, host_name));
}

TEST_F(ResolverTest, GetAddrInfoV6_nonresponsive) {
    const char* listen_addr0 = "127.0.0.7";
    const char* listen_addr1 = "127.0.0.8";
    const char* listen_srv = "53";
    const char* host_name1 = "ohayou.example.com.";
    const char* host_name2 = "ciao.example.com.";

    // dns0 does not respond with 100% probability, while
    // dns1 responds normally, at least initially.
    test::DNSResponder dns0(listen_addr0, listen_srv, 250, static_cast<ns_rcode>(-1));
    test::DNSResponder dns1(listen_addr1, listen_srv, 250, static_cast<ns_rcode>(-1));
    dns0.setResponseProbability(0.0);
    dns0.addMapping(host_name1, ns_type::ns_t_aaaa, "2001:db8::5");
    dns1.addMapping(host_name1, ns_type::ns_t_aaaa, "2001:db8::6");
    dns0.addMapping(host_name2, ns_type::ns_t_aaaa, "2001:db8::5");
    dns1.addMapping(host_name2, ns_type::ns_t_aaaa, "2001:db8::6");
    ASSERT_TRUE(dns0.startServer());
    ASSERT_TRUE(dns1.startServer());
    std::vector<std::string> servers = {listen_addr0, listen_addr1};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    const addrinfo hints = {.ai_family = AF_INET6};

    // dns0 will ignore the request, and we'll fallback to dns1 after the first
    // retry.
    ScopedAddrinfo result = safe_getaddrinfo(host_name1, nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(1U, GetNumQueries(dns0, host_name1));
    EXPECT_EQ(1U, GetNumQueries(dns1, host_name1));

    // Now make dns1 also ignore 100% requests... The resolve should alternate
    // retries between the nameservers and fail after 4 attempts.
    dns1.setResponseProbability(0.0);
    addrinfo* result2 = nullptr;
    EXPECT_EQ(EAI_NODATA, getaddrinfo(host_name2, nullptr, &hints, &result2));
    EXPECT_EQ(nullptr, result2);
    EXPECT_EQ(4U, GetNumQueries(dns0, host_name2));
    EXPECT_EQ(4U, GetNumQueries(dns1, host_name2));
}

TEST_F(ResolverTest, GetAddrInfoV6_concurrent) {
    const char* listen_addr0 = "127.0.0.9";
    const char* listen_addr1 = "127.0.0.10";
    const char* listen_addr2 = "127.0.0.11";
    const char* listen_srv = "53";
    const char* host_name = "konbanha.example.com.";
    test::DNSResponder dns0(listen_addr0, listen_srv, 250, ns_rcode::ns_r_servfail);
    test::DNSResponder dns1(listen_addr1, listen_srv, 250, ns_rcode::ns_r_servfail);
    test::DNSResponder dns2(listen_addr2, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns0.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::5");
    dns1.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::6");
    dns2.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::7");
    ASSERT_TRUE(dns0.startServer());
    ASSERT_TRUE(dns1.startServer());
    ASSERT_TRUE(dns2.startServer());
    const std::vector<std::string> servers = { listen_addr0, listen_addr1, listen_addr2 };
    std::vector<std::thread> threads(10);
    for (std::thread& thread : threads) {
       thread = std::thread([this, &servers]() {
            unsigned delay = arc4random_uniform(1*1000*1000); // <= 1s
            usleep(delay);
            std::vector<std::string> serverSubset;
            for (const auto& server : servers) {
                if (arc4random_uniform(2)) {
                    serverSubset.push_back(server);
                }
            }
            if (serverSubset.empty()) serverSubset = servers;
            ASSERT_TRUE(SetResolversForNetwork(serverSubset, mDefaultSearchDomains,
                    mDefaultParams_Binder));
            addrinfo hints = {.ai_family = AF_INET6};
            addrinfo* result = nullptr;
            int rv = getaddrinfo("konbanha", nullptr, &hints, &result);
            EXPECT_EQ(0, rv) << "error [" << rv << "] " << gai_strerror(rv);
            if (result) {
                freeaddrinfo(result);
                result = nullptr;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

TEST_F(ResolverTest, GetAddrInfoStressTest_Binder_100) {
    const unsigned num_hosts = 100;
    const unsigned num_threads = 100;
    const unsigned num_queries = 100;
    ASSERT_NO_FATAL_FAILURE(RunGetAddrInfoStressTest_Binder(num_hosts, num_threads, num_queries));
}

TEST_F(ResolverTest, GetAddrInfoStressTest_Binder_100000) {
    const unsigned num_hosts = 100000;
    const unsigned num_threads = 100;
    const unsigned num_queries = 100;
    ASSERT_NO_FATAL_FAILURE(RunGetAddrInfoStressTest_Binder(num_hosts, num_threads, num_queries));
}

TEST_F(ResolverTest, EmptySetup) {
    using android::net::INetd;
    std::vector<std::string> servers;
    std::vector<std::string> domains;
    ASSERT_TRUE(SetResolversForNetwork(servers, domains, mDefaultParams_Binder));
    std::vector<std::string> res_servers;
    std::vector<std::string> res_domains;
    std::vector<std::string> res_tls_servers;
    __res_params res_params;
    std::vector<ResolverStats> res_stats;
    ASSERT_TRUE(
            GetResolverInfo(&res_servers, &res_domains, &res_tls_servers, &res_params, &res_stats));
    EXPECT_EQ(0U, res_servers.size());
    EXPECT_EQ(0U, res_domains.size());
    EXPECT_EQ(0U, res_tls_servers.size());
    ASSERT_EQ(static_cast<size_t>(INetd::RESOLVER_PARAMS_COUNT), mDefaultParams_Binder.size());
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_SAMPLE_VALIDITY],
            res_params.sample_validity);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_SUCCESS_THRESHOLD],
            res_params.success_threshold);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_MIN_SAMPLES], res_params.min_samples);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_MAX_SAMPLES], res_params.max_samples);
    EXPECT_EQ(mDefaultParams_Binder[INetd::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC],
              res_params.base_timeout_msec);
}

TEST_F(ResolverTest, SearchPathChange) {
    const char* listen_addr = "127.0.0.13";
    const char* listen_srv = "53";
    const char* host_name1 = "test13.domain1.org.";
    const char* host_name2 = "test13.domain2.org.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name1, ns_type::ns_t_aaaa, "2001:db8::13");
    dns.addMapping(host_name2, ns_type::ns_t_aaaa, "2001:db8::1:13");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };
    std::vector<std::string> domains = { "domain1.org" };
    ASSERT_TRUE(SetResolversForNetwork(servers, domains, mDefaultParams_Binder));

    const addrinfo hints = {.ai_family = AF_INET6};
    ScopedAddrinfo result = safe_getaddrinfo("test13", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(1U, dns.queries().size());
    EXPECT_EQ(1U, GetNumQueries(dns, host_name1));
    EXPECT_EQ("2001:db8::13", ToString(result));

    // Test that changing the domain search path on its own works.
    domains = { "domain2.org" };
    ASSERT_TRUE(SetResolversForNetwork(servers, domains, mDefaultParams_Binder));
    dns.clearQueries();

    result = safe_getaddrinfo("test13", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_EQ(1U, dns.queries().size());
    EXPECT_EQ(1U, GetNumQueries(dns, host_name2));
    EXPECT_EQ("2001:db8::1:13", ToString(result));
}

static std::string base64Encode(const std::vector<uint8_t>& input) {
    size_t out_len;
    EXPECT_EQ(1, EVP_EncodedLength(&out_len, input.size()));
    // out_len includes the trailing NULL.
    uint8_t output_bytes[out_len];
    EXPECT_EQ(out_len - 1, EVP_EncodeBlock(output_bytes, input.data(), input.size()));
    return std::string(reinterpret_cast<char*>(output_bytes));
}

// If we move this function to dns_responder_client, it will complicate the dependency need of
// dns_tls_frontend.h.
static void setupTlsServers(const std::vector<std::string>& servers,
                            std::vector<std::unique_ptr<test::DnsTlsFrontend>>* tls,
                            std::vector<std::string>* fingerprints) {
    const char* listen_udp = "53";
    const char* listen_tls = "853";

    for (const auto& server : servers) {
        auto t = std::make_unique<test::DnsTlsFrontend>(server, listen_tls, server, listen_udp);
        t = std::make_unique<test::DnsTlsFrontend>(server, listen_tls, server, listen_udp);
        t->startServer();
        fingerprints->push_back(base64Encode(t->fingerprint()));
        tls->push_back(std::move(t));
    }
}

static void shutdownTlsServers(std::vector<std::unique_ptr<test::DnsTlsFrontend>>* tls) {
    for (const auto& t : *tls) {
        t->stopServer();
    }
    tls->clear();
}

TEST_F(ResolverTest, MaxServerPrune_Binder) {
    using android::net::INetd;

    std::vector<std::string> domains;
    std::vector<std::unique_ptr<test::DNSResponder>> dns;
    std::vector<std::unique_ptr<test::DnsTlsFrontend>> tls;
    std::vector<std::string> servers;
    std::vector<std::string> fingerprints;
    std::vector<Mapping> mappings;

    for (unsigned i = 0; i < MAXDNSRCH + 1; i++) {
        domains.push_back(StringPrintf("example%u.com", i));
    }
    ASSERT_NO_FATAL_FAILURE(SetupMappings(1, domains, &mappings));
    ASSERT_NO_FATAL_FAILURE(SetupDNSServers(MAXNS + 1, mappings, &dns, &servers));
    ASSERT_NO_FATAL_FAILURE(setupTlsServers(servers, &tls, &fingerprints));

    ASSERT_TRUE(SetResolversWithTls(servers, domains, mDefaultParams_Binder, "", fingerprints));

    std::vector<std::string> res_servers;
    std::vector<std::string> res_domains;
    std::vector<std::string> res_tls_servers;
    __res_params res_params;
    std::vector<ResolverStats> res_stats;
    ASSERT_TRUE(
            GetResolverInfo(&res_servers, &res_domains, &res_tls_servers, &res_params, &res_stats));

    // Check the size of the stats and its contents.
    EXPECT_EQ(static_cast<size_t>(MAXNS), res_servers.size());
    EXPECT_EQ(static_cast<size_t>(MAXNS), res_tls_servers.size());
    EXPECT_EQ(static_cast<size_t>(MAXDNSRCH), res_domains.size());
    EXPECT_TRUE(std::equal(servers.begin(), servers.begin() + MAXNS, res_servers.begin()));
    EXPECT_TRUE(std::equal(servers.begin(), servers.begin() + MAXNS, res_tls_servers.begin()));
    EXPECT_TRUE(std::equal(domains.begin(), domains.begin() + MAXDNSRCH, res_domains.begin()));

    ASSERT_NO_FATAL_FAILURE(ShutdownDNSServers(&dns));
    ASSERT_NO_FATAL_FAILURE(shutdownTlsServers(&tls));
}

TEST_F(ResolverTest, ResolverStats) {
    const char* listen_addr1 = "127.0.0.4";
    const char* listen_addr2 = "127.0.0.5";
    const char* listen_addr3 = "127.0.0.6";
    const char* listen_srv = "53";
    const char* host_name = "hello.example.com.";

    // Set server 1 timeout.
    test::DNSResponder dns1(listen_addr1, listen_srv, 250, static_cast<ns_rcode>(-1));
    dns1.setResponseProbability(0.0);
    ASSERT_TRUE(dns1.startServer());

    // Set server 2 responding server failure.
    test::DNSResponder dns2(listen_addr2, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns2.setResponseProbability(0.0);
    ASSERT_TRUE(dns2.startServer());

    // Set server 3 workable.
    test::DNSResponder dns3(listen_addr3, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns3.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns3.startServer());

    std::vector<std::string> servers = {listen_addr1, listen_addr2, listen_addr3};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    dns3.clearQueries();
    addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    ScopedAddrinfo result = safe_getaddrinfo("hello", nullptr, &hints);
    size_t found = GetNumQueries(dns3, host_name);
    EXPECT_LE(1U, found);
    std::string result_str = ToString(result);
    EXPECT_TRUE(result_str == "1.2.3.4") << ", result_str='" << result_str << "'";

    std::vector<std::string> res_servers;
    std::vector<std::string> res_domains;
    std::vector<std::string> res_tls_servers;
    __res_params res_params;
    std::vector<ResolverStats> res_stats;
    ASSERT_TRUE(
            GetResolverInfo(&res_servers, &res_domains, &res_tls_servers, &res_params, &res_stats));

    EXPECT_EQ(1, res_stats[0].timeouts);
    EXPECT_EQ(1, res_stats[1].errors);
    EXPECT_EQ(1, res_stats[2].successes);

    dns1.stopServer();
    dns2.stopServer();
    dns3.stopServer();
}

// Test what happens if the specified TLS server is nonexistent.
TEST_F(ResolverTest, GetHostByName_TlsMissing) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_srv = "53";
    const char* host_name = "tlsmissing.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.3");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    // There's nothing listening on this address, so validation will either fail or
    /// hang.  Either way, queries will continue to flow to the DNSResponder.
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "", {}));

    const hostent* result;

    result = gethostbyname("tlsmissing");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.3", ToString(result));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    dns.stopServer();
}

// Test what happens if the specified TLS server replies with garbage.
TEST_F(ResolverTest, GetHostByName_TlsBroken) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_srv = "53";
    const char* host_name1 = "tlsbroken1.example.com.";
    const char* host_name2 = "tlsbroken2.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name1, ns_type::ns_t_a, "1.2.3.1");
    dns.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.2");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    // Bind the specified private DNS socket but don't respond to any client sockets yet.
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    ASSERT_TRUE(s >= 0);
    struct sockaddr_in tlsServer = {
        .sin_family = AF_INET,
        .sin_port = htons(853),
    };
    ASSERT_TRUE(inet_pton(AF_INET, listen_addr, &tlsServer.sin_addr));
    ASSERT_TRUE(enableSockopt(s, SOL_SOCKET, SO_REUSEPORT).ok());
    ASSERT_TRUE(enableSockopt(s, SOL_SOCKET, SO_REUSEADDR).ok());
    ASSERT_FALSE(bind(s, reinterpret_cast<struct sockaddr*>(&tlsServer), sizeof(tlsServer)));
    ASSERT_FALSE(listen(s, 1));

    // Trigger TLS validation.
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "", {}));

    struct sockaddr_storage cliaddr;
    socklen_t sin_size = sizeof(cliaddr);
    int new_fd = accept4(s, reinterpret_cast<struct sockaddr *>(&cliaddr), &sin_size, SOCK_CLOEXEC);
    ASSERT_TRUE(new_fd > 0);

    // We've received the new file descriptor but not written to it or closed, so the
    // validation is still pending.  Queries should still flow correctly because the
    // server is not used until validation succeeds.
    const hostent* result;
    result = gethostbyname("tlsbroken1");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.1", ToString(result));

    // Now we cause the validation to fail.
    std::string garbage = "definitely not a valid TLS ServerHello";
    write(new_fd, garbage.data(), garbage.size());
    close(new_fd);

    // Validation failure shouldn't interfere with lookups, because lookups won't be sent
    // to the TLS server unless validation succeeds.
    result = gethostbyname("tlsbroken2");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.2", ToString(result));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    dns.stopServer();
    close(s);
}

TEST_F(ResolverTest, GetHostByName_Tls) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name1 = "tls1.example.com.";
    const char* host_name2 = "tls2.example.com.";
    const char* host_name3 = "tls3.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name1, ns_type::ns_t_a, "1.2.3.1");
    dns.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.2");
    dns.addMapping(host_name3, ns_type::ns_t_a, "1.2.3.3");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "", {}));

    const hostent* result;

    // Wait for validation to complete.
    EXPECT_TRUE(tls.waitForQueries(1, 5000));

    result = gethostbyname("tls1");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.1", ToString(result));

    // Wait for query to get counted.
    EXPECT_TRUE(tls.waitForQueries(2, 5000));

    // Stop the TLS server.  Since we're in opportunistic mode, queries will
    // fall back to the locally-assigned (clear text) nameservers.
    tls.stopServer();

    dns.clearQueries();
    result = gethostbyname("tls2");
    EXPECT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.2", ToString(result));
    const auto queries = dns.queries();
    EXPECT_EQ(1U, queries.size());
    EXPECT_EQ("tls2.example.com.", queries[0].first);
    EXPECT_EQ(ns_t_a, queries[0].second);

    // Reset the resolvers without enabling TLS.  Queries should still be routed
    // to the UDP endpoint.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));

    result = gethostbyname("tls3");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.3", ToString(result));

    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_TlsFingerprint) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());
    for (int chain_length = 1; chain_length <= 3; ++chain_length) {
        std::string host_name = StringPrintf("tlsfingerprint%d.example.com.", chain_length);
        dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.1");
        std::vector<std::string> servers = { listen_addr };

        test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
        tls.set_chain_length(chain_length);
        ASSERT_TRUE(tls.startServer());
        ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
                { base64Encode(tls.fingerprint()) }));

        const hostent* result;

        // Wait for validation to complete.
        EXPECT_TRUE(tls.waitForQueries(1, 5000));

        result = gethostbyname(StringPrintf("tlsfingerprint%d", chain_length).c_str());
        EXPECT_FALSE(result == nullptr);
        if (result) {
            EXPECT_EQ("1.2.3.1", ToString(result));

            // Wait for query to get counted.
            EXPECT_TRUE(tls.waitForQueries(2, 5000));
        }

        // Clear TLS bit to ensure revalidation.
        ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
        tls.stopServer();
    }
    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_BadTlsFingerprint) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name = "badtlsfingerprint.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.1");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    std::vector<uint8_t> bad_fingerprint = tls.fingerprint();
    bad_fingerprint[5] += 1;  // Corrupt the fingerprint.
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
            { base64Encode(bad_fingerprint) }));

    // The initial validation should fail at the fingerprint check before
    // issuing a query.
    EXPECT_FALSE(tls.waitForQueries(1, 500));

    // A fingerprint was provided and failed to match, so the query should fail.
    EXPECT_EQ(nullptr, gethostbyname("badtlsfingerprint"));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls.stopServer();
    dns.stopServer();
}

// Test that we can pass two different fingerprints, and connection succeeds as long as
// at least one of them matches the server.
TEST_F(ResolverTest, GetHostByName_TwoTlsFingerprints) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name = "twotlsfingerprints.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.1");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    std::vector<uint8_t> bad_fingerprint = tls.fingerprint();
    bad_fingerprint[5] += 1;  // Corrupt the fingerprint.
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
            { base64Encode(bad_fingerprint), base64Encode(tls.fingerprint()) }));

    const hostent* result;

    // Wait for validation to complete.
    EXPECT_TRUE(tls.waitForQueries(1, 5000));

    result = gethostbyname("twotlsfingerprints");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.1", ToString(result));

    // Wait for query to get counted.
    EXPECT_TRUE(tls.waitForQueries(2, 5000));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls.stopServer();
    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_TlsFingerprintGoesBad) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name1 = "tlsfingerprintgoesbad1.example.com.";
    const char* host_name2 = "tlsfingerprintgoesbad2.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name1, ns_type::ns_t_a, "1.2.3.1");
    dns.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.2");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
            { base64Encode(tls.fingerprint()) }));

    const hostent* result;

    // Wait for validation to complete.
    EXPECT_TRUE(tls.waitForQueries(1, 5000));

    result = gethostbyname("tlsfingerprintgoesbad1");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.1", ToString(result));

    // Wait for query to get counted.
    EXPECT_TRUE(tls.waitForQueries(2, 5000));

    // Restart the TLS server.  This will generate a new certificate whose fingerprint
    // no longer matches the stored fingerprint.
    tls.stopServer();
    tls.startServer();

    result = gethostbyname("tlsfingerprintgoesbad2");
    ASSERT_TRUE(result == nullptr);
    EXPECT_EQ(HOST_NOT_FOUND, h_errno);

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls.stopServer();
    dns.stopServer();
}

TEST_F(ResolverTest, GetHostByName_TlsFailover) {
    const char* listen_addr1 = "127.0.0.3";
    const char* listen_addr2 = "127.0.0.4";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name1 = "tlsfailover1.example.com.";
    const char* host_name2 = "tlsfailover2.example.com.";
    test::DNSResponder dns1(listen_addr1, listen_udp, 250, ns_rcode::ns_r_servfail);
    test::DNSResponder dns2(listen_addr2, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns1.addMapping(host_name1, ns_type::ns_t_a, "1.2.3.1");
    dns1.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.2");
    dns2.addMapping(host_name1, ns_type::ns_t_a, "1.2.3.3");
    dns2.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns1.startServer());
    ASSERT_TRUE(dns2.startServer());
    std::vector<std::string> servers = { listen_addr1, listen_addr2 };

    test::DnsTlsFrontend tls1(listen_addr1, listen_tls, listen_addr1, listen_udp);
    test::DnsTlsFrontend tls2(listen_addr2, listen_tls, listen_addr2, listen_udp);
    ASSERT_TRUE(tls1.startServer());
    ASSERT_TRUE(tls2.startServer());
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
            { base64Encode(tls1.fingerprint()), base64Encode(tls2.fingerprint()) }));

    const hostent* result;

    // Wait for validation to complete.
    EXPECT_TRUE(tls1.waitForQueries(1, 5000));
    EXPECT_TRUE(tls2.waitForQueries(1, 5000));

    result = gethostbyname("tlsfailover1");
    ASSERT_FALSE(result == nullptr);
    EXPECT_EQ("1.2.3.1", ToString(result));

    // Wait for query to get counted.
    EXPECT_TRUE(tls1.waitForQueries(2, 5000));
    // No new queries should have reached tls2.
    EXPECT_EQ(1, tls2.queries());

    // Stop tls1.  Subsequent queries should attempt to reach tls1, fail, and retry to tls2.
    tls1.stopServer();

    result = gethostbyname("tlsfailover2");
    EXPECT_EQ("1.2.3.4", ToString(result));

    // Wait for query to get counted.
    EXPECT_TRUE(tls2.waitForQueries(2, 5000));

    // No additional queries should have reached the insecure servers.
    EXPECT_EQ(2U, dns1.queries().size());
    EXPECT_EQ(2U, dns2.queries().size());

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls2.stopServer();
    dns1.stopServer();
    dns2.stopServer();
}

TEST_F(ResolverTest, GetHostByName_BadTlsName) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name = "badtlsname.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.1");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder,
            "www.example.com", {}));

    // The TLS server's certificate doesn't chain to a known CA, and a nonempty name was specified,
    // so the client should fail the TLS handshake before ever issuing a query.
    EXPECT_FALSE(tls.waitForQueries(1, 500));

    // The query should fail hard, because a name was specified.
    EXPECT_EQ(nullptr, gethostbyname("badtlsname"));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls.stopServer();
    dns.stopServer();
}

TEST_F(ResolverTest, GetAddrInfo_Tls) {
    const char* listen_addr = "127.0.0.3";
    const char* listen_udp = "53";
    const char* listen_tls = "853";
    const char* host_name = "addrinfotls.example.com.";
    test::DNSResponder dns(listen_addr, listen_udp, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = { listen_addr };

    test::DnsTlsFrontend tls(listen_addr, listen_tls, listen_addr, listen_udp);
    ASSERT_TRUE(tls.startServer());
    ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
            { base64Encode(tls.fingerprint()) }));

    // Wait for validation to complete.
    EXPECT_TRUE(tls.waitForQueries(1, 5000));

    dns.clearQueries();
    ScopedAddrinfo result = safe_getaddrinfo("addrinfotls", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    size_t found = GetNumQueries(dns, host_name);
    EXPECT_LE(1U, found);
    // Could be A or AAAA
    std::string result_str = ToString(result);
    EXPECT_TRUE(result_str == "1.2.3.4" || result_str == "::1.2.3.4")
        << ", result_str='" << result_str << "'";
    // Wait for both A and AAAA queries to get counted.
    EXPECT_TRUE(tls.waitForQueries(3, 5000));

    // Clear TLS bit.
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains,  mDefaultParams_Binder));
    tls.stopServer();
    dns.stopServer();
}

TEST_F(ResolverTest, TlsBypass) {
    const char OFF[] = "off";
    const char OPPORTUNISTIC[] = "opportunistic";
    const char STRICT[] = "strict";

    const char GETHOSTBYNAME[] = "gethostbyname";
    const char GETADDRINFO[] = "getaddrinfo";
    const char GETADDRINFOFORNET[] = "getaddrinfofornet";

    const unsigned BYPASS_NETID = NETID_USE_LOCAL_NAMESERVERS | TEST_NETID;

    const std::vector<uint8_t> NOOP_FINGERPRINT(SHA256_SIZE, 0U);

    const char ADDR4[] = "192.0.2.1";
    const char ADDR6[] = "2001:db8::1";

    const char cleartext_addr[] = "127.0.0.53";
    const char cleartext_port[] = "53";
    const char tls_port[] = "853";
    const std::vector<std::string> servers = { cleartext_addr };

    test::DNSResponder dns(cleartext_addr, cleartext_port, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());

    test::DnsTlsFrontend tls(cleartext_addr, tls_port, cleartext_addr, cleartext_port);

    struct TestConfig {
        const std::string mode;
        const bool withWorkingTLS;
        const std::string method;

        std::string asHostName() const {
            return StringPrintf("%s.%s.%s.",
                                mode.c_str(),
                                withWorkingTLS ? "tlsOn" : "tlsOff",
                                method.c_str());
        }
    } testConfigs[]{
        {OFF,           false, GETHOSTBYNAME},
        {OPPORTUNISTIC, false, GETHOSTBYNAME},
        {STRICT,        false, GETHOSTBYNAME},
        {OFF,           true,  GETHOSTBYNAME},
        {OPPORTUNISTIC, true,  GETHOSTBYNAME},
        {STRICT,        true,  GETHOSTBYNAME},
        {OFF,           false, GETADDRINFO},
        {OPPORTUNISTIC, false, GETADDRINFO},
        {STRICT,        false, GETADDRINFO},
        {OFF,           true,  GETADDRINFO},
        {OPPORTUNISTIC, true,  GETADDRINFO},
        {STRICT,        true,  GETADDRINFO},
        {OFF,           false, GETADDRINFOFORNET},
        {OPPORTUNISTIC, false, GETADDRINFOFORNET},
        {STRICT,        false, GETADDRINFOFORNET},
        {OFF,           true,  GETADDRINFOFORNET},
        {OPPORTUNISTIC, true,  GETADDRINFOFORNET},
        {STRICT,        true,  GETADDRINFOFORNET},
    };

    for (const auto& config : testConfigs) {
        const std::string testHostName = config.asHostName();
        SCOPED_TRACE(testHostName);

        // Don't tempt test bugs due to caching.
        const char* host_name = testHostName.c_str();
        dns.addMapping(host_name, ns_type::ns_t_a, ADDR4);
        dns.addMapping(host_name, ns_type::ns_t_aaaa, ADDR6);

        if (config.withWorkingTLS) ASSERT_TRUE(tls.startServer());

        if (config.mode == OFF) {
            ASSERT_TRUE(SetResolversForNetwork(
                    servers, mDefaultSearchDomains,  mDefaultParams_Binder));
        } else if (config.mode == OPPORTUNISTIC) {
            ASSERT_TRUE(SetResolversWithTls(
                    servers, mDefaultSearchDomains, mDefaultParams_Binder, "", {}));
            // Wait for validation to complete.
            if (config.withWorkingTLS) EXPECT_TRUE(tls.waitForQueries(1, 5000));
        } else if (config.mode == STRICT) {
            // We use the existence of fingerprints to trigger strict mode,
            // rather than hostname validation.
            const auto& fingerprint =
                    (config.withWorkingTLS) ? tls.fingerprint() : NOOP_FINGERPRINT;
            ASSERT_TRUE(SetResolversWithTls(
                    servers, mDefaultSearchDomains, mDefaultParams_Binder, "",
                    { base64Encode(fingerprint) }));
            // Wait for validation to complete.
            if (config.withWorkingTLS) EXPECT_TRUE(tls.waitForQueries(1, 5000));
        } else {
            FAIL() << "Unsupported Private DNS mode: " << config.mode;
        }

        const int tlsQueriesBefore = tls.queries();

        const hostent* h_result = nullptr;
        ScopedAddrinfo ai_result;

        if (config.method == GETHOSTBYNAME) {
            ASSERT_EQ(0, setNetworkForResolv(BYPASS_NETID));
            h_result = gethostbyname(host_name);

            EXPECT_EQ(1U, GetNumQueriesForType(dns, ns_type::ns_t_a, host_name));
            ASSERT_FALSE(h_result == nullptr);
            ASSERT_EQ(4, h_result->h_length);
            ASSERT_FALSE(h_result->h_addr_list[0] == nullptr);
            EXPECT_EQ(ADDR4, ToString(h_result));
            EXPECT_TRUE(h_result->h_addr_list[1] == nullptr);
        } else if (config.method == GETADDRINFO) {
            ASSERT_EQ(0, setNetworkForResolv(BYPASS_NETID));
            ai_result = safe_getaddrinfo(host_name, nullptr, nullptr);
            EXPECT_TRUE(ai_result != nullptr);

            EXPECT_LE(1U, GetNumQueries(dns, host_name));
            // Could be A or AAAA
            const std::string result_str = ToString(ai_result);
            EXPECT_TRUE(result_str == ADDR4 || result_str == ADDR6)
                << ", result_str='" << result_str << "'";
        } else if (config.method == GETADDRINFOFORNET) {
            addrinfo* raw_ai_result = nullptr;
            EXPECT_EQ(0, android_getaddrinfofornet(host_name, /*servname=*/nullptr,
                                                   /*hints=*/nullptr, BYPASS_NETID, MARK_UNSET,
                                                   &raw_ai_result));
            ai_result.reset(raw_ai_result);

            EXPECT_LE(1U, GetNumQueries(dns, host_name));
            // Could be A or AAAA
            const std::string result_str = ToString(ai_result);
            EXPECT_TRUE(result_str == ADDR4 || result_str == ADDR6)
                << ", result_str='" << result_str << "'";
        } else {
            FAIL() << "Unsupported query method: " << config.method;
        }

        const int tlsQueriesAfter = tls.queries();
        EXPECT_EQ(0, tlsQueriesAfter - tlsQueriesBefore);

        // Clear per-process resolv netid.
        ASSERT_EQ(0, setNetworkForResolv(NETID_UNSET));
        tls.stopServer();
        dns.clearQueries();
    }

    dns.stopServer();
}

TEST_F(ResolverTest, StrictMode_NoTlsServers) {
    const std::vector<uint8_t> NOOP_FINGERPRINT(SHA256_SIZE, 0U);
    const char cleartext_addr[] = "127.0.0.53";
    const char cleartext_port[] = "53";
    const std::vector<std::string> servers = { cleartext_addr };

    test::DNSResponder dns(cleartext_addr, cleartext_port, 250, ns_rcode::ns_r_servfail);
    const char* host_name = "strictmode.notlsips.example.com.";
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    ASSERT_TRUE(SetResolversWithTls(
            servers, mDefaultSearchDomains, mDefaultParams_Binder,
            {}, "", { base64Encode(NOOP_FINGERPRINT) }));

    addrinfo* ai_result = nullptr;
    EXPECT_NE(0, getaddrinfo(host_name, nullptr, nullptr, &ai_result));
    EXPECT_EQ(0U, GetNumQueries(dns, host_name));
}

namespace {

int getAsyncResponse(int fd, int* rcode, u_char* buf, int bufLen) {
    struct pollfd wait_fd[1];
    wait_fd[0].fd = fd;
    wait_fd[0].events = POLLIN;
    short revents;
    int ret;

    ret = poll(wait_fd, 1, -1);
    revents = wait_fd[0].revents;
    if (revents & POLLIN) {
        int n = resNetworkResult(fd, rcode, buf, bufLen);
        return n;
    }
    return -1;
}

std::string toString(u_char* buf, int bufLen, int ipType) {
    ns_msg handle;
    int ancount, n = 0;
    ns_rr rr;

    if (ns_initparse((const uint8_t*) buf, bufLen, &handle) >= 0) {
        ancount = ns_msg_count(handle, ns_s_an);
        if (ns_parserr(&handle, ns_s_an, n, &rr) == 0) {
            const u_char* rdata = ns_rr_rdata(rr);
            char buffer[INET6_ADDRSTRLEN];
            if (inet_ntop(ipType, (const char*) rdata, buffer, sizeof(buffer))) {
                return buffer;
            }
        }
    }
    return "";
}

int dns_open_proxy() {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s == -1) {
        return -1;
    }
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    static const struct sockaddr_un proxy_addr = {
            .sun_family = AF_UNIX,
            .sun_path = "/dev/socket/dnsproxyd",
    };

    if (TEMP_FAILURE_RETRY(connect(s, (const struct sockaddr*) &proxy_addr, sizeof(proxy_addr))) !=
        0) {
        close(s);
        return -1;
    }

    return s;
}

}  // namespace

TEST_F(ResolverTest, Async_NormalQueryV4V6) {
    const char listen_addr[] = "127.0.0.4";
    const char listen_srv[] = "53";
    const char host_name[] = "howdy.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    int fd1 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 1);   // Type A       1
    int fd2 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 28);  // Type AAAA    28
    EXPECT_TRUE(fd1 != -1);
    EXPECT_TRUE(fd2 != -1);

    u_char buf[MAXPACKET] = {};
    int rcode;
    int res = getAsyncResponse(fd2, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("::1.2.3.4", toString(buf, res, AF_INET6));

    res = getAsyncResponse(fd1, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("1.2.3.4", toString(buf, res, AF_INET));

    EXPECT_EQ(2U, GetNumQueries(dns, host_name));

    // Re-query verify cache works
    fd1 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 1);   // Type A       1
    fd2 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 28);  // Type AAAA    28

    EXPECT_TRUE(fd1 != -1);
    EXPECT_TRUE(fd2 != -1);

    res = getAsyncResponse(fd2, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("::1.2.3.4", toString(buf, res, AF_INET6));

    res = getAsyncResponse(fd1, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("1.2.3.4", toString(buf, res, AF_INET));

    EXPECT_EQ(2U, GetNumQueries(dns, host_name));
}

TEST_F(ResolverTest, Async_BadQuery) {
    const char listen_addr[] = "127.0.0.4";
    const char listen_srv[] = "53";
    const char host_name[] = "howdy.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    static struct {
        int fd;
        const char* dname;
        const int queryType;
        const int expectRcode;
    } kTestData[] = {
            {-1, "", T_AAAA, 0},
            {-1, "as65ass46", T_AAAA, 0},
            {-1, "454564564564", T_AAAA, 0},
            {-1, "h645235", T_A, 0},
            {-1, "www.google.com", T_A, 0},
    };

    for (auto& td : kTestData) {
        SCOPED_TRACE(td.dname);
        td.fd = resNetworkQuery(TEST_NETID, td.dname, 1, td.queryType);
        EXPECT_TRUE(td.fd != -1);
    }

    // dns_responder return empty resp(packet only contains query part) with no error currently
    for (const auto& td : kTestData) {
        u_char buf[MAXPACKET] = {};
        int rcode;
        SCOPED_TRACE(td.dname);
        int res = getAsyncResponse(td.fd, &rcode, buf, MAXPACKET);
        EXPECT_GT(res, 0);
        EXPECT_EQ(rcode, td.expectRcode);
    }
}

TEST_F(ResolverTest, Async_EmptyAnswer) {
    const char listen_addr[] = "127.0.0.4";
    const char listen_srv[] = "53";
    const char host_name[] = "howdy.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, static_cast<ns_rcode>(-1));
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // TODO: Disable retry to make this test explicit.
    auto& cv = dns.getCv();
    auto& cvMutex = dns.getCvMutex();
    int fd1;
    // Wait on the condition variable to ensure that the DNS server has handled our first query.
    {
        std::unique_lock lk(cvMutex);
        // A 1  AAAA 28
        fd1 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 28);
        EXPECT_TRUE(fd1 != -1);
        EXPECT_EQ(std::cv_status::no_timeout, cv.wait_for(lk, std::chrono::seconds(1)));
    }

    dns.setResponseProbability(0.0);

    int fd2 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 1);
    EXPECT_TRUE(fd2 != -1);

    int fd3 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 1);
    EXPECT_TRUE(fd3 != -1);

    uint8_t buf[MAXPACKET] = {};
    int rcode;

    // expect no response
    int res = getAsyncResponse(fd3, &rcode, buf, MAXPACKET);
    EXPECT_EQ(-ETIMEDOUT, res);

    // expect no response
    memset(buf, 0, MAXPACKET);
    res = getAsyncResponse(fd2, &rcode, buf, MAXPACKET);
    EXPECT_EQ(-ETIMEDOUT, res);

    dns.setResponseProbability(1.0);

    int fd4 = resNetworkQuery(TEST_NETID, "howdy.example.com", 1, 1);
    EXPECT_TRUE(fd4 != -1);

    memset(buf, 0, MAXPACKET);
    res = getAsyncResponse(fd4, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("1.2.3.4", toString(buf, res, AF_INET));

    memset(buf, 0, MAXPACKET);
    res = getAsyncResponse(fd1, &rcode, buf, MAXPACKET);
    EXPECT_GT(res, 0);
    EXPECT_EQ("::1.2.3.4", toString(buf, res, AF_INET6));
}

TEST_F(ResolverTest, Async_MalformedQuery) {
    const char listen_addr[] = "127.0.0.4";
    const char listen_srv[] = "53";
    const char host_name[] = "howdy.example.com.";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    int fd = dns_open_proxy();
    EXPECT_TRUE(fd > 0);

    const std::string badMsg = "16-52512#";
    static struct {
        const std::string cmd;
        const int expectErr;
    } kTestData[] = {
            // Less arguement
            {"resnsend " + badMsg + '\0', -EINVAL},
            // Bad netId
            {"resnsend " + badMsg + " badnetId" + '\0', -EINVAL},
            // Bad raw data
            {"resnsend " + badMsg + " " + std::to_string(TEST_NETID) + '\0', -EILSEQ},
    };

    for (unsigned int i = 0; i < std::size(kTestData); i++) {
        auto& td = kTestData[i];
        SCOPED_TRACE(td.cmd);
        ssize_t rc = TEMP_FAILURE_RETRY(write(fd, td.cmd.c_str(), td.cmd.size()));
        EXPECT_EQ(rc, static_cast<ssize_t>(td.cmd.size()));

        int32_t tmp;
        rc = TEMP_FAILURE_RETRY(read(fd, &tmp, sizeof(tmp)));
        EXPECT_TRUE(rc > 0);
        EXPECT_EQ(static_cast<int>(ntohl(tmp)), td.expectErr);
    }
    // Normal query with answer buffer
    // This is raw data of query "howdy.example.com" type 1 class 1
    std::string query = "81sBAAABAAAAAAAABWhvd2R5B2V4YW1wbGUDY29tAAABAAE=";
    std::string cmd = "resnsend " + query + " " + std::to_string(TEST_NETID) + '\0';
    ssize_t rc = TEMP_FAILURE_RETRY(write(fd, cmd.c_str(), cmd.size()));
    EXPECT_EQ(rc, static_cast<ssize_t>(cmd.size()));

    u_char smallBuf[1] = {};
    int rcode;
    rc = getAsyncResponse(fd, &rcode, smallBuf, 1);
    EXPECT_EQ(rc, -EMSGSIZE);

    // Do the normal test with large buffer again
    fd = dns_open_proxy();
    EXPECT_TRUE(fd > 0);
    rc = TEMP_FAILURE_RETRY(write(fd, cmd.c_str(), cmd.size()));
    EXPECT_EQ(rc, static_cast<ssize_t>(cmd.size()));
    u_char buf[MAXPACKET] = {};
    rc = getAsyncResponse(fd, &rcode, buf, MAXPACKET);
    EXPECT_EQ("1.2.3.4", toString(buf, rc, AF_INET));
}

// This test checks that the resolver should not generate the request containing OPT RR when using
// cleartext DNS. If we query the DNS server not supporting EDNS0 and it reponds with FORMERR, we
// will fallback to no EDNS0 and try again. If the server does no response, we won't retry so that
// we get no answer.
TEST_F(ResolverTest, BrokenEdns) {
    typedef test::DNSResponder::Edns Edns;
    enum ExpectResult { EXPECT_FAILURE, EXPECT_SUCCESS };

    const char OFF[] = "off";
    const char OPPORTUNISTIC_UDP[] = "opportunistic_udp";
    const char OPPORTUNISTIC_TLS[] = "opportunistic_tls";
    const char STRICT[] = "strict";
    const char GETHOSTBYNAME[] = "gethostbyname";
    const char GETADDRINFO[] = "getaddrinfo";
    const std::vector<uint8_t> NOOP_FINGERPRINT(SHA256_SIZE, 0U);
    const char ADDR4[] = "192.0.2.1";
    const char CLEARTEXT_ADDR[] = "127.0.0.53";
    const char CLEARTEXT_PORT[] = "53";
    const char TLS_PORT[] = "853";
    const std::vector<std::string> servers = { CLEARTEXT_ADDR };

    test::DNSResponder dns(CLEARTEXT_ADDR, CLEARTEXT_PORT, 250, ns_rcode::ns_r_servfail);
    ASSERT_TRUE(dns.startServer());

    test::DnsTlsFrontend tls(CLEARTEXT_ADDR, TLS_PORT, CLEARTEXT_ADDR, CLEARTEXT_PORT);

    static const struct TestConfig {
        std::string mode;
        std::string method;
        Edns edns;
        ExpectResult expectResult;

        std::string asHostName() const {
            const char* ednsString;
            switch (edns) {
                case Edns::ON:
                    ednsString = "ednsOn";
                    break;
                case Edns::FORMERR:
                    ednsString = "ednsFormerr";
                    break;
                case Edns::DROP:
                    ednsString = "ednsDrop";
                    break;
                default:
                    ednsString = "";
                    break;
            }
            return StringPrintf("%s.%s.%s.", mode.c_str(), method.c_str(), ednsString);
        }
    } testConfigs[] = {
            // In OPPORTUNISTIC_TLS, we get no answer if the DNS server supports TLS but not EDNS0.
            // Could such server exist? if so, we might need to fallback to query cleartext DNS.
            // Another thing is that {OPPORTUNISTIC_TLS, Edns::DROP} and {STRICT, Edns::DROP} are
            // commented out since TLS timeout is not configurable.
            // TODO: Uncomment them after TLS timeout is configurable.
            {OFF,               GETHOSTBYNAME, Edns::ON,      EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETHOSTBYNAME, Edns::ON,      EXPECT_SUCCESS},
            {OPPORTUNISTIC_TLS, GETHOSTBYNAME, Edns::ON,      EXPECT_SUCCESS},
            {STRICT,            GETHOSTBYNAME, Edns::ON,      EXPECT_SUCCESS},
            {OFF,               GETHOSTBYNAME, Edns::FORMERR, EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETHOSTBYNAME, Edns::FORMERR, EXPECT_SUCCESS},
            {OPPORTUNISTIC_TLS, GETHOSTBYNAME, Edns::FORMERR, EXPECT_FAILURE},
            {STRICT,            GETHOSTBYNAME, Edns::FORMERR, EXPECT_FAILURE},
            {OFF,               GETHOSTBYNAME, Edns::DROP,    EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETHOSTBYNAME, Edns::DROP,    EXPECT_SUCCESS},
            //{OPPORTUNISTIC_TLS, GETHOSTBYNAME, Edns::DROP,    EXPECT_FAILURE},
            //{STRICT,            GETHOSTBYNAME, Edns::DROP,    EXPECT_FAILURE},
            {OFF,               GETADDRINFO,   Edns::ON,      EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETADDRINFO,   Edns::ON,      EXPECT_SUCCESS},
            {OPPORTUNISTIC_TLS, GETADDRINFO,   Edns::ON,      EXPECT_SUCCESS},
            {STRICT,            GETADDRINFO,   Edns::ON,      EXPECT_SUCCESS},
            {OFF,               GETADDRINFO,   Edns::FORMERR, EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETADDRINFO,   Edns::FORMERR, EXPECT_SUCCESS},
            {OPPORTUNISTIC_TLS, GETADDRINFO,   Edns::FORMERR, EXPECT_FAILURE},
            {STRICT,            GETADDRINFO,   Edns::FORMERR, EXPECT_FAILURE},
            {OFF,               GETADDRINFO,   Edns::DROP,    EXPECT_SUCCESS},
            {OPPORTUNISTIC_UDP, GETADDRINFO,   Edns::DROP,    EXPECT_SUCCESS},
            //{OPPORTUNISTIC_TLS, GETADDRINFO,   Edns::DROP,   EXPECT_FAILURE},
            //{STRICT,            GETADDRINFO,   Edns::DROP,   EXPECT_FAILURE},
    };

    for (const auto& config : testConfigs) {
        const std::string testHostName = config.asHostName();
        SCOPED_TRACE(testHostName);

        const char* host_name = testHostName.c_str();
        dns.addMapping(host_name, ns_type::ns_t_a, ADDR4);
        dns.setEdns(config.edns);

        if (config.mode == OFF) {
            ASSERT_TRUE(
                    SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
        } else if (config.mode == OPPORTUNISTIC_UDP) {
            ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder,
                                            "", {}));
        } else if (config.mode == OPPORTUNISTIC_TLS) {
            ASSERT_TRUE(tls.startServer());
            ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder,
                                            "", {}));
            // Wait for validation to complete.
            EXPECT_TRUE(tls.waitForQueries(1, 5000));
        } else if (config.mode == STRICT) {
            ASSERT_TRUE(tls.startServer());
            ASSERT_TRUE(SetResolversWithTls(servers, mDefaultSearchDomains, mDefaultParams_Binder,
                                            "", {base64Encode(tls.fingerprint())}));
            // Wait for validation to complete.
            EXPECT_TRUE(tls.waitForQueries(1, 5000));
        }

        if (config.method == GETHOSTBYNAME) {
            const hostent* h_result = gethostbyname(host_name);
            if (config.expectResult == EXPECT_SUCCESS) {
                EXPECT_LE(1U, GetNumQueries(dns, host_name));
                ASSERT_TRUE(h_result != nullptr);
                ASSERT_EQ(4, h_result->h_length);
                ASSERT_FALSE(h_result->h_addr_list[0] == nullptr);
                EXPECT_EQ(ADDR4, ToString(h_result));
                EXPECT_TRUE(h_result->h_addr_list[1] == nullptr);
            } else {
                EXPECT_EQ(0U, GetNumQueriesForType(dns, ns_type::ns_t_a, host_name));
                ASSERT_TRUE(h_result == nullptr);
                ASSERT_EQ(HOST_NOT_FOUND, h_errno);
            }
        } else if (config.method == GETADDRINFO) {
            ScopedAddrinfo ai_result;
            addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
            ai_result = safe_getaddrinfo(host_name, nullptr, &hints);
            if (config.expectResult == EXPECT_SUCCESS) {
                EXPECT_TRUE(ai_result != nullptr);
                EXPECT_EQ(1U, GetNumQueries(dns, host_name));
                const std::string result_str = ToString(ai_result);
                EXPECT_EQ(ADDR4, result_str);
            } else {
                EXPECT_TRUE(ai_result == nullptr);
                EXPECT_EQ(0U, GetNumQueries(dns, host_name));
            }
        } else {
            FAIL() << "Unsupported query method: " << config.method;
        }

        tls.stopServer();
        dns.clearQueries();
    }

    dns.stopServer();
}

TEST_F(ResolverTest, GetAddrInfo_Dns64Synthesize) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_addr2[] = "127.0.0.5";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4only.example.com.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    test::DNSResponder dns2(listen_addr2, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns2.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    ASSERT_TRUE(dns2.startServer());

    std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // hints are necessary in order to let netd know which type of addresses the caller is
    // interested in.
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    ScopedAddrinfo result = safe_getaddrinfo("v4only", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, host_name));

    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");

    // Let's test the case when there's an IPv4 resolver.
    servers = {listen_addr, listen_addr2};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();
    dns2.clearQueries();

    // Netd doesn't detect prefix because there has an IPv4 resolver but all IPv6 resolvers.
    EXPECT_FALSE(WaitForPrefix64Detected(TEST_NETID, 1000));

    result = safe_getaddrinfo("v4only", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, host_name));

    result_str = ToString(result);
    EXPECT_EQ(result_str, "1.2.3.4");
}

// blocked by aosp/816674 which causes wrong error code EAI_FAIL (4) but EAI_NODATA (7).
// TODO: fix aosp/816674 and add testcases GetAddrInfo_Dns64QuerySpecified back.
/*
TEST_F(ResolverTest, GetAddrInfo_Dns64QuerySpecified) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4only.example.com.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Ensure to synthesize AAAA if AF_INET6 is specified, and not to synthesize AAAA
    // in AF_INET case.
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    ScopedAddrinfo result = safe_getaddrinfo("v4only", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");

    hints.ai_family = AF_INET;
    result = safe_getaddrinfo("v4only", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(2U, GetNumQueries(dns, host_name));
    result_str = ToString(result);
    EXPECT_EQ(result_str, "1.2.3.4");
}
*/

TEST_F(ResolverTest, GetAddrInfo_Dns64QueryUnspecifiedV6) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4v6.example.com.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    ScopedAddrinfo result = safe_getaddrinfo("v4v6", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(2U, GetNumQueries(dns, host_name));

    // In AF_UNSPEC case, do not synthesize AAAA if there's at least one AAAA answer.
    std::vector<std::string> result_strs = ToStrings(result);
    for (const auto& str : result_strs) {
        EXPECT_TRUE(str == "1.2.3.4" || str == "2001:db8::102:304")
                << ", result_str='" << str << "'";
    }
}

TEST_F(ResolverTest, GetAddrInfo_Dns64QueryUnspecifiedNoV6) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4v6.example.com.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    ScopedAddrinfo result = safe_getaddrinfo("v4v6", nullptr, &hints);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(2U, GetNumQueries(dns, host_name));

    // In AF_UNSPEC case, synthesize AAAA if there's no AAAA answer.
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");
}

TEST_F(ResolverTest, GetAddrInfo_Dns64QuerySpecialUseIPv4Addresses) {
    constexpr char THIS_NETWORK[] = "this_network";
    constexpr char LOOPBACK[] = "loopback";
    constexpr char LINK_LOCAL[] = "link_local";
    constexpr char MULTICAST[] = "multicast";
    constexpr char LIMITED_BROADCAST[] = "limited_broadcast";

    constexpr char ADDR_THIS_NETWORK[] = "0.0.0.1";
    constexpr char ADDR_LOOPBACK[] = "127.0.0.1";
    constexpr char ADDR_LINK_LOCAL[] = "169.254.0.1";
    constexpr char ADDR_MULTICAST[] = "224.0.0.1";
    constexpr char ADDR_LIMITED_BROADCAST[] = "255.255.255.255";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    static const struct TestConfig {
        std::string name;
        std::string addr;

        std::string asHostName() const { return StringPrintf("%s.example.com.", name.c_str()); }
    } testConfigs[]{
        {THIS_NETWORK,      ADDR_THIS_NETWORK},
        {LOOPBACK,          ADDR_LOOPBACK},
        {LINK_LOCAL,        ADDR_LINK_LOCAL},
        {MULTICAST,         ADDR_MULTICAST},
        {LIMITED_BROADCAST, ADDR_LIMITED_BROADCAST}
    };

    for (const auto& config : testConfigs) {
        const std::string testHostName = config.asHostName();
        SCOPED_TRACE(testHostName);

        const char* host_name = testHostName.c_str();
        dns.addMapping(host_name, ns_type::ns_t_a, config.addr.c_str());

        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        ScopedAddrinfo result = safe_getaddrinfo(config.name.c_str(), nullptr, &hints);
        // In AF_INET6 case, don't return IPv4 answers
        EXPECT_TRUE(result == nullptr);
        EXPECT_LE(2U, GetNumQueries(dns, host_name));
        dns.clearQueries();

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        result = safe_getaddrinfo(config.name.c_str(), nullptr, &hints);
        EXPECT_TRUE(result != nullptr);
        // Expect IPv6 query only. IPv4 answer has been cached in previous query.
        EXPECT_LE(1U, GetNumQueries(dns, host_name));
        // In AF_UNSPEC case, don't synthesize special use IPv4 address.
        std::string result_str = ToString(result);
        EXPECT_EQ(result_str, config.addr.c_str());
        dns.clearQueries();
    }
}

TEST_F(ResolverTest, GetAddrInfo_Dns64QueryWithNullArgumentHints) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4only.example.com.";
    constexpr char host_name2[] = "v4v6.example.com.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name2, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name2, ns_type::ns_t_aaaa, "2001:db8::1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Assign argument hints of getaddrinfo() as null is equivalent to set ai_family AF_UNSPEC.
    // In AF_UNSPEC case, synthesize AAAA if there has A answer only.
    ScopedAddrinfo result = safe_getaddrinfo("v4only", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(2U, GetNumQueries(dns, host_name));
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");
    dns.clearQueries();

    // In AF_UNSPEC case, do not synthesize AAAA if there's at least one AAAA answer.
    result = safe_getaddrinfo("v4v6", nullptr, nullptr);
    EXPECT_TRUE(result != nullptr);
    EXPECT_LE(2U, GetNumQueries(dns, host_name2));
    std::vector<std::string> result_strs = ToStrings(result);
    for (const auto& str : result_strs) {
        EXPECT_TRUE(str == "1.2.3.4" || str == "2001:db8::102:304")
                << ", result_str='" << str << "'";
    }
}

TEST_F(ResolverTest, GetAddrInfo_Dns64QueryNullArgumentNode) {
    constexpr char ADDR_ANYADDR_V4[] = "0.0.0.0";
    constexpr char ADDR_ANYADDR_V6[] = "::";
    constexpr char ADDR_LOCALHOST_V4[] = "127.0.0.1";
    constexpr char ADDR_LOCALHOST_V6[] = "::1";

    constexpr char PORT_NAME_HTTP[] = "http";
    constexpr char PORT_NUMBER_HTTP[] = "80";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // If node is null, return address is listed by libc/getaddrinfo.c as follows.
    // - passive socket -> anyaddr (0.0.0.0 or ::)
    // - non-passive socket -> localhost (127.0.0.1 or ::1)
    static const struct TestConfig {
        int flag;
        std::string addr_v4;
        std::string addr_v6;

        std::string asParameters() const {
            return StringPrintf("flag=%d, addr_v4=%s, addr_v6=%s", flag, addr_v4.c_str(),
                                addr_v6.c_str());
        }
    } testConfigs[]{
        {0 /* non-passive */, ADDR_LOCALHOST_V4, ADDR_LOCALHOST_V6},
        {AI_PASSIVE,          ADDR_ANYADDR_V4,   ADDR_ANYADDR_V6}
    };

    for (const auto& config : testConfigs) {
        SCOPED_TRACE(config.asParameters());

        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;  // any address family
        hints.ai_socktype = 0;        // any type
        hints.ai_protocol = 0;        // any protocol
        hints.ai_flags = config.flag;

        // Assign hostname as null and service as port name.
        ScopedAddrinfo result = safe_getaddrinfo(nullptr, PORT_NAME_HTTP, &hints);
        ASSERT_TRUE(result != nullptr);

        // Can't be synthesized because it should not get into Netd.
        std::vector<std::string> result_strs = ToStrings(result);
        for (const auto& str : result_strs) {
            EXPECT_TRUE(str == config.addr_v4 || str == config.addr_v6)
                    << ", result_str='" << str << "'";
        }

        // Assign hostname as null and service as numeric port number.
        hints.ai_flags = config.flag | AI_NUMERICSERV;
        result = safe_getaddrinfo(nullptr, PORT_NUMBER_HTTP, &hints);
        ASSERT_TRUE(result != nullptr);

        // Can't be synthesized because it should not get into Netd.
        result_strs = ToStrings(result);
        for (const auto& str : result_strs) {
            EXPECT_TRUE(str == config.addr_v4 || str == config.addr_v6)
                    << ", result_str='" << str << "'";
        }
    }
}

TEST_F(ResolverTest, GetHostByAddr_ReverseDnsQueryWithHavingNat64Prefix) {
    struct hostent* result = nullptr;
    struct in_addr v4addr;
    struct in6_addr v6addr;

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char ptr_name[] = "v4v6.example.com.";
    // PTR record for IPv4 address 1.2.3.4
    constexpr char ptr_addr_v4[] = "4.3.2.1.in-addr.arpa.";
    // PTR record for IPv6 address 2001:db8::102:304
    constexpr char ptr_addr_v6[] =
            "4.0.3.0.2.0.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(ptr_addr_v4, ns_type::ns_t_ptr, ptr_name);
    dns.addMapping(ptr_addr_v6, ns_type::ns_t_ptr, ptr_name);
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Reverse IPv4 DNS query. Prefix should have no effect on it.
    inet_pton(AF_INET, "1.2.3.4", &v4addr);
    result = gethostbyaddr(&v4addr, sizeof(v4addr), AF_INET);
    ASSERT_TRUE(result != nullptr);
    std::string result_str = result->h_name ? result->h_name : "null";
    EXPECT_EQ(result_str, "v4v6.example.com");

    // Reverse IPv6 DNS query. Prefix should have no effect on it.
    inet_pton(AF_INET6, "2001:db8::102:304", &v6addr);
    result = gethostbyaddr(&v6addr, sizeof(v6addr), AF_INET6);
    ASSERT_TRUE(result != nullptr);
    result_str = result->h_name ? result->h_name : "null";
    EXPECT_EQ(result_str, "v4v6.example.com");
}

TEST_F(ResolverTest, GetHostByAddr_ReverseDns64Query) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char ptr_name[] = "v4only.example.com.";
    // PTR record for IPv4 address 1.2.3.4
    constexpr char ptr_addr_v4[] = "4.3.2.1.in-addr.arpa.";
    // PTR record for IPv6 address 64:ff9b::1.2.3.4
    constexpr char ptr_addr_v6_nomapping[] =
            "4.0.3.0.2.0.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.b.9.f.f.4.6.0.0.ip6.arpa.";
    constexpr char ptr_name_v6_synthesis[] = "v6synthesis.example.com.";
    // PTR record for IPv6 address 64:ff9b::5.6.7.8
    constexpr char ptr_addr_v6_synthesis[] =
            "8.0.7.0.6.0.5.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.b.9.f.f.4.6.0.0.ip6.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(ptr_addr_v4, ns_type::ns_t_ptr, ptr_name);
    dns.addMapping(ptr_addr_v6_synthesis, ns_type::ns_t_ptr, ptr_name_v6_synthesis);
    // "ptr_addr_v6_nomapping" is not mapped in DNS server
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Synthesized PTR record doesn't exist on DNS server
    // Reverse IPv6 DNS64 query while DNS server doesn't have an answer for synthesized address.
    // After querying synthesized address failed, expect that prefix is removed from IPv6
    // synthesized address and do reverse IPv4 query instead.
    struct in6_addr v6addr;
    inet_pton(AF_INET6, "64:ff9b::1.2.3.4", &v6addr);
    struct hostent* result = gethostbyaddr(&v6addr, sizeof(v6addr), AF_INET6);
    ASSERT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v6_nomapping));  // PTR record not exist
    EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v4));            // PTR record exist
    std::string result_str = result->h_name ? result->h_name : "null";
    EXPECT_EQ(result_str, "v4only.example.com");
    // Check that return address has been mapped from IPv4 to IPv6 address because Netd
    // removes NAT64 prefix and does IPv4 DNS reverse lookup in this case. Then, Netd
    // fakes the return IPv4 address as original queried IPv6 address.
    result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");
    dns.clearQueries();

    // Synthesized PTR record exists on DNS server
    // Reverse IPv6 DNS64 query while DNS server has an answer for synthesized address.
    // Expect to Netd pass through synthesized address for DNS queries.
    inet_pton(AF_INET6, "64:ff9b::5.6.7.8", &v6addr);
    result = gethostbyaddr(&v6addr, sizeof(v6addr), AF_INET6);
    ASSERT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v6_synthesis));
    result_str = result->h_name ? result->h_name : "null";
    EXPECT_EQ(result_str, "v6synthesis.example.com");
}

TEST_F(ResolverTest, GetHostByAddr_ReverseDns64QueryFromHostFile) {
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "localhost";
    // The address is synthesized by prefix64:localhost.
    constexpr char host_addr[] = "64:ff9b::7f00:1";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    ASSERT_TRUE(dns.startServer());
    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Using synthesized "localhost" address to be a trick for resolving host name
    // from host file /etc/hosts and "localhost" is the only name in /etc/hosts. Note that this is
    // not realistic: the code never synthesizes AAAA records for addresses in 127.0.0.0/8.
    struct in6_addr v6addr;
    inet_pton(AF_INET6, host_addr, &v6addr);
    struct hostent* result = gethostbyaddr(&v6addr, sizeof(v6addr), AF_INET6);
    ASSERT_TRUE(result != nullptr);
    // Expect no DNS queries; localhost is resolved via /etc/hosts.
    EXPECT_EQ(0U, GetNumQueries(dns, host_name));

    ASSERT_EQ(sizeof(in6_addr), (unsigned) result->h_length);
    ASSERT_EQ(AF_INET6, result->h_addrtype);
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, host_addr);
    result_str = result->h_name ? result->h_name : "null";
    EXPECT_EQ(result_str, host_name);
}

TEST_F(ResolverTest, GetNameInfo_ReverseDnsQueryWithHavingNat64Prefix) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char ptr_name[] = "v4v6.example.com.";
    // PTR record for IPv4 address 1.2.3.4
    constexpr char ptr_addr_v4[] = "4.3.2.1.in-addr.arpa.";
    // PTR record for IPv6 address 2001:db8::102:304
    constexpr char ptr_addr_v6[] =
            "4.0.3.0.2.0.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.8.b.d.0.1.0.0.2.ip6.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(ptr_addr_v4, ns_type::ns_t_ptr, ptr_name);
    dns.addMapping(ptr_addr_v6, ns_type::ns_t_ptr, ptr_name);
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    static const struct TestConfig {
        int flag;
        int family;
        std::string addr;
        std::string host;

        std::string asParameters() const {
            return StringPrintf("flag=%d, family=%d, addr=%s, host=%s", flag, family, addr.c_str(),
                                host.c_str());
        }
    } testConfigs[]{
        {NI_NAMEREQD,    AF_INET,  "1.2.3.4",           "v4v6.example.com"},
        {NI_NUMERICHOST, AF_INET,  "1.2.3.4",           "1.2.3.4"},
        {0,              AF_INET,  "1.2.3.4",           "v4v6.example.com"},
        {0,              AF_INET,  "5.6.7.8",           "5.6.7.8"},           // unmapped
        {NI_NAMEREQD,    AF_INET6, "2001:db8::102:304", "v4v6.example.com"},
        {NI_NUMERICHOST, AF_INET6, "2001:db8::102:304", "2001:db8::102:304"},
        {0,              AF_INET6, "2001:db8::102:304", "v4v6.example.com"},
        {0,              AF_INET6, "2001:db8::506:708", "2001:db8::506:708"}, // unmapped
    };

    // Reverse IPv4/IPv6 DNS query. Prefix should have no effect on it.
    for (const auto& config : testConfigs) {
        SCOPED_TRACE(config.asParameters());

        int rv;
        char host[NI_MAXHOST];
        struct sockaddr_in sin;
        struct sockaddr_in6 sin6;
        if (config.family == AF_INET) {
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            inet_pton(AF_INET, config.addr.c_str(), &sin.sin_addr);
            rv = getnameinfo((const struct sockaddr*) &sin, sizeof(sin), host, sizeof(host),
                             nullptr, 0, config.flag);
            if (config.flag == NI_NAMEREQD) EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v4));
        } else if (config.family == AF_INET6) {
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            inet_pton(AF_INET6, config.addr.c_str(), &sin6.sin6_addr);
            rv = getnameinfo((const struct sockaddr*) &sin6, sizeof(sin6), host, sizeof(host),
                             nullptr, 0, config.flag);
            if (config.flag == NI_NAMEREQD) EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v6));
        }
        ASSERT_EQ(0, rv);
        std::string result_str = host;
        EXPECT_EQ(result_str, config.host);
        dns.clearQueries();
    }
}

TEST_F(ResolverTest, GetNameInfo_ReverseDns64Query) {
    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char ptr_name[] = "v4only.example.com.";
    // PTR record for IPv4 address 1.2.3.4
    constexpr char ptr_addr_v4[] = "4.3.2.1.in-addr.arpa.";
    // PTR record for IPv6 address 64:ff9b::1.2.3.4
    constexpr char ptr_addr_v6_nomapping[] =
            "4.0.3.0.2.0.1.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.b.9.f.f.4.6.0.0.ip6.arpa.";
    constexpr char ptr_name_v6_synthesis[] = "v6synthesis.example.com.";
    // PTR record for IPv6 address 64:ff9b::5.6.7.8
    constexpr char ptr_addr_v6_synthesis[] =
            "8.0.7.0.6.0.5.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.b.9.f.f.4.6.0.0.ip6.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(ptr_addr_v4, ns_type::ns_t_ptr, ptr_name);
    dns.addMapping(ptr_addr_v6_synthesis, ns_type::ns_t_ptr, ptr_name_v6_synthesis);
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    static const struct TestConfig {
        bool hasSynthesizedPtrRecord;
        int flag;
        std::string addr;
        std::string host;

        std::string asParameters() const {
            return StringPrintf("hasSynthesizedPtrRecord=%d, flag=%d, addr=%s, host=%s",
                                hasSynthesizedPtrRecord, flag, addr.c_str(), host.c_str());
        }
    } testConfigs[]{
        {false, NI_NAMEREQD,    "64:ff9b::102:304", "v4only.example.com"},
        {false, NI_NUMERICHOST, "64:ff9b::102:304", "64:ff9b::102:304"},
        {false, 0,              "64:ff9b::102:304", "v4only.example.com"},
        {true,  NI_NAMEREQD,    "64:ff9b::506:708", "v6synthesis.example.com"},
        {true,  NI_NUMERICHOST, "64:ff9b::506:708", "64:ff9b::506:708"},
        {true,  0,              "64:ff9b::506:708", "v6synthesis.example.com"}
    };

    // hasSynthesizedPtrRecord = false
    //   Synthesized PTR record doesn't exist on DNS server
    //   Reverse IPv6 DNS64 query while DNS server doesn't have an answer for synthesized address.
    //   After querying synthesized address failed, expect that prefix is removed from IPv6
    //   synthesized address and do reverse IPv4 query instead.
    //
    // hasSynthesizedPtrRecord = true
    //   Synthesized PTR record exists on DNS server
    //   Reverse IPv6 DNS64 query while DNS server has an answer for synthesized address.
    //   Expect to just pass through synthesized address for DNS queries.
    for (const auto& config : testConfigs) {
        SCOPED_TRACE(config.asParameters());

        char host[NI_MAXHOST];
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        inet_pton(AF_INET6, config.addr.c_str(), &sin6.sin6_addr);
        int rv = getnameinfo((const struct sockaddr*) &sin6, sizeof(sin6), host, sizeof(host),
                             nullptr, 0, config.flag);
        ASSERT_EQ(0, rv);
        if (config.flag == NI_NAMEREQD) {
            if (config.hasSynthesizedPtrRecord) {
                EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v6_synthesis));
            } else {
                EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v6_nomapping));  // PTR record not exist.
                EXPECT_LE(1U, GetNumQueries(dns, ptr_addr_v4));            // PTR record exist.
            }
        }
        std::string result_str = host;
        EXPECT_EQ(result_str, config.host);
        dns.clearQueries();
    }
}

TEST_F(ResolverTest, GetNameInfo_ReverseDns64QueryFromHostFile) {
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "localhost";
    // The address is synthesized by prefix64:localhost.
    constexpr char host_addr[] = "64:ff9b::7f00:1";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    ASSERT_TRUE(dns.startServer());
    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Using synthesized "localhost" address to be a trick for resolving host name
    // from host file /etc/hosts and "localhost" is the only name in /etc/hosts. Note that this is
    // not realistic: the code never synthesizes AAAA records for addresses in 127.0.0.0/8.
    char host[NI_MAXHOST];
    struct sockaddr_in6 sin6;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, host_addr, &sin6.sin6_addr);
    int rv = getnameinfo((const struct sockaddr*) &sin6, sizeof(sin6), host, sizeof(host), nullptr,
                         0, NI_NAMEREQD);
    ASSERT_EQ(0, rv);
    // Expect no DNS queries; localhost is resolved via /etc/hosts.
    EXPECT_EQ(0U, GetNumQueries(dns, host_name));

    std::string result_str = host;
    EXPECT_EQ(result_str, host_name);
}

// blocked by aosp/816674 which causes wrong error code EAI_FAIL (4) but EAI_NODATA (7).
// TODO:
// 1. fix aosp/816674 and add testcases GetHostByName2_Dns64Synthesize back.
// 2. Manual test gethostbyname2 synthesis for IPv4 address which comes from host file.
/*
TEST_F(ResolverTest, GetHostByName2_Dns64Synthesize) {
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "ipv4only.example.com.";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // Query an IPv4-only hostname. Expect that gets a synthesized address.
    struct hostent* result = gethostbyname2("ipv4only", AF_INET6);
    ASSERT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, host_name));
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "64:ff9b::102:304");
}
*/

TEST_F(ResolverTest, GetHostByName2_DnsQueryWithHavingNat64Prefix) {
    constexpr char dns64_name[] = "ipv4only.arpa.";
    constexpr char host_name[] = "v4v6.example.com.";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::192.0.0.170");
    dns.addMapping(host_name, ns_type::ns_t_a, "1.2.3.4");
    dns.addMapping(host_name, ns_type::ns_t_aaaa, "2001:db8::1.2.3.4");
    ASSERT_TRUE(dns.startServer());
    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    // IPv4 DNS query. Prefix should have no effect on it.
    struct hostent* result = gethostbyname2("v4v6", AF_INET);
    ASSERT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, host_name));
    std::string result_str = ToString(result);
    EXPECT_EQ(result_str, "1.2.3.4");
    dns.clearQueries();

    // IPv6 DNS query. Prefix should have no effect on it.
    result = gethostbyname2("v4v6", AF_INET6);
    ASSERT_TRUE(result != nullptr);
    EXPECT_LE(1U, GetNumQueries(dns, host_name));
    result_str = ToString(result);
    EXPECT_EQ(result_str, "2001:db8::102:304");
}

TEST_F(ResolverTest, GetHostByName2_Dns64QuerySpecialUseIPv4Addresses) {
    constexpr char THIS_NETWORK[] = "this_network";
    constexpr char LOOPBACK[] = "loopback";
    constexpr char LINK_LOCAL[] = "link_local";
    constexpr char MULTICAST[] = "multicast";
    constexpr char LIMITED_BROADCAST[] = "limited_broadcast";

    constexpr char ADDR_THIS_NETWORK[] = "0.0.0.1";
    constexpr char ADDR_LOOPBACK[] = "127.0.0.1";
    constexpr char ADDR_LINK_LOCAL[] = "169.254.0.1";
    constexpr char ADDR_MULTICAST[] = "224.0.0.1";
    constexpr char ADDR_LIMITED_BROADCAST[] = "255.255.255.255";

    constexpr char listen_addr[] = "::1";
    constexpr char listen_srv[] = "53";
    constexpr char dns64_name[] = "ipv4only.arpa.";

    test::DNSResponder dns(listen_addr, listen_srv, 250, ns_rcode::ns_r_servfail);
    dns.addMapping(dns64_name, ns_type::ns_t_aaaa, "64:ff9b::");
    ASSERT_TRUE(dns.startServer());

    const std::vector<std::string> servers = {listen_addr};
    ASSERT_TRUE(SetResolversForNetwork(servers, mDefaultSearchDomains, mDefaultParams_Binder));
    dns.clearQueries();

    // Wait for detecting prefix to complete.
    EXPECT_TRUE(WaitForPrefix64Detected(TEST_NETID, 1000));

    static const struct TestConfig {
        std::string name;
        std::string addr;

        std::string asHostName() const {
            return StringPrintf("%s.example.com.",
                                name.c_str());
        }
    } testConfigs[]{
        {THIS_NETWORK,      ADDR_THIS_NETWORK},
        {LOOPBACK,          ADDR_LOOPBACK},
        {LINK_LOCAL,        ADDR_LINK_LOCAL},
        {MULTICAST,         ADDR_MULTICAST},
        {LIMITED_BROADCAST, ADDR_LIMITED_BROADCAST}
    };

    for (const auto& config : testConfigs) {
        const std::string testHostName = config.asHostName();
        SCOPED_TRACE(testHostName);

        const char* host_name = testHostName.c_str();
        dns.addMapping(host_name, ns_type::ns_t_a, config.addr.c_str());

        struct hostent* result = gethostbyname2(config.name.c_str(), AF_INET6);
        EXPECT_LE(1U, GetNumQueries(dns, host_name));

        // In AF_INET6 case, don't synthesize special use IPv4 address.
        // Expect to have no answer
        EXPECT_EQ(nullptr, result);

        dns.clearQueries();
    }
}