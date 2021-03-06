/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/session/HTTPSessionBase.h>

#include <proxygen/lib/http/codec/HTTP2Codec.h>
#include <proxygen/lib/http/session/HTTPSessionController.h>
#include <proxygen/lib/http/session/HTTPSessionStats.h>

using folly::SocketAddress;
using wangle::TransportInfo;

namespace proxygen {
uint32_t HTTPSessionBase::kDefaultReadBufLimit = 65536;
uint32_t HTTPSessionBase::maxReadBufferSize_ = 4000;
uint32_t HTTPSessionBase::egressBodySizeLimit_ = 4096;
uint32_t HTTPSessionBase::kDefaultWriteBufLimit = 65536;


HTTPSessionBase::HTTPSessionBase(
  const SocketAddress& localAddr,
  const SocketAddress& peerAddr,
  HTTPSessionController* controller,
  const TransportInfo& tinfo,
  InfoCallback* infoCallback,
  std::unique_ptr<HTTPCodec> codec) :
    infoCallback_(infoCallback),
    transportInfo_(tinfo),
    codec_(std::move(codec)),
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    prioritySample_(false),
    h2PrioritiesEnabled_(true),
    exHeadersEnabled_(false) {

  // If we receive IPv4-mapped IPv6 addresses, convert them to IPv4.
  localAddr_.tryConvertToIPv4();
  peerAddr_.tryConvertToIPv4();

  setController(controller);
}

void HTTPSessionBase::runDestroyCallbacks() {
  if (infoCallback_) {
    infoCallback_->onDestroy(*this);
  }
  if (controller_) {
    controller_->detachSession(this);
    controller_ = nullptr;
  }
}

void HTTPSessionBase::onCodecChanged() {
  if (controller_) {
    controller_->onSessionCodecChange(this);
  }

  initCodecHeaderIndexingStrategy();
}

void HTTPSessionBase::initCodecHeaderIndexingStrategy() {
  // Set the header indexing strategy to be employed by the codec if H2
  // This is done here so that the strategy could be dynamic depending on the
  // session
  if (controller_ && isHTTP2CodecProtocol(codec_->getProtocol())) {
    HTTP2Codec* h2Codec = static_cast<HTTP2Codec*>(codec_.getChainEndPtr());
    h2Codec->setHeaderIndexingStrategy(
      controller_->getHeaderIndexingStrategy());
  }
}

bool HTTPSessionBase::onBody(std::unique_ptr<folly::IOBuf> chain, size_t length,
                             uint16_t padding, HTTPTransaction* txn) {
  DestructorGuard dg(this);
  auto oldSize = pendingReadSize_;
  pendingReadSize_ += length + padding;
  txn->onIngressBody(std::move(chain), padding);
  if (oldSize < pendingReadSize_) {
    // Transaction must have buffered something and not called
    // notifyBodyProcessed() on it.
    VLOG(4) << *this << " Enqueued ingress. Ingress buffer uses "
            << pendingReadSize_  << " of "  << readBufLimit_
            << " bytes.";
    if (pendingReadSize_ > readBufLimit_ &&
        oldSize <= readBufLimit_) {
      if (infoCallback_) {
        infoCallback_->onIngressLimitExceeded(*this);
      }
      return true;
    }
  }
  return false;
}

bool HTTPSessionBase::notifyBodyProcessed(uint32_t bytes) {
  CHECK_GE(pendingReadSize_, bytes);
  auto oldSize = pendingReadSize_;
  pendingReadSize_ -= bytes;
  VLOG(4) << *this << " Dequeued " << bytes << " bytes of ingress. "
    << "Ingress buffer uses " << pendingReadSize_  << " of "
    << readBufLimit_ << " bytes.";
  if (oldSize > readBufLimit_ &&
      pendingReadSize_ <= readBufLimit_) {
    return true;
  }
  return false;
}

void HTTPSessionBase::setSessionStats(HTTPSessionStats* stats) {
  sessionStats_ = stats;
  if (byteEventTracker_) {
    byteEventTracker_->setTTLBAStats(stats);
  }
}

void HTTPSessionBase::setByteEventTracker(
  std::shared_ptr<ByteEventTracker> byteEventTracker,
  ByteEventTracker::Callback* cb) {
  if (byteEventTracker && byteEventTracker_) {
    byteEventTracker->absorb(std::move(*byteEventTracker_));
  }
  byteEventTracker_ = byteEventTracker;
  if (byteEventTracker_) {
    byteEventTracker_->setCallback(cb);
    byteEventTracker_->setTTLBAStats(sessionStats_);
  }
}

void
HTTPSessionBase::handleErrorDirectly(HTTPTransaction* txn,
                                     const HTTPException& error) {
  VLOG(4) << *this << " creating direct error handler";
  DCHECK(txn);
  auto handler = getParseErrorHandler(txn, error);
  if (!handler) {
    txn->sendAbort();
    return;
  }
  txn->setHandler(handler);
  if (infoCallback_) {
    infoCallback_->onIngressError(*this, error.getProxygenError());
  }
  txn->onError(error);
}

HTTPTransaction::Handler*
HTTPSessionBase::getParseErrorHandler(HTTPTransaction* txn,
                                      const HTTPException& error) {
  // we encounter an error before we finish reading the ingress headers.
  if (codec_->getTransportDirection() == TransportDirection::UPSTREAM) {
    // do not return the parse error handler for upstreams, since all we
    // can do in that direction is abort.
    return nullptr;
  }
  return controller_->getParseErrorHandler(txn, error, getLocalAddress());
}

void HTTPSessionBase::enableExHeadersSettings() noexcept {
  HTTPSettings* settings = codec_->getEgressSettings();
  if (settings) {
    settings->setSetting(SettingsId::ENABLE_EX_HEADERS, 1);
    exHeadersEnabled_ = true;
  }
}

void HTTPSessionBase::attachToSessionController() {
  auto controllerPtr = getController();
  if (controllerPtr) {
    controllerPtr->attachSession(this);
  }
}

}
