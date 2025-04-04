#include <map>
#include <memory>
#include <vector>
#include <node.h>
#include "byte_buffer.h"
#include "call.h"
#include "call_credentials.h"
#include "channel.h"
#include "completion_queue.h"
#include "grpc/grpc.h"
#include "grpc/grpc_security.h"
#include "grpc/support/alloc.h"
#include "grpc/support/log.h"
#include "grpc/support/time.h"
#include "slice.h"
#include "timeval.h"
using std::unique_ptr;
using std::shared_ptr;
using std::vector;
namespace grpc {
namespace node {
using Nan::Callback;
using Nan::EscapableHandleScope;
using Nan::HandleScope;
using Nan::Maybe;
using Nan::MaybeLocal;
using Nan::ObjectWrap;
using Nan::Persistent;
using Nan::Utf8String;
using v8::Array;
using v8::Boolean;
using v8::Exception;
using v8::External;
using v8::Function;
using v8::FunctionTemplate;
using v8::Integer;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::Uint32;
using v8::String;
using v8::Value;
Persistent<FunctionTemplate> Call::fun_tpl;
Persistent<FunctionTemplate> Call::fun_tpl;
Local<Value> nanErrorWithCode(const char *msg, grpc_call_error code) {
  EscapableHandleScope scope;
  Local<Object> err = Nan::Error(msg).As<Object>();
  Nan::Set(err, Nan::New("code").ToLocalChecked(), Nan::New<Uint32>(code));
  return scope.Escape(err);
}
bool CreateMetadataArray(Local<Object> metadata, grpc_metadata_array *array) {
  HandleScope scope;
  Local<Array> keys = Nan::GetOwnPropertyNames(metadata).ToLocalChecked();
  for (unsigned int i = 0; i < keys->Length(); i++) {
    Local<String> current_key =
        Nan::To<String>(Nan::Get(keys, i).ToLocalChecked()).ToLocalChecked();
    Local<Value> value_array = Nan::Get(metadata, current_key).ToLocalChecked();
    if (!value_array->IsArray()) {
      return false;
    }
    array->capacity += Local<Array>::Cast(value_array)->Length();
  }
  array->metadata = reinterpret_cast<grpc_metadata *>(
      gpr_zalloc(array->capacity * sizeof(grpc_metadata)));
  for (unsigned int i = 0; i < keys->Length(); i++) {
    Local<String> current_key(Nan::To<String>(keys->Get(i)).ToLocalChecked());
    Local<Array> values =
        Local<Array>::Cast(Nan::Get(metadata, current_key).ToLocalChecked());
    grpc_slice key_slice = CreateSliceFromString(current_key);
    grpc_slice key_intern_slice = grpc_slice_intern(key_slice);
    grpc_slice_unref(key_slice);
    for (unsigned int j = 0; j < values->Length(); j++) {
      Local<Value> value = Nan::Get(values, j).ToLocalChecked();
      grpc_metadata *current = &array->metadata[array->count];
      current->key = key_intern_slice;
      if (grpc_is_binary_header(key_intern_slice)) {
        if (::node::Buffer::HasInstance(value)) {
          current->value = CreateSliceFromBuffer(value);
        } else {
          return false;
        }
      } else {
        if (value->IsString()) {
          Local<String> string_value = Nan::To<String>(value).ToLocalChecked();
          current->value = CreateSliceFromString(string_value);
        } else {
          return false;
        }
      }
      array->count += 1;
    }
  }
  return true;
}
void DestroyMetadataArray(grpc_metadata_array *array) {
  for (size_t i = 0; i < array->count; i++) {
    grpc_slice_unref(array->metadata[i].value);
  }
  grpc_metadata_array_destroy(array);
}
Local<Value> ParseMetadata(const grpc_metadata_array *metadata_array) {
  EscapableHandleScope scope;
  grpc_metadata *metadata_elements = metadata_array->metadata;
  size_t length = metadata_array->count;
  Local<Object> metadata_object = Nan::New<Object>();
  for (unsigned int i = 0; i < length; i++) {
    grpc_metadata *elem = &metadata_elements[i];
    Local<String> key_string = CopyStringFromSlice(elem->key);
    Local<Array> array;
    MaybeLocal<Value> maybe_array = Nan::Get(metadata_object, key_string);
    if (maybe_array.IsEmpty() || !maybe_array.ToLocalChecked()->IsArray()) {
      array = Nan::New<Array>(0);
      Nan::Set(metadata_object, key_string, array);
    } else {
      array = Local<Array>::Cast(maybe_array.ToLocalChecked());
    }
    if (grpc_is_binary_header(elem->key)) {
      Nan::Set(array, array->Length(), CreateBufferFromSlice(elem->value));
    } else {
      Nan::Set(array, array->Length(), CopyStringFromSlice(elem->value));
    }
  }
  return scope.Escape(metadata_object);
}
Local<Value> Op::GetOpType() const {
  EscapableHandleScope scope;
  return scope.Escape(Nan::New(GetTypeString()).ToLocalChecked());
}
Op::~Op() {}
class SendMetadataOp : public Op {
public:
  SendMetadataOp() { grpc_metadata_array_init(&send_metadata); }
  ~SendMetadataOp() { DestroyMetadataArray(&send_metadata); }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(Nan::True());
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    if (!value->IsObject()) {
      return false;
    }
    MaybeLocal<Object> maybe_metadata = Nan::To<Object>(value);
    if (maybe_metadata.IsEmpty()) {
      return false;
    }
    if (!CreateMetadataArray(maybe_metadata.ToLocalChecked(), &send_metadata)) {
      return false;
    }
    out->data.send_initial_metadata.count = send_metadata.count;
    out->data.send_initial_metadata.metadata = send_metadata.metadata;
    return true;
  }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "send_metadata"; }
private:
  grpc_metadata_array send_metadata;
};
class SendMessageOp : public Op {
public:
  SendMessageOp() { send_message = NULL; }
  ~SendMessageOp() {
    if (send_message != NULL) {
      grpc_byte_buffer_destroy(send_message);
    }
  }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(Nan::True());
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    if (!::node::Buffer::HasInstance(value)) {
      return false;
    }
    Local<Object> object_value = Nan::To<Object>(value).ToLocalChecked();
    MaybeLocal<Value> maybe_flag_value =
        Nan::Get(object_value, Nan::New("grpcWriteFlags").ToLocalChecked());
    if (!maybe_flag_value.IsEmpty()) {
      Local<Value> flag_value = maybe_flag_value.ToLocalChecked();
      if (flag_value->IsUint32()) {
        Maybe<uint32_t> maybe_flag = Nan::To<uint32_t>(flag_value);
        out->flags = maybe_flag.FromMaybe(0) & GRPC_WRITE_USED_MASK;
      }
    }
    send_message = BufferToByteBuffer(value);
    out->data.send_message.send_message = send_message;
    return true;
  }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "send_message"; }
