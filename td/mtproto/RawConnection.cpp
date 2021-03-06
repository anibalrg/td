//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/mtproto/RawConnection.h"

#include "td/mtproto/AuthKey.h"
#include "td/mtproto/Transport.h"

#include "td/utils/format.h"
#include "td/utils/logging.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"

#include <utility>

namespace td {
namespace mtproto {

void RawConnection::send_crypto(const Storer &storer, int64 session_id, int64 salt, const AuthKey &auth_key,
                                uint64 quick_ack_token) {
  mtproto::PacketInfo info;
  info.version = 2;
  info.no_crypto_flag = false;
  info.salt = salt;
  info.session_id = session_id;

  auto packet = BufferWriter{mtproto::Transport::write(storer, auth_key, &info), transport_->max_prepend_size(), 0};
  mtproto::Transport::write(storer, auth_key, &info, packet.as_slice());

  bool use_quick_ack = false;
  if (quick_ack_token != 0 && transport_->support_quick_ack()) {
    auto tmp = quick_ack_to_token_.insert(std::make_pair(info.message_ack, quick_ack_token));
    if (tmp.second) {
      use_quick_ack = true;
    } else {
      LOG(ERROR) << "quick_ack collision " << tag("quick_ack", info.message_ack);
    }
  }

  transport_->write(std::move(packet), use_quick_ack);
}

uint64 RawConnection::send_no_crypto(const Storer &storer) {
  mtproto::PacketInfo info;

  info.no_crypto_flag = true;
  auto packet =
      BufferWriter{mtproto::Transport::write(storer, mtproto::AuthKey(), &info), transport_->max_prepend_size(), 0};
  mtproto::Transport::write(storer, mtproto::AuthKey(), &info, packet.as_slice());
  LOG(INFO) << "Send handshake packet: " << format::as_hex_dump<4>(packet.as_slice());
  transport_->write(std::move(packet), false);
  return info.message_id;
}

Status RawConnection::flush_read(const AuthKey &auth_key, Callback &callback) {
  auto r = socket_fd_.flush_read();
  if (r.is_ok() && stats_callback_) {
    stats_callback_->on_read(r.ok());
  }
  while (transport_->can_read()) {
    BufferSlice packet;
    uint32 quick_ack = 0;
    TRY_RESULT(wait_size, transport_->read_next(&packet, &quick_ack));
    if (wait_size != 0) {
      break;
    }

    if (quick_ack != 0) {
      auto it = quick_ack_to_token_.find(quick_ack);
      if (it == quick_ack_to_token_.end()) {
        LOG(WARNING) << Status::Error(PSLICE() << "Unknown " << tag("quick_ack", quick_ack));
        continue;
        // TODO: return Status::Error(PSLICE() << "Unknown " << tag("quick_ack", quick_ack));
      }
      auto token = it->second;
      quick_ack_to_token_.erase(it);
      callback.on_quick_ack(token);
      continue;
    }

    MutableSlice data = packet.as_slice();
    PacketInfo info;
    info.version = 2;

    int32 error_code = 0;
    TRY_STATUS(mtproto::Transport::read(data, auth_key, &info, &data, &error_code));

    if (error_code) {
      if (error_code == -429) {
        if (stats_callback_) {
          stats_callback_->on_mtproto_error();
        }
        return Status::Error(500, PSLICE() << "Mtproto error: " << error_code);
      }
      if (error_code == -404) {
        return Status::Error(-404, PSLICE() << "Mtproto error: " << error_code);
      }
      return Status::Error(PSLICE() << "Mtproto error: " << error_code);
    }

    // If a packet was successfully decrypted, then it is ok to assume that the connection is alive
    if (!auth_key.empty()) {
      if (stats_callback_) {
        stats_callback_->on_pong();
      }
    }

    TRY_STATUS(callback.on_raw_packet(info, packet.from_slice(data)));
  }
  TRY_STATUS(std::move(r));
  return Status::OK();
}

Status RawConnection::flush_write() {
  TRY_RESULT(size, socket_fd_.flush_write());
  if (size > 0 && stats_callback_) {
    stats_callback_->on_write(size);
  }
  return Status::OK();
}

}  // namespace mtproto
}  // namespace td
