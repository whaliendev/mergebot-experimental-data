#ifndef GRPCPP_IMPL_CODEGEN_ASYNC_UNARY_CALL_IMPL_H
#define GRPCPP_IMPL_CODEGEN_ASYNC_UNARY_CALL_IMPL_H 
#include <grpcpp/impl/codegen/call.h>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/impl/codegen/client_context.h>
#include <grpcpp/impl/codegen/server_context_impl.h>
#include <grpcpp/impl/codegen/client_context_impl.h>
#include <grpcpp/impl/codegen/server_context.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/impl/codegen/status.h>
namespace grpc_impl {
template <class R>
class ClientAsyncResponseReaderInterface {
 public:
  virtual ~ClientAsyncResponseReaderInterface() {}
  virtual void StartCall() = 0;
  virtual void ReadInitialMetadata(void* tag) = 0;
  virtual void Finish(R* msg, ::grpc::Status* status, void* tag) = 0;
};
namespace internal {
template <class R>
class ClientAsyncResponseReaderFactory {
 public:
  template <class W>
  static ClientAsyncResponseReader<R>* Create(
      ::grpc::ChannelInterface* channel, ::grpc::CompletionQueue* cq,
      const ::grpc::internal::RpcMethod& method, ::grpc::ClientContext* context,
      const W& request, bool start) {
    ::grpc::internal::Call call = channel->CreateCall(method, context, cq);
    return new (::grpc::g_core_codegen_interface->grpc_call_arena_alloc(
        call.call(), sizeof(ClientAsyncResponseReader<R>)))
        ClientAsyncResponseReader<R>(call, context, request, start);
  }
};
}
template <class R>
class ClientAsyncResponseReader final
    : public ClientAsyncResponseReaderInterface<R> {
 public:
  static void operator delete(void* , std::size_t size) {
    GPR_CODEGEN_ASSERT(size == sizeof(ClientAsyncResponseReader));
  }
  static void operator delete(void*, void*) { GPR_CODEGEN_ASSERT(false); }
  void StartCall() override {
    GPR_CODEGEN_ASSERT(!started_);
    started_ = true;
    StartCallInternal();
  }
  void ReadInitialMetadata(void* tag) override {
    GPR_CODEGEN_ASSERT(started_);
    GPR_CODEGEN_ASSERT(!context_->initial_metadata_received_);
    single_buf.set_output_tag(tag);
    single_buf.RecvInitialMetadata(context_);
    call_.PerformOps(&single_buf);
    initial_metadata_read_ = true;
  }
  void Finish(R* msg, ::grpc::Status* status, void* tag) override {
    GPR_CODEGEN_ASSERT(started_);
    if (initial_metadata_read_) {
      finish_buf.set_output_tag(tag);
      finish_buf.RecvMessage(msg);
      finish_buf.AllowNoMessage();
      finish_buf.ClientRecvStatus(context_, status);
      call_.PerformOps(&finish_buf);
    } else {
      single_buf.set_output_tag(tag);
      single_buf.RecvInitialMetadata(context_);
      single_buf.RecvMessage(msg);
      single_buf.AllowNoMessage();
      single_buf.ClientRecvStatus(context_, status);
      call_.PerformOps(&single_buf);
    }
  }
 private:
  friend class internal::ClientAsyncResponseReaderFactory<R>;
  ::grpc::ClientContext* const context_;
  ::grpc::internal::Call call_;
  bool started_;
  bool initial_metadata_read_ = false;
  template <class W>
  ClientAsyncResponseReader(::grpc::internal::Call call,
                            ::grpc::ClientContext* context, const W& request,
                            bool start)
      : context_(context), call_(call), started_(start) {
    GPR_CODEGEN_ASSERT(single_buf.SendMessage(request).ok());
    single_buf.ClientSendClose();
    if (start) StartCallInternal();
  }
  void StartCallInternal() {
    single_buf.SendInitialMetadata(&context_->send_initial_metadata_,
                                   context_->initial_metadata_flags());
  }
  static void* operator new(std::size_t size);
  static void* operator new(std::size_t , void* p) { return p; }
  ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                              ::grpc::internal::CallOpSendMessage,
                              ::grpc::internal::CallOpClientSendClose,
                              ::grpc::internal::CallOpRecvInitialMetadata,
                              ::grpc::internal::CallOpRecvMessage<R>,
                              ::grpc::internal::CallOpClientRecvStatus>
      single_buf;
  ::grpc::internal::CallOpSet<::grpc::internal::CallOpRecvMessage<R>,
                              ::grpc::internal::CallOpClientRecvStatus>
      finish_buf;
};
template <class W>
class ServerAsyncResponseWriter final
    : public ::grpc::internal::ServerAsyncStreamingInterface {
 public:
  explicit ServerAsyncResponseWriter(::grpc::ServerContext* ctx)
      : call_(nullptr, nullptr, nullptr), ctx_(ctx) {}
  void SendInitialMetadata(void* tag) override {
    GPR_CODEGEN_ASSERT(!ctx_->sent_initial_metadata_);
    meta_buf_.set_output_tag(tag);
    meta_buf_.SendInitialMetadata(&ctx_->initial_metadata_,
                                  ctx_->initial_metadata_flags());
    if (ctx_->compression_level_set()) {
      meta_buf_.set_compression_level(ctx_->compression_level());
    }
    ctx_->sent_initial_metadata_ = true;
    call_.PerformOps(&meta_buf_);
  }
  void Finish(const W& msg, const ::grpc::Status& status, void* tag) {
    finish_buf_.set_output_tag(tag);
    finish_buf_.set_core_cq_tag(&finish_buf_);
    if (!ctx_->sent_initial_metadata_) {
      finish_buf_.SendInitialMetadata(&ctx_->initial_metadata_,
                                      ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        finish_buf_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
    }
    if (status.ok()) {
      finish_buf_.ServerSendStatus(&ctx_->trailing_metadata_,
                                   finish_buf_.SendMessage(msg));
    } else {
      finish_buf_.ServerSendStatus(&ctx_->trailing_metadata_, status);
    }
    call_.PerformOps(&finish_buf_);
  }
  void FinishWithError(const ::grpc::Status& status, void* tag) {
    GPR_CODEGEN_ASSERT(!status.ok());
    finish_buf_.set_output_tag(tag);
    if (!ctx_->sent_initial_metadata_) {
      finish_buf_.SendInitialMetadata(&ctx_->initial_metadata_,
                                      ctx_->initial_metadata_flags());
      if (ctx_->compression_level_set()) {
        finish_buf_.set_compression_level(ctx_->compression_level());
      }
      ctx_->sent_initial_metadata_ = true;
    }
    finish_buf_.ServerSendStatus(&ctx_->trailing_metadata_, status);
    call_.PerformOps(&finish_buf_);
  }
 private:
  void BindCall(::grpc::internal::Call* call) override { call_ = *call; }
  ::grpc::internal::Call call_;
  ::grpc::ServerContext* ctx_;
  ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata>
      meta_buf_;
  ::grpc::internal::CallOpSet<::grpc::internal::CallOpSendInitialMetadata,
                              ::grpc::internal::CallOpSendMessage,
                              ::grpc::internal::CallOpServerSendStatus>
      finish_buf_;
};
}
namespace std {
template <class R>
class default_delete<::grpc_impl::ClientAsyncResponseReader<R>> {
 public:
  void operator()(void* ) {}
};
template <class R>
class default_delete<::grpc_impl::ClientAsyncResponseReaderInterface<R>> {
 public:
  void operator()(void* ) {}
};
}
#endif
