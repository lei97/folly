/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <folly/io/SocketOptionMap.h>

#include <folly/Function.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/io/IOBuf.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/AsyncSocketBase.h>
#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventHandler.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetOpsDispatcher.h>
#include <folly/net/NetworkSocket.h>

namespace folly {

/**
 * UDP socket
 */
class AsyncUDPSocket : public EventHandler {
 public:
  enum class FDOwnership { OWNS, SHARED };

  AsyncUDPSocket(const AsyncUDPSocket&) = delete;
  AsyncUDPSocket& operator=(const AsyncUDPSocket&) = delete;

  class ReadCallback {
   public:
    struct OnDataAvailableParams {
      int gro = -1;
      // RX timestamp if available
      using Timestamp = std::array<struct timespec, 3>;
      folly::Optional<Timestamp> ts;
      uint8_t tos = 0;

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
      static constexpr size_t kCmsgSpace = CMSG_SPACE(sizeof(uint16_t)) +
          CMSG_SPACE(sizeof(Timestamp)) + CMSG_SPACE(sizeof(uint8_t));
#endif
    };

    /**
     * Invoked when the socket becomes readable and we want buffer
     * to write to.
     *
     * NOTE: From socket we will end up reading at most `len` bytes
     *       and if there were more bytes in datagram, we will end up
     *       dropping them.
     */
    virtual void getReadBuffer(void** buf, size_t* len) noexcept = 0;

    /**
     * Invoked when a new datagram is available on the socket. `len`
     * is the number of bytes read and `truncated` is true if we had
     * to drop few bytes because of running out of buffer space.
     *  OnDataAvailableParams::gro is the GRO segment size
     */
    virtual void onDataAvailable(
        const folly::SocketAddress& client,
        size_t len,
        bool truncated,
        OnDataAvailableParams params) noexcept = 0;

    /**
     * Notifies when data is available. This is only invoked when
     * shouldNotifyOnly() returns true.
     */
    virtual void onNotifyDataAvailable(AsyncUDPSocket&) noexcept {}

    /**
     * Returns whether or not the read callback should only notify
     * but not call getReadBuffer.
     * If shouldNotifyOnly() returns true, AsyncUDPSocket will invoke
     * onNotifyDataAvailable() instead of getReadBuffer().
     * If shouldNotifyOnly() returns false, AsyncUDPSocket will invoke
     * getReadBuffer() and onDataAvailable().
     */
    virtual bool shouldOnlyNotify() { return false; }

    /**
     * Invoked when there is an error reading from the socket.
     *
     * NOTE: Since UDP is connectionless, you can still read from the socket.
     *       But you have to re-register readCallback yourself after
     *       onReadError.
     */
    virtual void onReadError(const AsyncSocketException& ex) noexcept = 0;

    /**
     * Invoked when socket is closed and a read callback is registered.
     */
    virtual void onReadClosed() noexcept = 0;

    virtual ~ReadCallback() = default;
  };

  class ErrMessageCallback {
   public:
    virtual ~ErrMessageCallback() = default;

    /**
     * errMessage() will be invoked when kernel puts a message to
     * the error queue associated with the socket.
     *
     * @param cmsg      Reference to cmsghdr structure describing
     *                  a message read from error queue associated
     *                  with the socket.
     */
    virtual void errMessage(const cmsghdr& cmsg) noexcept = 0;

    /**
     * errMessageError() will be invoked if an error occurs reading a message
     * from the socket error stream.
     *
     * @param ex        An exception describing the error that occurred.
     */
    virtual void errMessageError(const AsyncSocketException& ex) noexcept = 0;
  };

  static void fromMsg(
      [[maybe_unused]] ReadCallback::OnDataAvailableParams& params,
      [[maybe_unused]] struct msghdr& msg);

  using IOBufFreeFunc = folly::Function<void(std::unique_ptr<folly::IOBuf>&&)>;

  using AdditionalCmsgsFunc = folly::Function<folly::Optional<SocketCmsgMap>()>;

  struct WriteOptions {
    WriteOptions() = default;
    WriteOptions(int gsoVal, bool zerocopyVal)
        : gso(gsoVal), zerocopy(zerocopyVal) {}
    int gso{0};
    bool zerocopy{false};
    std::chrono::microseconds txTime{0};
  };

  struct TXTime {
    int clockid{-1};
    bool deadline{false};
  };

  /**
   * Create a new UDP socket that will run in the
   * given eventbase
   */
  explicit AsyncUDPSocket(EventBase* evb);
  ~AsyncUDPSocket() override;