private:
  grpc_byte_buffer *send_message;
};
class SendClientCloseOp : public Op {
public:
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(Nan::True());
  }
  bool ParseOp(Local<Value> value, grpc_op *out) { return true; }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "client_close"; }
};
class SendServerStatusOp : public Op {
public:
  SendServerStatusOp() { grpc_metadata_array_init(&status_metadata); }
  ~SendServerStatusOp() {
    grpc_slice_unref(details);
    DestroyMetadataArray(&status_metadata);
  }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(Nan::True());
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    if (!value->IsObject()) {
      return false;
    }
    Local<Object> server_status = Nan::To<Object>(value).ToLocalChecked();
    MaybeLocal<Value> maybe_metadata =
        Nan::Get(server_status, Nan::New("metadata").ToLocalChecked());
    if (maybe_metadata.IsEmpty()) {
      return false;
    }
    if (!maybe_metadata.ToLocalChecked()->IsObject()) {
      return false;
    }
    Local<Object> metadata =
        Nan::To<Object>(maybe_metadata.ToLocalChecked()).ToLocalChecked();
    MaybeLocal<Value> maybe_code =
        Nan::Get(server_status, Nan::New("code").ToLocalChecked());
    if (maybe_code.IsEmpty()) {
      return false;
    }
    if (!maybe_code.ToLocalChecked()->IsUint32()) {
      return false;
    }
    uint32_t code = Nan::To<uint32_t>(maybe_code.ToLocalChecked()).FromJust();
    MaybeLocal<Value> maybe_details =
        Nan::Get(server_status, Nan::New("details").ToLocalChecked());
    if (maybe_details.IsEmpty()) {
      return false;
    }
    if (!maybe_details.ToLocalChecked()->IsString()) {
      return false;
    }
    Local<String> details =
        Nan::To<String>(maybe_details.ToLocalChecked()).ToLocalChecked();
    if (!CreateMetadataArray(metadata, &status_metadata)) {
      return false;
    }
    out->data.send_status_from_server.trailing_metadata_count =
        status_metadata.count;
    out->data.send_status_from_server.trailing_metadata =
        status_metadata.metadata;
    out->data.send_status_from_server.status =
        static_cast<grpc_status_code>(code);
    this->details = CreateSliceFromString(details);
    out->data.send_status_from_server.status_details = &this->details;
    return true;
  }
  bool IsFinalOp() { return true; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "send_status"; }
