// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "channels.h"
#include "ds/serialized.h"
#include "enclave/rpchandler.h"
#include "nodetypes.h"
#include "tls/tls.h"

#include <algorithm>
#include <fmt/format_header_only.h>

namespace ccf
{
  class NodeToNode
  {
  private:
    NodeId self;
    std::unique_ptr<ChannelManager> channels;
    ringbuffer::WriterPtr to_host;

    void establish_channel(NodeId to)
    {
      // If the channel is not yet established, replace all sent messages with
      // a key exchange message. In the case of raft, this is acceptable since
      // append entries and vote requests are re-sent after a short timeout
      auto signed_public = channels->get_signed_public(to);
      if (!signed_public.has_value())
      {
        return;
      }

      LOG_DEBUG_FMT("node2node channel with {} initiated", to);

      ChannelHeader msg = {ChannelMsg::key_exchange, self};
      to_host->write(
        node_outbound,
        to,
        NodeMsgType::channel_msg,
        msg,
        signed_public.value());
    }

  public:
    NodeToNode(ringbuffer::AbstractWriterFactory& writer_factory_) :
      to_host(writer_factory_.create_writer_to_outside())
    {}

    void initialize(NodeId id, const tls::Pem& network_pkey)
    {
      self = id;
      channels = std::make_unique<ChannelManager>(network_pkey);
    }

    bool is_channel_established(NodeId id, Channel& channel)
    {
      if (channel.get_status() != ChannelStatus::ESTABLISHED)
      {
        establish_channel(id);
        return false;
      }
      return true;
    }

    template <class T>
    bool send_authenticated(
      const NodeMsgType& msg_type, NodeId to, const T& data)
    {
      auto& n2n_channel = channels->get(to);
      if (!is_channel_established(to, n2n_channel))
      {
        return false;
      }

      // The secure channel between self and to has already been established
      GcmHdr hdr;
      n2n_channel.tag(hdr, asCb(data));

      to_host->write(node_outbound, to, msg_type, data, hdr);
      return true;
    }

    template <>
    bool send_authenticated(
      const NodeMsgType& msg_type, NodeId to, const std::vector<uint8_t>& data)
    {
      auto& n2n_channel = channels->get(to);
      if (!is_channel_established(to, n2n_channel))
      {
        return false;
      }

      // The secure channel between self and to has already been established
      GcmHdr hdr;
      n2n_channel.tag(hdr, CBuffer{data.data(), data.size()});

      to_host->write(node_outbound, to, msg_type, data, hdr);
      return true;
    }

    template <class T>
    const T& recv_authenticated(const uint8_t*& data, size_t& size)
    {
      const auto& t = serialized::overlay<T>(data, size);
      const auto& hdr = serialized::overlay<GcmHdr>(data, size);

      auto& n2n_channel = channels->get(t.from_node);

      if (!n2n_channel.verify(hdr, asCb(t)))
      {
        throw std::logic_error(fmt::format(
          "Invalid authenticated node2node message from node {} (size: {})",
          t.from_node,
          size));
      }

      return t;
    }

    template <class T>
    std::vector<uint8_t> validate_authenticated(
      const uint8_t*& data, size_t& size)
    {
      const auto& t = serialized::peek<T>(data, size);
      const auto d = serialized::read(data, size, size - sizeof(GcmHdr));
      const auto& hdr = serialized::overlay<GcmHdr>(data, size);

      auto& n2n_channel = channels->get(t.from_node);

      if (!n2n_channel.verify(hdr, CBuffer{d.data(), d.size()}))
      {
        throw std::logic_error(fmt::format(
          "Invalid authenticated node2node message from node {} (size: {})",
          t.from_node,
          size));
      }

      return d;
    }

    template <class T>
    bool send_encrypted(
      const NodeMsgType& msg_type,
      NodeId to,
      const std::vector<uint8_t>& data,
      const T& msg)
    {
      auto& n2n_channel = channels->get(to);
      if (!is_channel_established(to, n2n_channel))
      {
        return false;
      }

      GcmHdr hdr;
      std::vector<uint8_t> cipher(data.size());
      n2n_channel.encrypt(hdr, asCb(msg), data, cipher);

      to_host->write(node_outbound, to, msg_type, msg, hdr, cipher);

      return true;
    }

    template <class T>
    std::pair<T, std::vector<uint8_t>> recv_encrypted(
      const uint8_t* data, size_t size)
    {
      auto t = serialized::read<T>(data, size);
      const auto& hdr = serialized::overlay<GcmHdr>(data, size);
      std::vector<uint8_t> plain(size);

      auto& n2n_channel = channels->get(t.from_node);
      if (!n2n_channel.decrypt(hdr, asCb(t), {data, size}, plain))
      {
        throw std::logic_error(fmt::format(
          "Invalid authenticated node2node message from node {} (size: {})",
          t.from_node,
          size));
      }

      return std::make_pair(t, plain);
    }

    void process_key_exchange(const uint8_t* data, size_t size)
    {
      // Called on channel target when a key exchange message is received from
      // the initiator
      const auto& ke = serialized::overlay<ChannelHeader>(data, size);

      auto signed_public = channels->get_signed_public(ke.from_node);
      if (!signed_public.has_value())
      {
        // Channel is already established
        return;
      }

      if (!channels->load_peer_signed_public(
            ke.from_node, std::vector<uint8_t>(data, data + size)))
      {
        return;
      }

      ChannelHeader msg = {ChannelMsg::key_exchange_response, self};
      to_host->write(
        node_outbound,
        ke.from_node,
        NodeMsgType::channel_msg,
        msg,
        signed_public.value());
    }

    void complete_key_exchange(const uint8_t* data, size_t size)
    {
      // Called on channel initiator when a key exchange response message is
      // received from the target
      const auto& ke = serialized::overlay<ChannelHeader>(data, size);

      if (!channels->load_peer_signed_public(
            ke.from_node, std::vector<uint8_t>(data, data + size)))
      {
        return;
      }
    }

    void recv_message(const uint8_t* data, size_t size)
    {
      switch (serialized::peek<ChannelMsg>(data, size))
      {
        case key_exchange:
        {
          process_key_exchange(data, size);
          break;
        }

        case key_exchange_response:
        {
          complete_key_exchange(data, size);
          break;
        }

        default:
        {}
        break;
      }
    }
  };
}