  /**
   * Returns the address server is listening on
   */
  virtual const folly::SocketAddress& address() const {
    CHECK_NE(NetworkSocket(), fd_) << "Server not yet bound to an address";
    return localAddress_;
  }

  /**
   * Contains options to pass to bind.
   */
  struct BindOptions {
    BindOptions() noexcept {}
    // Whether IPV6_ONLY should be set on the socket.
    bool bindV6Only{true};
    std::string ifName;
  };

  /**
   * Bind the socket to the following address. If port is not
   * set in the `address` an ephemeral port is chosen and you can
   * use `address()` method above to get it after this method successfully
   * returns. The parameter BindOptions contains parameters for the bind.
   */
  virtual void bind(
      const folly::SocketAddress& address, BindOptions options = BindOptions());

  /**
   * Connects the UDP socket to a remote destination address provided in
   * address. This can speed up UDP writes on linux because it will cache flow
   * state on connects.
   * Using connect has many quirks, and you should be aware of them before using
   * this API:
   * 1. If this is called before bind, the socket will be automatically bound to
   * the IP address of the current default network interface.
   * 2. Normally UDP can use the 2 tuple (src ip, src port) to steer packets
   * sent by the peer to the socket, however after connecting the socket, only
   * packets destined to the destination address specified in connect() will be
   * forwarded and others will be dropped. If the server can send a packet
   * from a different destination port / IP then you probably do not want to use
   * this API.
   * 3. It can be called repeatedly on either the client or server however it's
   * normally only useful on the client and not server.
   *
   * Returns the result of calling the connect syscall.
   */
  virtual void connect(const folly::SocketAddress& address);

  /**
   * Use an already bound file descriptor. You can either transfer ownership
   * of this FD by using ownership = FDOwnership::OWNS or share it using
   * FDOwnership::SHARED. In case FD is shared, it will not be `close`d in
   * destructor.
   */
  virtual void setFD(NetworkSocket fd, FDOwnership ownership);

  bool setZeroCopy(bool enable);
  bool getZeroCopy() const { return zeroCopyEnabled_; }

  uint32_t getZeroCopyBufId() const { return zeroCopyBufId_; }

  size_t getZeroCopyReenableThreshold() const {
    return zeroCopyReenableThreshold_;
  }

  void setZeroCopyReenableThreshold(size_t threshold) {
    zeroCopyReenableThreshold_ = threshold;
  }

  /**
   * Set extra control messages to send
   */
  virtual void setCmsgs(const SocketCmsgMap& cmsgs);
  virtual void setNontrivialCmsgs(
      const SocketNontrivialCmsgMap& nontrivialCmsgs);

  virtual void appendCmsgs(const SocketCmsgMap& cmsgs);
  virtual void appendNontrivialCmsgs(
      const SocketNontrivialCmsgMap& nontrivialCmsgs);
  virtual void setAdditionalCmsgsFunc(
      AdditionalCmsgsFunc&& additionalCmsgsFunc) {
    additionalCmsgsFunc_ = std::move(additionalCmsgsFunc);
    dynamicCmsgs_.clear();
  }

  /**
   * Send the data in buffer to destination. Returns the return code from
   * ::sendmsg.
   */
  virtual ssize_t write(
      const folly::SocketAddress& address,
      const std::unique_ptr<folly::IOBuf>& buf);

  /**
   * Send the data in buffers to destination. Returns the return code from
   * ::sendmmsg.
   * bufs is an array of std::unique_ptr<folly::IOBuf>
   * of size num
   */
  virtual int writem(
      Range<SocketAddress const*> addrs,
      const std::unique_ptr<folly::IOBuf>* bufs,
      size_t count);

  /**
   * Send the data in buffers to destination. Returns the return code from
   * ::sendmmsg.
   * iov is an array of iovecs, which is composed of "count" messages that
   * need to be sent. Each message can have multiple iovecs. The number of
   * iovecs per message is specified in numIovecsInBuffer.
   */
  virtual int writemv(
      Range<SocketAddress const*> addrs,
      iovec* iov,
      size_t* numIovecsInBuffer,
      size_t count);

  /**
   * Send the data in buffer to destination. Returns the return code from
   * ::sendmsg.
   *  gso is the generic segmentation offload value
   *  writeGSO will return -1 if
   *  buf->computeChainDataLength() <= gso
   *  Before calling writeGSO with a positive value
   *  verify GSO is supported on this platform by calling getGSO
   */
  virtual ssize_t writeGSO(
      const folly::SocketAddress& address,
      const std::unique_ptr<folly::IOBuf>& buf,
      WriteOptions options);