private:
  grpc_slice details;
  grpc_metadata_array status_metadata;
};
class GetMetadataOp : public Op {
public:
  GetMetadataOp() { grpc_metadata_array_init(&recv_metadata); }
  ~GetMetadataOp() { grpc_metadata_array_destroy(&recv_metadata); }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(ParseMetadata(&recv_metadata));
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    out->data.recv_initial_metadata.recv_initial_metadata = &recv_metadata;
    return true;
  }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "metadata"; }
private:
  grpc_metadata_array recv_metadata;
};
class ReadMessageOp : public Op {
public:
  ReadMessageOp() { recv_message = NULL; }
  ~ReadMessageOp() {
    if (recv_message != NULL) {
      grpc_byte_buffer_destroy(recv_message);
    }
  }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(ByteBufferToBuffer(recv_message));
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    out->data.recv_message.recv_message = &recv_message;
    return true;
  }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "read"; }
private:
  grpc_byte_buffer *recv_message;
};
class ClientStatusOp : public Op {
public:
  ClientStatusOp() { grpc_metadata_array_init(&metadata_array); }
  ~ClientStatusOp() { grpc_metadata_array_destroy(&metadata_array); }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    out->data.recv_status_on_client.trailing_metadata = &metadata_array;
    out->data.recv_status_on_client.status = &status;
    out->data.recv_status_on_client.status_details = &status_details;
    return true;
  }
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    Local<Object> status_obj = Nan::New<Object>();
    Nan::Set(status_obj, Nan::New("code").ToLocalChecked(),
             Nan::New<Number>(status));
    Nan::Set(status_obj, Nan::New("details").ToLocalChecked(),
             CopyStringFromSlice(status_details));
    Nan::Set(status_obj, Nan::New("metadata").ToLocalChecked(),
             ParseMetadata(&metadata_array));
    return scope.Escape(status_obj);
  }
  bool IsFinalOp() { return true; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "status"; }
