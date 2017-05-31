#ifndef SRC_NODE_HTTP2_CORE_INL_H_
#define SRC_NODE_HTTP2_CORE_INL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "node_http2_core.h"
#include "node_internals.h"  // arraysize
#include "freelist.h"

namespace node {
namespace http2 {

#define FREELIST_MAX 1024

#define LINKED_LIST_ADD(list, item)                                           \
  do {                                                                        \
    if (list ## _tail_ == nullptr) {                                          \
      list ## _head_ = item;                                                  \
      list ## _tail_ = item;                                                  \
    } else {                                                                  \
      list ## _tail_->next = item;                                            \
      list ## _tail_ = item;                                                  \
    }                                                                         \
  } while (0);

extern Freelist<nghttp2_data_chunk_t, FREELIST_MAX>
    data_chunk_free_list;

extern Freelist<Nghttp2Stream, FREELIST_MAX> stream_free_list;

extern Freelist<nghttp2_header_list, FREELIST_MAX> header_free_list;

extern Freelist<nghttp2_data_chunks_t, FREELIST_MAX>
    data_chunks_free_list;

// See: https://nghttp2.org/documentation/nghttp2_submit_shutdown_notice.html
inline void Nghttp2Session::SubmitShutdownNotice() {
  nghttp2_submit_shutdown_notice(session_);
}

// Sends a SETTINGS frame on the current session
inline int Nghttp2Session::SubmitSettings(const nghttp2_settings_entry iv[],
                                          size_t niv) {
  return nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, niv);
}

// Returns the Nghttp2Stream associated with the given id, or nullptr if none
inline Nghttp2Stream* Nghttp2Session::FindStream(int32_t id) {
  auto s = streams_.find(id);
  if (s != streams_.end()) {
    return s->second;
  } else {
    return nullptr;
  }
}

// Passes all of the the chunks for a data frame out to the JS layer
// The chunks are collected as the frame is being processed and sent out
// to the JS side only when the frame is fully processed.
inline void Nghttp2Session::HandleDataFrame(const nghttp2_frame* frame) {
  int32_t id = frame->hd.stream_id;
  Nghttp2Stream* stream = this->FindStream(id);
  // If the stream does not exist, something really bad happened
  CHECK_NE(stream, nullptr);

  while (stream->data_chunks_head_ != nullptr) {
    nghttp2_data_chunk_t* item = stream->data_chunks_head_;
    stream->data_chunks_head_ = item->next;
    OnDataChunk(stream, item);
    delete[] item->buf.base;
    data_chunk_free_list.push(item);
  }
  stream->data_chunks_tail_ = nullptr;
}

// Passes all of the collected headers for a HEADERS frame out to the JS layer.
// The headers are collected as the frame is being processed and sent out
// to the JS side only when the frame is fully processed.
inline void Nghttp2Session::HandleHeadersFrame(const nghttp2_frame* frame) {
  int32_t id = (frame->hd.type == NGHTTP2_PUSH_PROMISE) ?
    frame->push_promise.promised_stream_id : frame->hd.stream_id;
  Nghttp2Stream* stream = FindStream(id);
  // If the stream does not exist, something really bad happened
  CHECK_NE(stream, nullptr);
  OnHeaders(stream,
            stream->headers(),
            stream->headers_category(),
            frame->hd.flags);
  stream->FreeHeaders();
}

// Notifies the JS layer that a PRIORITY frame has been received
inline void Nghttp2Session::HandlePriorityFrame(const nghttp2_frame* frame) {
  nghttp2_priority priority_frame = frame->priority;
  int32_t id = frame->hd.stream_id;
  // Ignore the priority frame if stream ID is <= 0
  // This actually should never happen because nghttp2 should treat this as
  // an error condition that terminates the session.
  if (id > 0) {
    nghttp2_priority_spec spec = priority_frame.pri_spec;
    OnPriority(id, spec.stream_id, spec.weight, spec.exclusive);
  }
}

// Prompts nghttp2 to flush the queue of pending data frames
inline void Nghttp2Session::SendPendingData() {
  const uint8_t* data;
  size_t amount = 0;
  size_t offset = 0;
  size_t src_offset = 0;
  uv_buf_t* current = AllocateSend(SEND_BUFFER_RECOMMENDED_SIZE);
  assert(current);
  size_t remaining = current->len;
  while ((amount = nghttp2_session_mem_send(session_, &data)) > 0) {
    while (amount > 0) {
      if (amount > remaining) {
        // The amount copied does not fit within the remaining available
        // buffer, copy what we can tear it off and keep going.
        memcpy(current->base + offset, data + src_offset, remaining);
        offset += remaining;
        src_offset = remaining;
        amount -= remaining;
        Send(current, offset);
        offset = 0;
        current = AllocateSend(SEND_BUFFER_RECOMMENDED_SIZE);
        assert(current);
        remaining = current->len;
        continue;
      }
      memcpy(current->base + offset, data + src_offset, amount);
      offset += amount;
      remaining -= amount;
      amount = 0;
      src_offset = 0;
    }
  }
  Send(current, offset);
}

// Initialize the Nghttp2Session handle by creating and
// assigning the Nghttp2Session instance and associated
// uv_loop_t.
inline int Nghttp2Session::Init(uv_loop_t* loop,
                               const nghttp2_session_type type,
                               nghttp2_option* options,
                               nghttp2_mem* mem) {
  loop_ = loop;
  session_type_ = type;
  int ret = 0;

  nghttp2_session_callbacks* callbacks
      = callback_struct_saved[HasGetPaddingCallback() ? 1 : 0].callbacks;

  nghttp2_option* opts;
  if (options != nullptr) {
    opts = options;
  } else {
    nghttp2_option_new(&opts);
  }

  switch (type) {
    case NGHTTP2_SESSION_SERVER:
      ret = nghttp2_session_server_new3(&session_,
                                        callbacks,
                                        this,
                                        opts,
                                        mem);
      break;
    case NGHTTP2_SESSION_CLIENT:
      ret = nghttp2_session_client_new3(&session_,
                                        callbacks,
                                        this,
                                        opts,
                                        mem);
      break;
  }
  if (opts != options) {
    nghttp2_option_del(opts);
  }

  uv_prepare_init(loop_, &prep_);

  uv_prepare_start(&prep_, [](uv_prepare_t* t) {
    Nghttp2Session* session = ContainerOf(&Nghttp2Session::prep_, t);
    session->SendPendingData();
  });
  return ret;
}


inline int Nghttp2Session::Free() {
  assert(session_ != nullptr);

  // Stop the loop
  uv_prepare_stop(&prep_);
  auto PrepClose = [](uv_handle_t* handle) {
    Nghttp2Session* session =
        ContainerOf(&Nghttp2Session::prep_,
                    reinterpret_cast<uv_prepare_t*>(handle));

    session->OnFreeSession();
  };
  uv_close(reinterpret_cast<uv_handle_t*>(&prep_), PrepClose);

  nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
  nghttp2_session_del(session_);
  session_ = nullptr;
  loop_ = nullptr;
  return 1;
}

// Write data received from the socket to the underlying nghttp2_session.
inline ssize_t Nghttp2Session::Write(const uv_buf_t* bufs, unsigned int nbufs) {
  size_t total = 0;
  for (unsigned int n = 0; n < nbufs; n++) {
    ssize_t ret =
      nghttp2_session_mem_recv(session_,
                               reinterpret_cast<uint8_t*>(bufs[n].base),
                               bufs[n].len);
    if (ret < 0) {
      return ret;
    } else {
      total += ret;
    }
  }
  SendPendingData();
  return total;
}

inline void Nghttp2Session::AddStream(Nghttp2Stream* stream) {
  streams_[stream->id()] = stream;
}

// Removes a stream instance from this session
inline void Nghttp2Session::RemoveStream(int32_t id) {
  streams_.erase(id);
}

// Implementation for Nghttp2Stream functions

inline Nghttp2Stream* Nghttp2Stream::Init(
    int32_t id,
    Nghttp2Session* session,
    nghttp2_headers_category category) {
  Nghttp2Stream* stream = stream_free_list.pop();
  stream->ResetState(id, session, category);
  session->AddStream(stream);
  return stream;
}


// Resets the state of the stream instance to defaults
inline void Nghttp2Stream::ResetState(
    int32_t id,
    Nghttp2Session* session,
    nghttp2_headers_category category) {
  session_ = session;
  queue_head_ = nullptr;
  queue_tail_ = nullptr;
  data_chunks_head_ = nullptr;
  data_chunks_tail_ = nullptr;
  current_headers_head_ = nullptr;
  current_headers_tail_ = nullptr;
  current_headers_category_ = category;
  flags_ = NGHTTP2_STREAM_FLAG_NONE;
  id_ = id;
  code_ = NGHTTP2_NO_ERROR;
  prev_local_window_size_ = 65535;
  queue_head_index_ = 0;
  queue_head_offset_ = 0;
}


inline void Nghttp2Stream::Destroy() {
  // Do nothing if this stream instance is already destroyed
  if (IsDestroyed() || IsDestroying())
    return;
  flags_ |= NGHTTP2_STREAM_DESTROYING;
  Nghttp2Session* session = this->session_;

  if (session != nullptr) {
    // Remove this stream from the associated session
    session_->RemoveStream(this->id());
    session_ = nullptr;
  }

  // Free any remaining data chunks.
  while (data_chunks_head_ != nullptr) {
    nghttp2_data_chunk_t* chunk = data_chunks_head_;
    data_chunks_head_ = chunk->next;
    delete[] chunk->buf.base;
    data_chunk_free_list.push(chunk);
  }
  data_chunks_tail_ = nullptr;

  // Free any remainingin headers
  FreeHeaders();

  // Return this stream instance to the freelist
  stream_free_list.push(this);
}

inline void Nghttp2Stream::FreeHeaders() {
  while (current_headers_head_ != nullptr) {
    nghttp2_header_list* item = current_headers_head_;
    current_headers_head_ = item->next;
    nghttp2_rcbuf_decref(item->value);
    header_free_list.push(item);
  }
  current_headers_tail_ = nullptr;
}

// Submit informational headers for a stream.
inline int Nghttp2Stream::SubmitInfo(nghttp2_nv* nva, size_t len) {
  return nghttp2_submit_headers(session_->session(),
                                NGHTTP2_FLAG_NONE,
                                id_, nullptr,
                                nva, len, nullptr);
}

inline int Nghttp2Stream::SubmitPriority(nghttp2_priority_spec* prispec,
                                         bool silent) {
  return silent ?
      nghttp2_session_change_stream_priority(session_->session(),
                                             id_, prispec) :
      nghttp2_submit_priority(session_->session(),
                              NGHTTP2_FLAG_NONE,
                              id_, prispec);
}

// Submit an RST_STREAM frame
inline int Nghttp2Stream::SubmitRstStream(const uint32_t code) {
  session_->SendPendingData();
  return nghttp2_submit_rst_stream(session_->session(),
                                   NGHTTP2_FLAG_NONE,
                                   id_,
                                   code);
}

// Submit a push promise
inline int32_t Nghttp2Stream::SubmitPushPromise(
    nghttp2_nv* nva,
    size_t len,
    Nghttp2Stream** assigned,
    bool emptyPayload) {
  int32_t ret = nghttp2_submit_push_promise(session_->session(),
                                            NGHTTP2_FLAG_NONE,
                                            id_, nva, len,
                                            nullptr);
  if (ret > 0) {
    auto stream = Nghttp2Stream::Init(ret, session_);
    if (emptyPayload) stream->Shutdown();
    if (assigned != nullptr) *assigned = stream;
  }
  return ret;
}

// Initiate a response. If the nghttp2_stream is still writable by
// the time this is called, then an nghttp2_data_provider will be
// initialized, causing at least one (possibly empty) data frame to
// be sent.
inline int Nghttp2Stream::SubmitResponse(nghttp2_nv* nva,
                                         size_t len,
                                         bool emptyPayload) {
  nghttp2_data_provider* provider = nullptr;
  nghttp2_data_provider prov;
  prov.source.ptr = this;
  prov.read_callback = Nghttp2Session::OnStreamRead;
  if (!emptyPayload && IsWritable())
    provider = &prov;

  return nghttp2_submit_response(session_->session(), id_,
                                 nva, len, provider);
}


// Initiate a request. If writable is true (the default), then
// an nghttp2_data_provider will be initialized, causing at
// least one (possibly empty) data frame to to be sent.
inline int32_t Nghttp2Session::SubmitRequest(
    nghttp2_priority_spec* prispec,
    nghttp2_nv* nva,
    size_t len,
    Nghttp2Stream** assigned,
    bool emptyPayload) {
  nghttp2_data_provider* provider = nullptr;
  nghttp2_data_provider prov;
  prov.source.ptr = this;
  prov.read_callback = OnStreamRead;
  if (!emptyPayload)
    provider = &prov;
  int32_t ret = nghttp2_submit_request(session_,
                                       prispec, nva, len,
                                       provider, nullptr);
  // Assign the Nghttp2Stream handle
  if (ret > 0) {
    Nghttp2Stream* stream = Nghttp2Stream::Init(ret, this);
    if (emptyPayload) stream->Shutdown();
    if (assigned != nullptr) *assigned = stream;
  }
  return ret;
}

// Queue the given set of uv_but_t handles for writing to an
// nghttp2_stream. The callback will be invoked once the chunks
// of data have been flushed to the underlying nghttp2_session.
// Note that this does *not* mean that the data has been flushed
// to the socket yet.
inline int Nghttp2Stream::Write(nghttp2_stream_write_t* req,
                                const uv_buf_t bufs[],
                                unsigned int nbufs,
                                nghttp2_stream_write_cb cb) {
  if (!IsWritable()) {
    if (cb != nullptr)
      cb(req, UV_EOF);
    return 0;
  }
  nghttp2_stream_write_queue* item = new nghttp2_stream_write_queue;
  item->cb = cb;
  item->req = req;
  item->nbufs = nbufs;
  item->bufs.AllocateSufficientStorage(nbufs);
  req->handle = this;
  req->item = item;
  memcpy(*(item->bufs), bufs, nbufs * sizeof(*bufs));

  if (queue_head_ == nullptr) {
    queue_head_ = item;
    queue_tail_ = item;
  } else {
    queue_tail_->next = item;
    queue_tail_ = item;
  }
  nghttp2_session_resume_data(session_->session(), id_);
  return 0;
}

inline void Nghttp2Stream::ReadStart() {
  if (IsPaused()) {
    // If handle->reading is less than zero, read_start had never previously
    // been called. If handle->reading is zero, reading had started and read
    // stop had been previously called, meaning that the flow control window
    // has been explicitly set to zero. Reset the flow control window now to
    // restart the flow of data.
    nghttp2_session_set_local_window_size(session_->session(),
                                          NGHTTP2_FLAG_NONE,
                                          id_,
                                          prev_local_window_size_);
  }
  flags_ |= NGHTTP2_STREAM_READ_START;
  flags_ &= ~NGHTTP2_STREAM_READ_PAUSED;
  // TODO(jasnell): Drain the queued data chunks...
}

inline void Nghttp2Stream::ReadStop() {
  // Has no effect if IsReading() is false, which will happen if we either
  // have not started reading yet at all (NGHTTP2_STREAM_READ_START is not
  // set) or if we're already paused (NGHTTP2_STREAM_READ_PAUSED is set.
  if (!IsReading())
    return;
  flags_ |= NGHTTP2_STREAM_READ_PAUSED;

  // When not reading, explicitly set the local window size to 0 so that
  // the peer does not keep sending data that has to be buffered
  int32_t ret =
    nghttp2_session_get_stream_local_window_size(session_->session(), id_);
  if (ret >= 0)
    prev_local_window_size_ = ret;
  nghttp2_session_set_local_window_size(session_->session(),
                                        NGHTTP2_FLAG_NONE,
                                        id_, 0);
}

nghttp2_data_chunks_t::~nghttp2_data_chunks_t() {
  for (unsigned int n = 0; n < nbufs; n++) {
    free(buf[n].base);
  }
}

Nghttp2Session::Callbacks::Callbacks(bool kHasGetPaddingCallback) {
  nghttp2_session_callbacks_new(&callbacks);
  nghttp2_session_callbacks_set_on_begin_headers_callback(
    callbacks, OnBeginHeadersCallback);
  nghttp2_session_callbacks_set_on_header_callback2(
    callbacks, OnHeaderCallback);
  nghttp2_session_callbacks_set_on_frame_recv_callback(
    callbacks, OnFrameReceive);
  nghttp2_session_callbacks_set_on_stream_close_callback(
    callbacks, OnStreamClose);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
    callbacks, OnDataChunkReceived);

  if (kHasGetPaddingCallback) {
    nghttp2_session_callbacks_set_select_padding_callback(
      callbacks, OnSelectPadding);
  }
}

Nghttp2Session::Callbacks::~Callbacks() {
  nghttp2_session_callbacks_del(callbacks);
}

}  // namespace http2
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_NODE_HTTP2_CORE_INL_H_