  virtual ssize_t writeChain(
      const folly::SocketAddress& address,
      std::unique_ptr<folly::IOBuf>&& buf,
      WriteOptions options);

  /**
   * Send the data in buffers to destination. Returns the return code from
   * ::sendmmsg.
   * bufs is an array of std::unique_ptr<folly::IOBuf>
   * of size num
   * options is an array of WriteOptions or nullptr
   *  Before calling writeGSO with a positive value
   *  verify GSO is supported on this platform by calling getGSO
   */
  virtual int writemGSO(
      Range<SocketAddress const*> addrs,
      const std::unique_ptr<folly::IOBuf>* bufs,
      size_t count,
      const WriteOptions* options);

  /**
   * Send the data in buffers to destination. Returns the return code from
   * ::sendmmsg.
   * iov is an array of iovecs, which is composed of "count" messages that
   * need to be sent. Each message can have multiple iovecs. The number of
   * iovecs per message is specified in numIovecsInBuffer.
   * options is an array of WriteOptions or nullptr
   * Before calling writeGSO with a positive value
   * verify GSO is supported on this platform by calling getGSO
   */
  virtual int writemGSOv(
      Range<SocketAddress const*> addrs,
      iovec* iov,
      size_t* numIovecsInBuffer,
      size_t count,
      const WriteOptions* options);

  /**
   * Send data in iovec to destination. Returns the return code from sendmsg.
   */
  virtual ssize_t writev(
      const folly::SocketAddress& address,
      const struct iovec* vec,
      size_t iovec_len,
      WriteOptions options);

  virtual ssize_t writev(
      const folly::SocketAddress& address,
      const struct iovec* vec,
      size_t iovec_len);

  virtual ssize_t recvmsg(struct msghdr* msg, int flags);

  virtual int recvmmsg(
      struct mmsghdr* msgvec,
      unsigned int vlen,
      unsigned int flags,
      struct timespec* timeout);

  /**
   * Start reading datagrams
   */
  virtual void resumeRead(ReadCallback* cob);

  /**
   * Pause reading datagrams
   */
  virtual void pauseRead();

  /**
   * Stop listening on the socket.
   */
  virtual void close();

  /**
   * Get internal FD used by this socket
   */
  virtual NetworkSocket getNetworkSocket() const {
    CHECK_NE(NetworkSocket(), fd_) << "Need to bind before getting FD out";
    return fd_;
  }

  /**
   * Set IP_FREEBIND to allow binding to an address that is nonlocal or doesn't
   * exist yet.
   */
  virtual void setFreeBind(bool freeBind) { freeBind_ = freeBind; }

  /**
   * Set IP_TRANSPARENT to allow enables transparent proxying on the socket
   */
  virtual void setTransparent(bool transparent) { transparent_ = transparent; }

  /**
   * Set IPV6_RECVTCLASS/IP_RECVTOS to allow receiving of the IPv6 Traffic
   * Class/IPv4 Type of Service field.
   */
  virtual void setRecvTos(bool recvTos) { recvTos_ = recvTos; }

  /**
   * Get IPV6_RECVTCLASS/IP_RECVTOS status of the socket. If true, the IPv6
   * Traffic Class/IPv4 Type of Service field should be populated in
   * OnDataAvailableParams.
   */
  virtual bool getRecvTos() { return recvTos_; }

  /**
   * Set reuse port mode to call bind() on the same address multiple times
   */
  virtual void setReusePort(bool reusePort) { reusePort_ = reusePort; }

  /**
   * Set SO_REUSEADDR flag on the socket. Default is OFF.
   */
  virtual void setReuseAddr(bool reuseAddr) { reuseAddr_ = reuseAddr; }

  /**
   * Set SO_RCVBUF option on the socket, if not zero. Default is zero.
   */
  virtual void setRcvBuf(int rcvBuf) { rcvBuf_ = rcvBuf; }

  /**
   * Set SO_SNDBUF option on the socket, if not zero. Default is zero.
   */
  virtual void setSndBuf(int sndBuf) { sndBuf_ = sndBuf; }

  /**
   * Set SO_BUSY_POLL option on the socket, if not zero. Default is zero.
   * Caution! The feature is not available on Apple's systems.
   */
  virtual void setBusyPoll(int busyPollUs) { busyPollUs_ = busyPollUs; }

