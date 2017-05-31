#include "node_http2_core-inl.h"

namespace node {
namespace http2 {

// nghttp2 calls this at the beginning a new HEADERS or PUSH_PROMISE frame.
// We use it to ensure that an Nghttp2Stream instance is allocated to store
// the state.
int Nghttp2Session::OnBeginHeadersCallback(nghttp2_session* session,
                                           const nghttp2_frame* frame,
                                           void* user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  int32_t id = (frame->hd.type == NGHTTP2_PUSH_PROMISE) ?
    frame->push_promise.promised_stream_id :
    frame->hd.stream_id;
  Nghttp2Stream* stream = handle->FindStream(id);
  if (stream == nullptr) {
    Nghttp2Stream::Init(id, handle, frame->headers.cat);
  } else {
    stream->StartHeaders(frame->headers.cat);
  }
  return 0;
}

// nghttp2 calls this once for every header name-value pair in a HEADERS
// or PUSH_PROMISE block. CONTINUATION frames are handled automatically
// and transparently so we do not need to worry about those at all.
int Nghttp2Session::OnHeaderCallback(nghttp2_session* session,
                                     const nghttp2_frame* frame,
                                     nghttp2_rcbuf *name,
                                     nghttp2_rcbuf *value,
                                     uint8_t flags,
                                     void* user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  int32_t id = (frame->hd.type == NGHTTP2_PUSH_PROMISE) ?
    frame->push_promise.promised_stream_id :
    frame->hd.stream_id;
  Nghttp2Stream* stream = handle->FindStream(id);
  nghttp2_header_list* header = header_free_list.pop();
  header->name = name;
  header->value = value;
  nghttp2_rcbuf_incref(name);
  nghttp2_rcbuf_incref(value);
  LINKED_LIST_ADD(stream->current_headers, header);
  return 0;
}

// When nghttp2 has completely processed a frame, it calls OnFrameReceive.
// It is our responsibility to delegate out from there. We can ignore most
// control frames since nghttp2 will handle those for us.
int Nghttp2Session::OnFrameReceive(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  switch (frame->hd.type) {
    case NGHTTP2_DATA:
      handle->HandleDataFrame(frame);
      break;
    case NGHTTP2_PUSH_PROMISE:
    case NGHTTP2_HEADERS:
      handle->HandleHeadersFrame(frame);
      break;
    case NGHTTP2_SETTINGS:
      // Ignore settings acknowledgements
      if ((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0)
        handle->OnSettings();
      break;
    case NGHTTP2_PRIORITY:
      handle->HandlePriorityFrame(frame);
      break;
    default:
      break;
  }
  return 0;
}

// Called when nghttp2 closes a stream, either in response to an RST_STREAM
// frame or the stream closing naturally on it's own
int Nghttp2Session::OnStreamClose(nghttp2_session *session,
                                  int32_t id,
                                  uint32_t code,
                                  void *user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  Nghttp2Stream* stream = handle->FindStream(id);
  // Intentionally ignore the callback if the stream does not exist
  if (stream != nullptr)
    stream->Close(code);
  return 0;
}

// Called by nghttp2 multiple times while processing a DATA frame
int Nghttp2Session::OnDataChunkReceived(nghttp2_session *session,
                                        uint8_t flags,
                                        int32_t id,
                                        const uint8_t *data,
                                        size_t len,
                                        void *user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  Nghttp2Stream* stream = handle->FindStream(id);
  nghttp2_data_chunk_t* chunk = data_chunk_free_list.pop();
  chunk->buf = uv_buf_init(Malloc(len), len);
  memcpy(chunk->buf.base, data, len);
  if (stream->data_chunks_tail_ == nullptr) {
    stream->data_chunks_head_ =
        stream->data_chunks_tail_ = chunk;
  } else {
    stream->data_chunks_tail_->next = chunk;
    stream->data_chunks_tail_ = chunk;
  }
  return 0;
}

// Called by nghttp2 when it needs to determine how much padding to apply
// to a DATA or HEADERS frame
ssize_t Nghttp2Session::OnSelectPadding(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        size_t maxPayloadLen,
                                        void* user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  assert(handle->HasGetPaddingCallback());
  return handle->GetPadding(frame->hd.length, maxPayloadLen);
}

// Called by nghttp2 to collect the data to pack within a DATA frame.
// The buf is the DATA frame buffer that needs to be filled with at most
// length bytes. flags is used to control what nghttp2 does next.
ssize_t Nghttp2Session::OnStreamRead(nghttp2_session* session,
                                     int32_t id,
                                     uint8_t* buf,
                                     size_t length,
                                     uint32_t* flags,
                                     nghttp2_data_source* source,
                                     void* user_data) {
  Nghttp2Session* handle = static_cast<Nghttp2Session*>(user_data);
  Nghttp2Stream* stream = handle->FindStream(id);
  size_t remaining = length;
  size_t offset = 0;

  while (stream->queue_head_ != nullptr) {
    nghttp2_stream_write_queue* head = stream->queue_head_;
    while (stream->queue_head_index_ < head->nbufs) {
      if (remaining == 0) {
        goto end;
      }

      unsigned int n = stream->queue_head_index_;
      // len is the number of bytes in head->bufs[n] that are yet to be written
      size_t len = head->bufs[n].len - stream->queue_head_offset_;
      size_t bytes_to_write = len < remaining ? len : remaining;
      memcpy(buf + offset,
             head->bufs[n].base + stream->queue_head_offset_,
             bytes_to_write);
      offset += bytes_to_write;
      remaining -= bytes_to_write;
      if (bytes_to_write < len) {
        stream->queue_head_offset_ += bytes_to_write;
      } else {
        stream->queue_head_index_++;
        stream->queue_head_offset_ = 0;
      }
    }
    stream->queue_head_offset_ = 0;
    stream->queue_head_index_ = 0;
    stream->queue_head_ = head->next;
    head->cb(head->req, 0);
    delete head;
  }
  stream->queue_tail_ = nullptr;

 end:
  int writable = stream->queue_head_ != nullptr || stream->IsWritable();
  if (offset == 0 && writable && stream->queue_head_ == nullptr) {
    return NGHTTP2_ERR_DEFERRED;
  }
  if (!writable) {
    *flags |= NGHTTP2_DATA_FLAG_EOF;

    MaybeStackBuffer<nghttp2_nv> trailers;
    handle->OnTrailers(stream, &trailers);
    if (trailers.length() > 0) {
      *flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
      nghttp2_submit_trailer(session,
                             stream->id(),
                             *trailers,
                             trailers.length());
    }
    for (size_t n = 0; n < trailers.length(); n++) {
      free(trailers[n].name);
      free(trailers[n].value);
    }
  }
  assert(offset <= length);
  return offset;
}

Freelist<nghttp2_data_chunk_t, FREELIST_MAX>
    data_chunk_free_list;

Freelist<Nghttp2Stream, FREELIST_MAX> stream_free_list;

Freelist<nghttp2_header_list, FREELIST_MAX> header_free_list;

Freelist<nghttp2_data_chunks_t, FREELIST_MAX>
    data_chunks_free_list;

Nghttp2Session::Callbacks Nghttp2Session::callback_struct_saved[2] = {
  Callbacks(false),
  Callbacks(true)
};

}  // namespace http2
}  // namespace node
