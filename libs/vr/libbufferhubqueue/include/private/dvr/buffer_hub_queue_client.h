#ifndef ANDROID_DVR_BUFFER_HUB_QUEUE_CLIENT_H_
#define ANDROID_DVR_BUFFER_HUB_QUEUE_CLIENT_H_

#include <gui/BufferQueueDefs.h>

#include <pdx/client.h>
#include <private/dvr/buffer_hub_client.h>
#include <private/dvr/epoll_file_descriptor.h>
#include <private/dvr/ring_buffer.h>

#include <memory>
#include <vector>

namespace android {
namespace dvr {

class ConsumerQueue;

// |BufferHubQueue| manages a queue of |BufferHubBuffer|s. Buffers are
// automatically re-requeued when released by the remote side.
class BufferHubQueue : public pdx::Client {
 public:
  using LocalHandle = pdx::LocalHandle;
  using LocalChannelHandle = pdx::LocalChannelHandle;
  template <typename T>
  using Status = pdx::Status<T>;

  virtual ~BufferHubQueue() {}
  void Initialize();

  // Create a new consumer queue that is attached to the producer. Returns
  // a new consumer queue client or nullptr on failure.
  std::unique_ptr<ConsumerQueue> CreateConsumerQueue();

  // Return the number of buffers avaiable for dequeue.
  size_t count() const { return available_buffers_.GetSize(); }

  // Return the total number of buffers that the queue is tracking.
  size_t capacity() const { return capacity_; }

  // Return the size of metadata structure associated with this BufferBubQueue.
  size_t metadata_size() const { return meta_size_; }

  // Return whether the buffer queue is alrady full.
  bool is_full() const { return  available_buffers_.IsFull(); }

  explicit operator bool() const { return epoll_fd_.IsValid(); }

  std::shared_ptr<BufferHubBuffer> GetBuffer(size_t slot) const {
    return buffers_[slot];
  }

  Status<int> GetEventMask(int events) {
    if (auto* client_channel = GetChannel()) {
      return client_channel->GetEventMask(events);
    } else {
      return pdx::ErrorStatus(EINVAL);
    }
  }

  // Enqueue a buffer marks buffer to be available (|Gain|'ed for producer
  // and |Acquire|'ed for consumer. This is only used for internal bookkeeping.
  void Enqueue(std::shared_ptr<BufferHubBuffer> buf, size_t slot);

  // |BufferHubQueue| will keep track of at most this value of buffers.
  static constexpr size_t kMaxQueueCapacity =
      android::BufferQueueDefs::NUM_BUFFER_SLOTS;

  // Special epoll data field indicating that the epoll event refers to the
  // queue.
  static constexpr int64_t kEpollQueueEventIndex = -1;

  // When pass |kNoTimeout| to |Dequeue|, it will block indefinitely without a
  // timeout.
  static constexpr int kNoTimeOut = -1;

 protected:
  BufferHubQueue(LocalChannelHandle channel, size_t meta_size);
  BufferHubQueue(const std::string& endpoint_path, size_t meta_size);

  // Called by ProducerQueue::AddBuffer and ConsumerQueue::AddBuffer only. to
  // register a buffer for epoll and internal bookkeeping.
  int AddBuffer(const std::shared_ptr<BufferHubBuffer>& buf, size_t slot);

  // Called by ProducerQueue::DetachBuffer and ConsumerQueue::DetachBuffer only.
  // to deregister a buffer for epoll and internal bookkeeping.
  virtual int DetachBuffer(size_t slot);

  // Dequeue a buffer from the free queue, blocking until one is available. The
  // timeout argument specifies the number of milliseconds that |Dequeue()| will
  // block. Specifying a timeout of -1 causes |Dequeue()| to block indefinitely,
  // while specifying a timeout equal to zero cause |Dequeue()| to return
  // immediately, even if no buffers are available.
  std::shared_ptr<BufferHubBuffer> Dequeue(int timeout, size_t* slot,
                                           void* meta, LocalHandle* fence);

