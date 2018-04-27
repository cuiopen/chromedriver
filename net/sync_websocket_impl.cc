// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/chromedriver/net/sync_websocket_impl.h"

#include "base/bind.h"
#include "base/callback.h"
#include "base/location.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/waitable_event.h"
#include "chrome/test/chromedriver/net/timeout.h"
#include "net/base/net_errors.h"
#include "net/url_request/url_request_context_getter.h"
#include "url/gurl.h"

SyncWebSocketImpl::SyncWebSocketImpl(
    net::URLRequestContextGetter* context_getter)
    : core_(new Core(context_getter)) {}

SyncWebSocketImpl::~SyncWebSocketImpl() {}

bool SyncWebSocketImpl::IsConnected() {
  return core_->IsConnected();
}

bool SyncWebSocketImpl::Connect(const GURL& url) {
  return core_->Connect(url);
}

bool SyncWebSocketImpl::Send(const std::string& message) {
  return core_->Send(message);
}

SyncWebSocket::StatusCode SyncWebSocketImpl::ReceiveNextMessage(
    std::string* message, const Timeout& timeout) {
  return core_->ReceiveNextMessage(message, timeout);
}

bool SyncWebSocketImpl::HasNextMessage() {
  return core_->HasNextMessage();
}

SyncWebSocketImpl::Core::Core(net::URLRequestContextGetter* context_getter)
    : context_getter_(context_getter),
      is_connected_(false),
      on_update_event_(&lock_) {}

bool SyncWebSocketImpl::Core::IsConnected() {
  base::AutoLock lock(lock_);
  return is_connected_;
}

bool SyncWebSocketImpl::Core::Connect(const GURL& url) {
  bool success = false;
  base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                            base::WaitableEvent::InitialState::NOT_SIGNALED);
  // Try to connect up to 3 times, with 10 seconds delay in between.
  base::TimeDelta waitTime = base::TimeDelta::FromSeconds(10);
  for (int i = 0; i < 3; i++) {
    context_getter_->GetNetworkTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(&SyncWebSocketImpl::Core::ConnectOnIO, this,
                                  url, &success, &event));
    if (event.TimedWait(waitTime))
      break;
    LOG(WARNING) << "Timed out connecting to Chrome, "
                 << (i < 2 ? "retrying..." : "giving up.");
  }
  return success;
}

bool SyncWebSocketImpl::Core::Send(const std::string& message) {
  bool success = false;
  base::WaitableEvent event(base::WaitableEvent::ResetPolicy::AUTOMATIC,
                            base::WaitableEvent::InitialState::NOT_SIGNALED);
  context_getter_->GetNetworkTaskRunner()->PostTask(
      FROM_HERE, base::BindOnce(&SyncWebSocketImpl::Core::SendOnIO, this,
                                message, &success, &event));
  event.Wait();
  return success;
}

SyncWebSocket::StatusCode SyncWebSocketImpl::Core::ReceiveNextMessage(
    std::string* message,
    const Timeout& timeout) {
  base::AutoLock lock(lock_);
  while (received_queue_.empty() && is_connected_) {
    base::TimeDelta next_wait = timeout.GetRemainingTime();
    if (next_wait <= base::TimeDelta())
      return SyncWebSocket::kTimeout;
    on_update_event_.TimedWait(next_wait);
  }
  if (!is_connected_)
    return SyncWebSocket::kDisconnected;
  *message = received_queue_.front();
  received_queue_.pop_front();
  return SyncWebSocket::kOk;
}

bool SyncWebSocketImpl::Core::HasNextMessage() {
  base::AutoLock lock(lock_);
  return !received_queue_.empty();
}

void SyncWebSocketImpl::Core::OnMessageReceived(const std::string& message) {
  base::AutoLock lock(lock_);
  received_queue_.push_back(message);
  on_update_event_.Signal();
}

void SyncWebSocketImpl::Core::OnClose() {
  base::AutoLock lock(lock_);
  is_connected_ = false;
  on_update_event_.Signal();
}

SyncWebSocketImpl::Core::~Core() { }

void SyncWebSocketImpl::Core::ConnectOnIO(
    const GURL& url,
    bool* success,
    base::WaitableEvent* event) {
  {
    base::AutoLock lock(lock_);
    received_queue_.clear();
  }
  // If this is a retry to connect, there is a chance that the original attempt
  // to connect has succeeded after the retry was initiated, so double check if
  // we are already connected. The is_connected_ flag is only set on the I/O
  // thread, so no additional synchronization is needed to check it here.
  // Note: If is_connected_ is true, both |success| and |event| may point to
  // stale memory, so don't use either parameters before returning.
  if (socket_ && is_connected_)
    return;
  socket_.reset(new WebSocket(url, this));
  socket_->Connect(base::Bind(
      &SyncWebSocketImpl::Core::OnConnectCompletedOnIO,
      this, success, event));
}

void SyncWebSocketImpl::Core::OnConnectCompletedOnIO(
    bool* success,
    base::WaitableEvent* event,
    int error) {
  *success = (error == net::OK);
  if (*success) {
    base::AutoLock lock(lock_);
    is_connected_ = true;
  }
  event->Signal();
}

void SyncWebSocketImpl::Core::SendOnIO(
    const std::string& message,
    bool* success,
    base::WaitableEvent* event) {
  *success = socket_->Send(message);
  event->Signal();
}

void SyncWebSocketImpl::Core::OnDestruct() const {
  scoped_refptr<base::SingleThreadTaskRunner> network_task_runner =
      context_getter_->GetNetworkTaskRunner();
  if (network_task_runner->BelongsToCurrentThread())
    delete this;
  else
    network_task_runner->DeleteSoon(FROM_HERE, this);
}
