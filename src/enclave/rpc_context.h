// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "http/http_consts.h"
#include "http/ws_consts.h"
#include "node/client_signatures.h"
#include "node/entities.h"

#include <http-parser/http_parser.h>
#include <variant>
#include <vector>

namespace ccf
{
  static_assert(
    static_cast<int>(ws::Verb::WEBSOCKET) <
    static_cast<int>(http_method::HTTP_DELETE));
  /*!
    Extension of http_method including a special "WEBSOCKET" method,
    to allow make_*_endpoint() to be a single uniform interface to define
    handlers for either use cases.

    This may be removed if instead of exposing a single RpcContext, callbacks
    are instead given a specialised WsRpcContext, and make_endpoint becomes
    templated on Verb and specialised on the respective enum types.
  */
  class RESTVerb
  {
  private:
    int verb;

  public:
    RESTVerb(const http_method& hm) : verb(hm) {}
    RESTVerb(const ws::Verb& wv) : verb(wv) {}

    const char* c_str() const
    {
      if (verb == ws::WEBSOCKET)
      {
        return "WEBSOCKET";
      }
      else
      {
        return http_method_str(static_cast<http_method>(verb));
      }
    }

    bool operator<(const RESTVerb& o) const
    {
      return verb < o.verb;
    }

    bool operator!=(const RESTVerb& o) const
    {
      return verb != o.verb;
    }
  };
}

namespace enclave
{
  static constexpr size_t InvalidSessionId = std::numeric_limits<size_t>::max();

  struct SessionContext
  {
    size_t client_session_id = InvalidSessionId;
    std::vector<uint8_t> caller_cert = {}; // DER certificate
    bool is_forwarding = false;

    //
    // Only set in the case of a forwarded RPC
    //
    struct Forwarded
    {
      // Initialised when forwarded context is created
      const size_t client_session_id;
      const ccf::CallerId caller_id;

      Forwarded(size_t client_session_id_, ccf::CallerId caller_id_) :
        client_session_id(client_session_id_),
        caller_id(caller_id_)
      {}
    };
    std::optional<Forwarded> original_caller = std::nullopt;

    // Constructor used for non-forwarded RPC
    SessionContext(
      size_t client_session_id_, const std::vector<uint8_t>& caller_cert_) :
      client_session_id(client_session_id_),
      caller_cert(caller_cert_)
    {}

    // Constructor used for forwarded and PBFT RPC
    SessionContext(
      size_t fwd_session_id_,
      ccf::CallerId caller_id_,
      const std::vector<uint8_t>& caller_cert_ = {}) :
      original_caller(
        std::make_optional<Forwarded>(fwd_session_id_, caller_id_)),
      caller_cert(caller_cert_)
    {}
  };

  class RpcContext
  {
  public:
    std::shared_ptr<SessionContext> session;

    virtual FrameFormat frame_format() const = 0;

    // raw pbft Request
    std::vector<uint8_t> pbft_raw = {};

    bool is_create_request = false;

    RpcContext(std::shared_ptr<SessionContext> s) : session(s) {}

    RpcContext(
      std::shared_ptr<SessionContext> s,
      const std::vector<uint8_t>& pbft_raw_) :
      session(s),
      pbft_raw(pbft_raw_)
    {}

    virtual ~RpcContext() {}

    /// Request details
    virtual size_t get_request_index() const = 0;

    virtual const std::vector<uint8_t>& get_request_body() const = 0;
    virtual const std::string& get_request_query() const = 0;
    virtual const ccf::RESTVerb& get_request_verb() const = 0;

    virtual std::string get_method() const = 0;
    virtual void set_method(const std::string_view& method) = 0;

    virtual std::optional<std::string> get_request_header(
      const std::string_view& name) = 0;

    virtual const std::vector<uint8_t>& get_serialised_request() = 0;
    virtual std::optional<ccf::SignedReq> get_signed_request() = 0;

    /// Response details
    virtual void set_response_body(const std::vector<uint8_t>& body) = 0;
    virtual void set_response_body(std::vector<uint8_t>&& body) = 0;
    virtual void set_response_body(std::string&& body) = 0;

    virtual void set_response_status(int status) = 0;

    virtual void set_seqno(kv::Version) = 0;
    virtual void set_view(kv::Consensus::View) = 0;
    virtual void set_global_commit(kv::Version) = 0;

    virtual void set_response_header(
      const std::string_view& name, const std::string_view& value) = 0;
    virtual void set_response_header(const std::string_view& name, size_t n)
    {
      set_response_header(name, fmt::format("{}", n));
    }

    virtual void set_apply_writes(bool apply) = 0;
    virtual bool should_apply_writes() const = 0;

    virtual std::vector<uint8_t> serialise_response() const = 0;

    virtual std::vector<uint8_t> serialise_error(
      size_t code, const std::string& msg) const = 0;
  };
}