private:
  grpc_metadata_array metadata_array;
  grpc_status_code status;
  grpc_slice status_details;
};
class ServerCloseResponseOp : public Op {
public:
  Local<Value> GetNodeValue() const {
    EscapableHandleScope scope;
    return scope.Escape(Nan::New<Boolean>(cancelled));
  }
  bool ParseOp(Local<Value> value, grpc_op *out) {
    out->data.recv_close_on_server.cancelled = &cancelled;
    return true;
  }
  bool IsFinalOp() { return false; }
  void OnComplete(bool success) {}
protected:
  std::string GetTypeString() const { return "cancelled"; }
private:
  int cancelled;
};
tag::tag(Callback *callback, OpVec *ops, Call *call, Local<Value> call_value)
    : callback(callback), ops(ops), call(call) {
  HandleScope scope;
  call_persist.Reset(call_value);
}
tag::~tag() {
  delete callback;
  delete ops;
}{
  delete callback;
  delete ops;
}
void CompleteTag(void *tag, const char *error_message) {
  HandleScope scope;
  struct tag *tag_struct = reinterpret_cast<struct tag *>(tag);
  Callback *callback = tag_struct->callback;
  if (error_message == NULL) {
    Local<Object> tag_obj = Nan::New<Object>();
    for (vector<unique_ptr<Op> >::iterator it = tag_struct->ops->begin();
         it != tag_struct->ops->end(); ++it) {
      Op *op_ptr = it->get();
      Nan::Set(tag_obj, op_ptr->GetOpType(), op_ptr->GetNodeValue());
    }
    Local<Value> argv[] = {Nan::Null(), tag_obj};
    callback->Call(2, argv);
  } else {
    Local<Value> argv[] = {Nan::Error(error_message)};
    callback->Call(1, argv);
  }
  bool success = (error_message == NULL);
  bool is_final_op = false;
  for (vector<unique_ptr<Op> >::iterator it = tag_struct->ops->begin();
       it != tag_struct->ops->end(); ++it) {
    Op *op_ptr = it->get();
    op_ptr->OnComplete(success);
    if (op_ptr->IsFinalOp()) {
      is_final_op = true;
    }
  }
  if (tag_struct->call == NULL) {
    return;
  }
  tag_struct->call->CompleteBatch(is_final_op);
}
void DestroyTag(void *tag) {
  struct tag *tag_struct = reinterpret_cast<struct tag *>(tag);
  delete tag_struct;
}
void Call::DestroyCall() {
  if (this->wrapped_call != NULL) {
    grpc_call_unref(this->wrapped_call);
    this->wrapped_call = NULL;
  }
}
Call::Call(grpc_call *call)
    : wrapped_call(call), pending_batches(0), has_final_op_completed(false) <<<<<<< HEAD
{}
||||||| 0bb3986ffa
{
}
=======
{
  peer = grpc_call_get_peer(call);
}
>>>>>>> 3410c8d7
Call::~Call() <<<<<<< HEAD
{ DestroyCall(); }
||||||| 0bb3986ffa
{
  DestroyCall();
}
=======
{
  DestroyCall();
  gpr_free(peer);
}
>>>>>>> 3410c8d7
void Call::Init(Local<Object> exports) {
  HandleScope scope;
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);
  tpl->SetClassName(Nan::New("Call").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  Nan::SetPrototypeMethod(tpl, "startBatch", StartBatch);
  Nan::SetPrototypeMethod(tpl, "cancel", Cancel);
  Nan::SetPrototypeMethod(tpl, "cancelWithStatus", CancelWithStatus);
  Nan::SetPrototypeMethod(tpl, "getPeer", GetPeer);
  Nan::SetPrototypeMethod(tpl, "setCredentials", SetCredentials);
  fun_tpl.Reset(tpl);
  Local<Function> ctr = Nan::GetFunction(tpl).ToLocalChecked();
  Nan::Set(exports, Nan::New("Call").ToLocalChecked(), ctr);
  constructor = new Callback(ctr);
}
bool Call::HasInstance(Local<Value> val) {
  HandleScope scope;
  return Nan::New(fun_tpl)->HasInstance(val);
}
Local<Value> Call::WrapStruct(grpc_call *call) {
  EscapableHandleScope scope;
  if (call == NULL) {
    return scope.Escape(Nan::Null());
  }
  const int argc = 1;
  Local<Value> argv[argc] = {
      Nan::New<External>(reinterpret_cast<void *>(call))};
  MaybeLocal<Object> maybe_instance =
      Nan::NewInstance(constructor->GetFunction(), argc, argv);
  if (maybe_instance.IsEmpty()) {
    return scope.Escape(Nan::Null());
  } else {
    return scope.Escape(maybe_instance.ToLocalChecked());
  }
}
void Call::CompleteBatch(bool is_final_op) {
  if (is_final_op) {
    this->has_final_op_completed = true;
  }
  this->pending_batches--;
  if (this->has_final_op_completed && this->pending_batches == 0) {
    this->DestroyCall();
  }
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
NAN_METHOD(Call::SetCredentials) {
  Nan::HandleScope scope;
  if (!HasInstance(info.This())) {
    return Nan::ThrowTypeError(
        "setCredentials can only be called on Call objects");
  }
  if (!CallCredentials::HasInstance(info[0])) {
    return Nan::ThrowTypeError(
        "setCredentials' first argument must be a CallCredentials");
  }
  Call *call = ObjectWrap::Unwrap<Call>(info.This());
  CallCredentials *creds_object = ObjectWrap::Unwrap<CallCredentials>(
      Nan::To<Object>(info[0]).ToLocalChecked());
  grpc_call_credentials *creds = creds_object->GetWrappedCredentials();
  grpc_call_error error = GRPC_CALL_ERROR;
  if (creds) {
    error = grpc_call_set_credentials(call->wrapped_call, creds);
  }
  info.GetReturnValue().Set(Nan::New<Uint32>(error));
}
}
}