  EventBase* getEventBase() const { return eventBase_; }

  /**
   * Enable or disable fragmentation on the socket.
   *
   * On Linux, this sets IP(V6)_MTU_DISCOVER to IP(V6)_PMTUDISC_DO when enabled,
   * and to IP(V6)_PMTUDISC_WANT when disabled. IP(V6)_PMTUDISC_WANT will use
   * per-route setting to set DF bit. It may be more desirable to use
   * IP(V6)_PMTUDISC_PROBE as opposed to IP(V6)_PMTUDISC_DO for apps that has
   * its own PMTU Discovery mechanism.
   * Note this doesn't work on Apple.
   */
  virtual void dontFragment(bool df);

  /**
   * Set Dont-Fragment (DF) but ignore Path MTU.
   *
   * On Linux, this sets  IP(V6)_MTU_DISCOVER to IP(V6)_PMTUDISC_PROBE.
   * This essentially sets DF but ignores Path MTU for this socket.
   * This may be desirable for apps that has its own PMTU Discovery mechanism.
   * See http://man7.org/linux/man-pages/man7/ip.7.html for more info.
   */
  virtual void setDFAndTurnOffPMTU();

  /**
   * Callback for receiving errors on the UDP sockets
   */
  virtual void setErrMessageCallback(ErrMessageCallback* errMessageCallback);

  virtual bool isBound() const { return fd_ != NetworkSocket(); }

  virtual bool isReading() const { return readCallback_ != nullptr; }

  /**
   * Set the maximum number of reads to execute from the underlying
   * socket each time the EventBase detects that new ingress data is
   * available. The default is kMaxReadsPerEvent
   *
   * @param maxReads  Maximum number of reads per data-available event;
   *                  a value of zero means unlimited.
   */
  void setMaxReadsPerEvent(uint16_t maxReads) { maxReadsPerEvent_ = maxReads; }

  /**
   * Get the maximum number of reads this object will execute from
   * the underlying socket each time the EventBase detects that new
   * ingress data is available.
   *
   * @returns Maximum number of reads per data-available event; a value
   *          of zero means unlimited.
   */
  uint16_t getMaxReadsPerEvent() const { return maxReadsPerEvent_; }

  virtual void detachEventBase();

  virtual void attachEventBase(folly::EventBase* evb);

  // generic segmentation offload get/set
  // negative return value means GSO is not available
  virtual int getGSO();

  bool setGSO(int val);

  void setIOBufFreeFunc(IOBufFreeFunc&& ioBufFreeFunc) {
    ioBufFreeFunc_ = std::move(ioBufFreeFunc);
  }

  // generic receive offload get/set
  // negative return value means GRO is not available
  int getGRO();

  bool setGRO(bool bVal);

  // TX time
  TXTime getTXTime();

  bool setTXTime(TXTime txTime);

  // packet timestamping
  int getTimestamping();
  bool setTimestamping(int val);

  // disable/enable RX zero checksum check for UDP over IPv6
  bool setRxZeroChksum6(bool bVal);

  // disable/enable TX zero checksum for UDP over IPv6
  bool setTxZeroChksum6(bool bVal);

  // Set ToS or Traffic Class in the underlying socket
  // depending on the address family.
  void setTosOrTrafficClass(uint8_t tosOrTclass);

  virtual void applyOptions(
      const SocketOptionMap& options, SocketOptionKey::ApplyPos pos);

  /**
   * Override netops::Dispatcher to be used for netops:: calls.
   *
   * Pass empty shared_ptr to reset to default.
   * Override can be used by unit tests to intercept and mock netops:: calls.
   */
  virtual void setOverrideNetOpsDispatcher(
      std::shared_ptr<netops::Dispatcher> dispatcher) {
    netops_.setOverride(std::move(dispatcher));
  }

  /**
   * Returns override netops::Dispatcher being used for netops:: calls.
   *
   * Returns empty shared_ptr if no override set.
   * Override can be used by unit tests to intercept and mock netops:: calls.
   */
  virtual std::shared_ptr<netops::Dispatcher> getOverrideNetOpsDispatcher()
      const {
    return netops_.getOverride();
  }

  // Initializes underlying socket fd. This is called in bind() and connect()
  // internally if fd is not yet set at the time of the call. But if there is a
  // need to apply socket options pre-bind, one can call this function
  // explicitly before bind()/connect() and socket opts application.
  void init(sa_family_t family, BindOptions bindOptions = BindOptions());

 protected:
  struct full_sockaddr_storage {
    sockaddr_storage storage;
    socklen_t len;
  };