  // Wait for buffers to be released and re-add them to the queue.
  bool WaitForBuffers(int timeout);
  void HandleBufferEvent(size_t slot, const epoll_event& event);
  void HandleQueueEvent(const epoll_event& event);

  virtual int OnBufferReady(std::shared_ptr<BufferHubBuffer> buf,
                            LocalHandle* fence) = 0;

  // Called when a buffer is allocated remotely.
  virtual int OnBufferAllocated() = 0;

  // Data members to handle arbitrary metadata passed through BufferHub. It is
  // fair to enforce that all buffers in the same queue share the same metadata
  // type. |meta_size_| is used to store the size of metadata on queue creation;
  // and |meta_buffer_tmp_| is allocated and resized to |meta_size_| on queue
  // creation to be later used as temporary space so that we can avoid
  // additional dynamic memory allocation in each |Enqueue| and |Dequeue| call.
  size_t meta_size_;

  // Here we intentionally choose |unique_ptr<uint8_t[]>| over vector<uint8_t>
  // to disallow dynamic resizing for stability reasons.
  std::unique_ptr<uint8_t[]> meta_buffer_tmp_;

 private:
  static constexpr size_t kMaxEvents = 128;

  // The |u64| data field of an epoll event is interpreted as int64_t:
  // When |index| >= 0 and |index| < kMaxQueueCapacity it refers to a specific
  // element of |buffers_| as a direct index;
  static bool is_buffer_event_index(int64_t index) {
    return index >= 0 &&
           index < static_cast<int64_t>(BufferHubQueue::kMaxQueueCapacity);
  }

  // When |index| == kEpollQueueEventIndex, it refers to the queue itself.
  static bool is_queue_event_index(int64_t index) {
    return index == BufferHubQueue::kEpollQueueEventIndex;
  }

  struct BufferInfo {
    // A logical slot number that is assigned to a buffer at allocation time.
    // The slot number remains unchanged during the entire life cycle of the
    // buffer and should not be confused with the enqueue and dequeue order.
    size_t slot;

    // A BufferHubBuffer client.
    std::shared_ptr<BufferHubBuffer> buffer;

    // Metadata associated with the buffer.
    std::unique_ptr<uint8_t[]> metadata;

    BufferInfo() : BufferInfo(-1, 0) {}

    BufferInfo(size_t slot, size_t metadata_size)
        : slot(slot),
          buffer(nullptr),
          metadata(metadata_size ? new uint8_t[metadata_size] : nullptr) {}

    BufferInfo(BufferInfo&& other)
        : slot(other.slot),
          buffer(std::move(other.buffer)),
          metadata(std::move(other.metadata)) {}

    BufferInfo& operator=(BufferInfo&& other) {
      slot = other.slot;
      buffer = std::move(other.buffer);
      metadata = std::move(other.metadata);
      return *this;
    }

   private:
    BufferInfo(const BufferInfo&) = delete;
    void operator=(BufferInfo&) = delete;
  };

  // Buffer queue:
  // |buffers_| tracks all |BufferHubBuffer|s created by this |BufferHubQueue|.
  std::vector<std::shared_ptr<BufferHubBuffer>> buffers_;

