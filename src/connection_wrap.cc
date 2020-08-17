#include "connection_wrap.h"

#include "connect_wrap.h"
#include "env-inl.h"
#include "pipe_wrap.h"
#include "stream_base-inl.h"
#include "stream_wrap.h"
#include "tcp_wrap.h"
#include "util-inl.h"

namespace node {

using v8::Boolean;
using v8::Context;
using v8::HandleScope;
using v8::Integer;
using v8::Local;
using v8::Object;
using v8::Value;


template <typename WrapType, typename UVType>
ConnectionWrap<WrapType, UVType>::ConnectionWrap(Environment* env,
                                                 Local<Object> object,
                                                 ProviderType provider)
    : LibuvStreamWrap(env,
                      object,
                      reinterpret_cast<uv_stream_t*>(&handle_),
                      provider) {}


template <typename WrapType, typename UVType>
void ConnectionWrap<WrapType, UVType>::OnConnection(uv_stream_t* handle,
                                                    int status) {
  // TCPWrap                                                    
  WrapType* wrap_data = static_cast<WrapType*>(handle->data);
  CHECK_NE(wrap_data, nullptr);
  CHECK_EQ(&wrap_data->handle_, reinterpret_cast<UVType*>(handle));

  Environment* env = wrap_data->env();
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  // We should not be getting this callback if someone has already called
  // uv_close() on the handle.
  CHECK_EQ(wrap_data->persistent().IsEmpty(), false);

  Local<Value> argv[] = {
    Integer::New(env->isolate(), status),
    Undefined(env->isolate())
  };

  if (status == 0) {
    // Instantiate the client javascript object and handle.
    // 新建一个表示和客户端通信的对象
    Local<Object> client_obj = WrapType::Instantiate(env,
                                                     wrap_data,
                                                     WrapType::SOCKET);

    // Unwrap the client javascript object.
    WrapType* wrap;
    // 解包出一个TCPWrap对象存到wrap
    ASSIGN_OR_RETURN_UNWRAP(&wrap, client_obj);
    uv_stream_t* client_handle =
        reinterpret_cast<uv_stream_t*>(&wrap->handle_);
    // uv_accept can fail if the new connection has already been closed, in
    // which case an EAGAIN (resource temporarily unavailable) will be
    // returned.
    // 把通信fd存储到client_handle中
    if (uv_accept(handle, client_handle))
      return;

    // Successful accept. Call the onconnection callback in JavaScript land.
    argv[1] = client_obj;
  }
  // 回调上层的onconnection函数
  wrap_data->MakeCallback(env->onconnection_string(), arraysize(argv), argv);
}

// 主动发起连接，成功后的回调
template <typename WrapType, typename UVType>
void ConnectionWrap<WrapType, UVType>::AfterConnect(uv_connect_t* req,
                                                    int status) {
  // data字段指向了封装该结构体的c++对象                                                    
  ConnectWrap* req_wrap = static_cast<ConnectWrap*>(req->data);
  CHECK_NE(req_wrap, nullptr);
  // handle的data字段指向了封装该handle结构体的c++对象
  WrapType* wrap = static_cast<WrapType*>(req->handle->data);
  CHECK_EQ(req_wrap->env(), wrap->env());
  Environment* env = wrap->env();

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());

  // The wrap and request objects should still be there.
  CHECK_EQ(req_wrap->persistent().IsEmpty(), false);
  CHECK_EQ(wrap->persistent().IsEmpty(), false);

  bool readable, writable;

  if (status) {
    readable = writable = 0;
  } else {
    readable = uv_is_readable(req->handle) != 0;
    writable = uv_is_writable(req->handle) != 0;
  }

  Local<Value> argv[5] = {
    Integer::New(env->isolate(), status),
    wrap->object(),
    req_wrap->object(),
    Boolean::New(env->isolate(), readable),
    Boolean::New(env->isolate(), writable)
  };

  req_wrap->MakeCallback(env->oncomplete_string(), arraysize(argv), argv);

  delete req_wrap;
}

template ConnectionWrap<PipeWrap, uv_pipe_t>::ConnectionWrap(
    Environment* env,
    Local<Object> object,
    ProviderType provider);

template ConnectionWrap<TCPWrap, uv_tcp_t>::ConnectionWrap(
    Environment* env,
    Local<Object> object,
    ProviderType provider);

template void ConnectionWrap<PipeWrap, uv_pipe_t>::OnConnection(
    uv_stream_t* handle, int status);

template void ConnectionWrap<TCPWrap, uv_tcp_t>::OnConnection(
    uv_stream_t* handle, int status);

template void ConnectionWrap<PipeWrap, uv_pipe_t>::AfterConnect(
    uv_connect_t* handle, int status);

template void ConnectionWrap<TCPWrap, uv_tcp_t>::AfterConnect(
    uv_connect_t* handle, int status);


}  // namespace node
