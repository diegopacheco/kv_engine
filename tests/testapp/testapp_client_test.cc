/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2015 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "testapp_client_test.h"

MemcachedConnection& TestappClientTest::getConnection() {
    switch (GetParam()) {
    case TransportProtocols::McbpPlain:
        return prepare(connectionMap.getConnection(false, AF_INET));
    case TransportProtocols::McbpIpv6Plain:
        return prepare(connectionMap.getConnection(false, AF_INET6));
    case TransportProtocols::McbpSsl:
        return prepare(connectionMap.getConnection(true, AF_INET));
    case TransportProtocols::McbpIpv6Ssl:
        return prepare(connectionMap.getConnection(true, AF_INET6));
    }
    throw std::logic_error("Unknown transport");
}

void TestappXattrClientTest::setClusterSessionToken(uint64_t nval) {
    auto& conn = getAdminConnection();
    BinprotResponse response;

    conn.executeCommand(BinprotSetControlTokenCommand{nval, token}, response);

    if (!response.isSuccess()) {
        throw ConnectionError("TestappClientTest::setClusterSessionToken",
                              response);
    }
    ASSERT_EQ(nval, ntohll(response.getCas()));

    token = nval;
}

void TestappXattrClientTest::SetUp() {
    TestappTest::SetUp();

    mcd_env->getTestBucket().setXattrEnabled(
            getConnection(),
            bucketName,
            ::testing::get<1>(GetParam()) == XattrSupport::Yes);
    if (::testing::get<1>(GetParam()) == XattrSupport::No) {
        xattrOperationStatus = PROTOCOL_BINARY_RESPONSE_NOT_SUPPORTED;
    }

    document.info.cas = mcbp::cas::Wildcard;
    document.info.flags = 0xcaffee;
    document.info.id = name;
    document.info.expiration = 0;
    document.value = to_string(memcached_cfg, false);
}

MemcachedConnection& TestappXattrClientTest::getConnection() {
    switch (::testing::get<0>(GetParam())) {
    case TransportProtocols::McbpPlain:
        return prepare(connectionMap.getConnection(false, AF_INET));
    case TransportProtocols::McbpIpv6Plain:
        return prepare(connectionMap.getConnection(false, AF_INET6));
    case TransportProtocols::McbpSsl:
        return prepare(connectionMap.getConnection(true, AF_INET));
    case TransportProtocols::McbpIpv6Ssl:
        return prepare(connectionMap.getConnection(true, AF_INET6));
    }
    throw std::logic_error("Unknown transport");
}

void TestappXattrClientTest::createXattr(const std::string& path,
                                         const std::string& value,
                                         bool macro) {
    runCreateXattr(path, value, macro, xattrOperationStatus);
}

ClientJSONSupport TestappXattrClientTest::hasJSONSupport() const {
    return ::testing::get<2>(GetParam());
}

cb::mcbp::Datatype TestappXattrClientTest::expectedJSONDatatype() const {
    return hasJSONSupport() == ClientJSONSupport::Yes ? cb::mcbp::Datatype::JSON
                                                      : cb::mcbp::Datatype::Raw;
}

/**
 * Helper function to check datatype is what we expect for this test config,
 * and if datatype says JSON; validate the value /is/ JSON.
 */
::testing::AssertionResult TestappXattrClientTest::hasCorrectDatatype(
        const Document& doc, cb::mcbp::Datatype expectedType) {
    return hasCorrectDatatype(expectedType,
                              doc.info.datatype,
                              {doc.value.data(), doc.value.size()});
}

::testing::AssertionResult TestappXattrClientTest::hasCorrectDatatype(
        cb::mcbp::Datatype expectedType,
        cb::mcbp::Datatype actualDatatype,
        cb::const_char_buffer value) {
    using namespace mcbp::datatype;
    if (actualDatatype != expectedType) {
        return ::testing::AssertionFailure()
               << "Datatype mismatch - expected:"
               << to_string(protocol_binary_datatype_t(expectedType))
               << " actual:"
               << to_string(protocol_binary_datatype_t(actualDatatype));
    }

    if (actualDatatype == cb::mcbp::Datatype::JSON) {
        if (!isJSON(value)) {
            return ::testing::AssertionFailure()
                   << "JSON validation failed for response data:'"
                   << to_string(value) << "''";
        }
    }
    return ::testing::AssertionSuccess();
}

BinprotSubdocResponse TestappXattrClientTest::getXattr(const std::string& path,
                                                       bool deleted) {
    return runGetXattr(path, deleted, xattrOperationStatus);
}

std::ostream& operator<<(std::ostream& os, const XattrSupport& xattrSupport) {
    os << to_string(xattrSupport);
    return os;
}

std::string to_string(const XattrSupport& xattrSupport) {
#ifdef JETBRAINS_CLION_IDE
    // CLion don't properly parse the output when the
    // output gets written as the string instead of the
    // number. This makes it harder to debug the tests
    // so let's just disable it while we're waiting
    // for them to supply a fix.
    // See https://youtrack.jetbrains.com/issue/CPP-6039
    return std::to_string(static_cast<int>(xattrSupport));
#else
    switch (xattrSupport) {
    case XattrSupport::Yes:
        return "XattrYes";
    case XattrSupport::No:
        return "XattrNo";
    }
    throw std::logic_error("Unknown xattr support");
#endif
}

std::string PrintToStringCombinedName::operator()(
        const ::testing::TestParamInfo<::testing::tuple<TransportProtocols,
                                                        XattrSupport,
                                                        ClientJSONSupport>>&
                info) const {
    std::string rv = to_string(::testing::get<0>(info.param)) + "_" +
                     to_string(::testing::get<1>(info.param)) + "_" +
                     to_string(::testing::get<2>(info.param));
    return rv;
}