  // |epollhup_pending_| tracks whether a slot of |buffers_| get detached before
  // its corresponding EPOLLHUP event got handled. This could happen as the
  // following sequence:
  // 1. Producer queue's client side allocates a new buffer (at slot 1).
  // 2. Producer queue's client side replaces an existing buffer (at slot 0).
  //    This is implemented by first detaching the buffer and then allocating a
  //    new buffer.
  // 3. During the same epoll_wait, Consumer queue's client side gets EPOLLIN
  //    event on the queue which indicates a new buffer is avaiable and the
  //    EPOLLHUP event for slot 0. Consumer handles these two events in order.
  // 4. Consumer client calls BufferHubRPC::ConsumerQueueImportBuffers and both
  //    slot 0 and (the new) slot 1 buffer will be imported. During the import
  //    of the buffer at slot 1, consuemr client detaches the old buffer so that
  //    the new buffer can be registered. At the same time
  //    |epollhup_pending_[slot]| is marked to indicate that buffer at this slot
  //    was detached prior to EPOLLHUP event.
  // 5. Consumer client continues to handle the EPOLLHUP. Since
  //    |epollhup_pending_[slot]| is marked as true, it can safely ignore the
  //    event without detaching the newly allocated buffer at slot 1.
  //
  // In normal situations where the previously described sequence doesn't
  // happen, an EPOLLHUP event should trigger a regular buffer detach.
  std::vector<bool> epollhup_pending_;

  // |available_buffers_| uses |dvr::RingBuffer| to implementation queue
  // sematics. When |Dequeue|, we pop the front element from
  // |available_buffers_|, and  that buffer's reference count will decrease by
  // one, while another reference in |buffers_| keeps the last reference to
  // prevent the buffer from being deleted.
  RingBuffer<BufferInfo> available_buffers_;

  // Fences (acquire fence for consumer and release fence for consumer) , one
  // for each buffer slot.
  std::vector<LocalHandle> fences_;

  // Keep track with how many buffers have been added into the queue.
  size_t capacity_;

  // Epoll fd used to wait for BufferHub events.
  EpollFileDescriptor epoll_fd_;

  BufferHubQueue(const BufferHubQueue&) = delete;
  void operator=(BufferHubQueue&) = delete;
};

class ProducerQueue : public pdx::ClientBase<ProducerQueue, BufferHubQueue> {
 public:
  template <typename Meta>
  static std::unique_ptr<ProducerQueue> Create() {
    return BASE::Create(sizeof(Meta));
  }

  // Usage bits in |usage_set_mask| will be automatically masked on. Usage bits
  // in |usage_clear_mask| will be automatically masked off. Note that
  // |usage_set_mask| and |usage_clear_mask| may conflict with each other, but
  // |usage_set_mask| takes precedence over |usage_clear_mask|. All buffer
  // allocation through this producer queue shall not have any of the usage bits
  // in |usage_deny_set_mask| set. Allocation calls violating this will be
  // rejected. All buffer allocation through this producer queue must have all
  // the usage bits in |usage_deny_clear_mask| set. Allocation calls violating
  // this will be rejected. Note that |usage_deny_set_mask| and
  // |usage_deny_clear_mask| shall not conflict with each other. Such
  // configuration will be treated as invalid input on creation.
  template <typename Meta>
  static std::unique_ptr<ProducerQueue> Create(int usage_set_mask,
                                               int usage_clear_mask,
                                               int usage_deny_set_mask,
                                               int usage_deny_clear_mask) {
    return BASE::Create(sizeof(Meta), usage_set_mask, usage_clear_mask,
                        usage_deny_set_mask, usage_deny_clear_mask);
  }

  // Import a |ProducerQueue| from a channel handle.
  template <typename Meta>
  static std::unique_ptr<ProducerQueue> Import(LocalChannelHandle handle) {
    return BASE::Create(std::move(handle), sizeof(Meta));
  }

  // Get a buffer producer. Note that the method doesn't check whether the
  // buffer slot has a valid buffer that has been allocated already. When no
  // buffer has been imported before it returns |nullptr|; otherwise it returns
  // a shared pointer to a |BufferProducer|.
  std::shared_ptr<BufferProducer> GetBuffer(size_t slot) const {
    return std::static_pointer_cast<BufferProducer>(
        BufferHubQueue::GetBuffer(slot));
  }

  // Allocate producer buffer to populate the queue. Once allocated, a producer
  // buffer is automatically enqueue'd into the ProducerQueue and available to
  // use (i.e. in |Gain|'ed mode).
  // Returns Zero on success and negative error code when buffer allocation
  // fails.
  int AllocateBuffer(int width, int height, int format, int usage,
                     size_t slice_count, size_t* out_slot);