  virtual ssize_t sendmsg(
      NetworkSocket socket, const struct msghdr* message, int flags) {
    return netops_->sendmsg(socket, message, flags);
  }

  virtual int sendmmsg(
      NetworkSocket socket,
      struct mmsghdr* msgvec,
      unsigned int vlen,
      int flags) {
    return netops_->sendmmsg(socket, msgvec, vlen, flags);
  }

  void fillIoVec(
      const std::unique_ptr<folly::IOBuf>* bufs,
      struct iovec* iov,
      size_t* messageIovLens,
      size_t count,
      size_t iov_count);

  void fillMsgVec(
      Range<full_sockaddr_storage*> addrs,
      size_t* messageIovLens,
      size_t count,
      struct mmsghdr* msgvec,
      struct iovec* iov,
      const WriteOptions* options,
      char* control);

  virtual int writeImplIOBufs(
      Range<SocketAddress const*> addrs,
      const std::unique_ptr<folly::IOBuf>* bufs,
      size_t count,
      struct mmsghdr* msgvec,
      const WriteOptions* options,
      char* control);

  virtual int writeImpl(
      Range<SocketAddress const*> addrs,
      size_t* messageIovLens,
      struct iovec* iov,
      size_t count,
      struct mmsghdr* msgvec,
      const WriteOptions* options,
      char* control);

  virtual ssize_t writevImpl(
      netops::Msgheader* msg, [[maybe_unused]] WriteOptions options);

  size_t handleErrMessages() noexcept;

  void failErrMessageRead(const AsyncSocketException& ex);

  static auto constexpr kDefaultReadsPerEvent = 1;
  uint16_t maxReadsPerEvent_{
      kDefaultReadsPerEvent}; ///< Max reads per event loop iteration

  // Non-null only when we are reading
  ReadCallback* readCallback_;

 private:
  // EventHandler
  void handlerReady(uint16_t events) noexcept override;

  void handleRead() noexcept;
  bool updateRegistration() noexcept;
  void maybeUpdateDynamicCmsgs() noexcept;

  EventBase* eventBase_;
  folly::SocketAddress localAddress_;

  NetworkSocket fd_;
  FDOwnership ownership_;

  // Temp space to receive client address
  folly::SocketAddress clientAddress_;

  // If the socket is connected.
  folly::SocketAddress connectedAddress_;
  bool connected_{false};

  bool reuseAddr_{false};
  bool reusePort_{false};
  bool freeBind_{false};
  bool transparent_{false};
  bool recvTos_{false};
  int rcvBuf_{0};
  int sndBuf_{0};
  int busyPollUs_{0};

  // generic segmentation offload value, if available
  // See https://lwn.net/Articles/188489/ for more details
  folly::Optional<int> gso_;

  // generic receive offload value, if available
  // See https://lwn.net/Articles/770978/ for more details
  folly::Optional<int> gro_;

  // multi release pacing for UDP GSO
  // See https://lwn.net/Articles/822726/ for more details
  folly::Optional<TXTime> txTime_;

  // packet timestamping
  folly::Optional<int> ts_;

  ErrMessageCallback* errMessageCallback_{nullptr};

  bool zeroCopyEnabled_{false};
  bool zeroCopyVal_{false};
  // zerocopy re-enable logic
  size_t zeroCopyReenableThreshold_{0};
  size_t zeroCopyReenableCounter_{0};

  uint32_t zeroCopyBufId_{0};

  int getZeroCopyFlags();
  static bool isZeroCopyMsg([[maybe_unused]] const cmsghdr& cmsg);
  void processZeroCopyMsg([[maybe_unused]] const cmsghdr& cmsg);
  void addZeroCopyBuf(std::unique_ptr<folly::IOBuf>&& buf);
  void releaseZeroCopyBuf(uint32_t id);

  uint32_t getNextZeroCopyBufId() { return zeroCopyBufId_++; }

  std::unordered_map<uint32_t, std::unique_ptr<folly::IOBuf>> idZeroCopyBufMap_;

  IOBufFreeFunc ioBufFreeFunc_;

  SocketCmsgMap defaultCmsgs_;
  SocketCmsgMap dynamicCmsgs_;
  SocketCmsgMap* cmsgs_{&defaultCmsgs_};

  SocketNontrivialCmsgMap nontrivialCmsgs_;

  AdditionalCmsgsFunc additionalCmsgsFunc_;

  netops::DispatcherContainer netops_;
};

} // namespace folly
