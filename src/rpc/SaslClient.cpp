/********************************************************************
 * Copyright (c) 2013 - 2014, Pivotal Inc.
 * All rights reserved.
 *
 * Author: Zhanwei Wang
 ********************************************************************/
/********************************************************************
 * 2014 -
 * open source under Apache License Version 2.0
 ********************************************************************/
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <cctype>

#include "Exception.h"
#include "ExceptionInternal.h"
#include "SaslClient.h"

#define SASL_SUCCESS 0

namespace Hdfs {
namespace Internal {

SaslClient::SaslClient(const RpcSaslProto_SaslAuth & auth, const Token & token,
                       const std::string & principal) :
    complete(false) {
    int rc;
    ctx = NULL;
    privacy = false;
    integrity = false;
    RpcAuth method = RpcAuth(RpcAuth::ParseMethod(auth.method()));
    rc = gsasl_init(&ctx);

    if (rc != GSASL_OK) {
        THROW(HdfsIOException, "cannot initialize libgsasl");
    }

    switch (method.getMethod()) {
    case AuthMethod::KERBEROS:
        initKerberos(auth, principal);
        break;

    case AuthMethod::TOKEN:
        initDigestMd5(auth, token);
        break;

    default:
        THROW(HdfsIOException, "unknown auth method.");
        break;
    }
}

SaslClient::~SaslClient() {
    if (session != NULL) {
        gsasl_finish(session);
    }

    if (ctx != NULL) {
        gsasl_done(ctx);
    }
}


void SaslClient::initKerberos(const RpcSaslProto_SaslAuth & auth,
                              const std::string & principal) {
    int rc;

    /* Create new authentication session. */
    if ((rc = gsasl_client_start(ctx, auth.mechanism().c_str(), &session)) != GSASL_OK) {
        THROW(HdfsIOException, "Cannot initialize client (%d): %s", rc,
              gsasl_strerror(rc));
    }

    gsasl_property_set(session, GSASL_SERVICE, auth.protocol().c_str());
    gsasl_property_set(session, GSASL_AUTHID, principal.c_str());
    gsasl_property_set(session, GSASL_HOSTNAME, auth.serverid().c_str());

    gsasl_property_set(session, GSASL_QOP, "qop-conf");
    //session->qop |= GSASL_QOP_AUTH;
    //session->qop |= GSASL_QOP_AUTH_INT;
    //session->qop |= GSASL_QOP_AUTH_CONF;
}

std::string Base64Encode(const std::string & in) {
    char * temp;
    size_t len;
    std::string retval;
    int rc = gsasl_base64_to(in.c_str(), in.size(), &temp, &len);

    if (rc != GSASL_OK) {
        throw std::bad_alloc();
    }

    if (temp) {
        retval = temp;
        free(temp);
    }

    if (!temp || retval.length() != len) {
        THROW(HdfsIOException, "SaslClient: Failed to encode string to base64");
    }

    return retval;
}

void SaslClient::initDigestMd5(const RpcSaslProto_SaslAuth & auth,
                               const Token & token) {
    int rc;

    if ((rc = gsasl_client_start(ctx, auth.mechanism().c_str(), &session)) != GSASL_OK) {
        THROW(HdfsIOException, "Cannot initialize client (%d): %s", rc, gsasl_strerror(rc));
    }

    std::string password = Base64Encode(token.getPassword());
    std::string identifier = Base64Encode(token.getIdentifier());
    gsasl_property_set(session, GSASL_PASSWORD, password.c_str());
    gsasl_property_set(session, GSASL_AUTHID, identifier.c_str());
    gsasl_property_set(session, GSASL_HOSTNAME, auth.serverid().c_str());
    gsasl_property_set(session, GSASL_SERVICE, auth.protocol().c_str());

}

int SaslClient::findPreferred(int possible) {
    if (possible & GSASL_QOP_AUTH)
        return GSASL_QOP_AUTH;
    if (possible & GSASL_QOP_AUTH_INT)
        return GSASL_QOP_AUTH_INT;
    if (possible & GSASL_QOP_AUTH_CONF)
        return GSASL_QOP_AUTH_CONF;
    return GSASL_QOP_AUTH;
}

std::string SaslClient::evaluateChallenge(const std::string & challenge) {
    int rc;
    char * output = NULL;
    size_t outputSize;
    std::string retval;

    rc = gsasl_step(session, &challenge[0], challenge.size(), &output,
                    &outputSize);

    if (rc == GSASL_NEEDS_MORE || rc == GSASL_OK) {
        retval.resize(outputSize);
        memcpy(&retval[0], output, outputSize);

        if (output) {
            free(output);
        }
    } else {
        if (output) {
            free(output);
        }

        THROW(AccessControlException, "Failed to evaluate challenge: %s", gsasl_strerror(rc));
    }

    if (rc == GSASL_OK) {
        complete = true;
        if (challenge.length()) {
            int qop = (int)challenge.c_str()[0];
            int preferred = findPreferred(qop);
            if (preferred & GSASL_QOP_AUTH_CONF) {
                privacy = true;
                integrity = true;
            } else if (preferred & GSASL_QOP_AUTH_INT) {
                integrity = true;
            }
            retval = "";
        }
    }

    return retval;
}

std::string SaslClient::encode(const char *input, size_t input_len) {
    std::string result;
    if (!privacy && !integrity) {
        result.resize(input_len);
        memcpy(&result[0], input, input_len);
        return result;
    }
    char *output=NULL;
    size_t output_len;
    int rc = gsasl_encode(session, input, input_len, &output, &output_len);
    if (rc != GSASL_OK) {
        THROW(AccessControlException, "Failed to encode wrapped data: %s", gsasl_strerror(rc));
    }
    if (output_len) {
        result.resize(output_len);
        memcpy(&result[0], output, output_len);
        free(output);
    }

    return result;
}

std::string  SaslClient::decode(const char *input, size_t input_len) {
    std::string result;
    if (!privacy && !integrity) {
        result.resize(input_len);
        memcpy(&result[0], input, input_len);
        return result;
    }
    char *output=NULL;
    size_t output_len;
    int rc = gsasl_decode(session, input, input_len, &output, &output_len);
    if (rc != GSASL_OK) {
        THROW(AccessControlException, "Failed to decode wrapped data: %s", gsasl_strerror(rc));
    }
    if (output_len) {
        result.resize(output_len);
        memcpy(&result[0], output, output_len);
        free(output);
    }

    return result;
}


bool SaslClient::isComplete() {
    return complete;
}

bool SaslClient::isPrivate() {
    return privacy;
}

bool SaslClient::isIntegrity() {
    return integrity;
}

}
}