  // Add a producer buffer to populate the queue. Once added, a producer buffer
  // is available to use (i.e. in |Gain|'ed mode).
  int AddBuffer(const std::shared_ptr<BufferProducer>& buf, size_t slot);

  // Detach producer buffer from the queue.
  // Returns Zero on success and negative error code when buffer detach
  // fails.
  int DetachBuffer(size_t slot) override;

  // Dequeue a producer buffer to write. The returned buffer in |Gain|'ed mode,
  // and caller should call Post() once it's done writing to release the buffer
  // to the consumer side.
  std::shared_ptr<BufferProducer> Dequeue(int timeout, size_t* slot,
                                          LocalHandle* release_fence);

 private:
  friend BASE;

  // Constructors are automatically exposed through ProducerQueue::Create(...)
  // static template methods inherited from ClientBase, which take the same
  // arguments as the constructors.
  explicit ProducerQueue(size_t meta_size);
  ProducerQueue(LocalChannelHandle handle, size_t meta_size);
  ProducerQueue(size_t meta_size, int usage_set_mask, int usage_clear_mask,
                int usage_deny_set_mask, int usage_deny_clear_mask);

  int OnBufferReady(std::shared_ptr<BufferHubBuffer> buf,
                    LocalHandle* release_fence) override;

  // Producer buffer is always allocated from the client (i.e. local) side.
  int OnBufferAllocated() override { return 0; }
};

class ConsumerQueue : public pdx::ClientBase<ConsumerQueue, BufferHubQueue> {
 public:
  // Get a buffer consumer. Note that the method doesn't check whether the
  // buffer slot has a valid buffer that has been imported already. When no
  // buffer has been imported before it returns |nullptr|; otherwise it returns
  // a shared pointer to a |BufferConsumer|.
  std::shared_ptr<BufferConsumer> GetBuffer(size_t slot) const {
    return std::static_pointer_cast<BufferConsumer>(
        BufferHubQueue::GetBuffer(slot));
  }

  // Import newly created buffers from the service side.
  // Returns number of buffers successfully imported; or negative error code
  // when buffer import fails.
  int ImportBuffers();

  // Dequeue a consumer buffer to read. The returned buffer in |Acquired|'ed
  // mode, and caller should call Releasse() once it's done writing to release
  // the buffer to the producer side. |meta| is passed along from BufferHub,
  // The user of BufferProducer is responsible with making sure that the
  // Dequeue() is done with the corect metadata type and size with those used
  // when the buffer is orignally created.
  template <typename Meta>
  std::shared_ptr<BufferConsumer> Dequeue(int timeout, size_t* slot, Meta* meta,
                                          LocalHandle* acquire_fence) {
    return Dequeue(timeout, slot, meta, sizeof(*meta), acquire_fence);
  }

 private:
  friend BASE;

  ConsumerQueue(LocalChannelHandle handle, size_t meta_size);

  // Add a consumer buffer to populate the queue. Once added, a consumer buffer
  // is NOT available to use until the producer side |Post| it. |WaitForBuffers|
  // will catch the |Post| and |Acquire| the buffer to make it available for
  // consumer.
  int AddBuffer(const std::shared_ptr<BufferConsumer>& buf, size_t slot);

  int OnBufferReady(std::shared_ptr<BufferHubBuffer> buf,
                    LocalHandle* acquire_fence) override;

  int OnBufferAllocated() override;

  std::shared_ptr<BufferConsumer> Dequeue(int timeout, size_t* slot, void* meta,
                                          size_t meta_size,
                                          LocalHandle* acquire_fence);
};

}  // namespace dvr
}  // namespace android

#endif  // ANDROID_DVR_BUFFER_HUB_QUEUE_CLIENT_H_
