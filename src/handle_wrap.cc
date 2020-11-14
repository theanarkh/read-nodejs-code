// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "handle_wrap.h"
#include "async_wrap-inl.h"
#include "env-inl.h"
#include "util-inl.h"
#include "node.h"

namespace node {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Local;
using v8::Object;
using v8::Value;

// 操作handle属性的一系列函数
void HandleWrap::Ref(const FunctionCallbackInfo<Value>& args) {
  HandleWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  if (IsAlive(wrap))
    uv_ref(wrap->GetHandle());
}


void HandleWrap::Unref(const FunctionCallbackInfo<Value>& args) {
  HandleWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  if (IsAlive(wrap))
    uv_unref(wrap->GetHandle());
}


void HandleWrap::HasRef(const FunctionCallbackInfo<Value>& args) {
  HandleWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());
  args.GetReturnValue().Set(HasRef(wrap));
}

// 关闭一个handle
void HandleWrap::Close(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  HandleWrap* wrap;
  // 拿到需要关闭的handle所在的c++对象
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());

  // Guard against uninitialized handle or double close.
  if (!IsAlive(wrap))
    return;

  if (wrap->state_ != kInitialized)
    return;

  CHECK_EQ(false, wrap->persistent().IsEmpty());
  // 关闭底层资源和解除注册的事件，关闭后在libuv close阶段执行OnClose  
  uv_close(wrap->handle_, OnClose);
  // 修改状态
  wrap->state_ = kClosing;
  // 执行回调onclose
  if (args[0]->IsFunction()) {
    wrap->object()->Set(env->onclose_string(), args[0]);
    // 需要执行js回调，见OnClose
    wrap->state_ = kClosingWithCallback;
  }
}


HandleWrap::HandleWrap(Environment* env,
                       Local<Object> object,
                       uv_handle_t* handle,
                       AsyncWrap::ProviderType provider)
    : AsyncWrap(env, object, provider),
      state_(kInitialized),
      handle_(handle) {
  // handle的data字段指向封装了该handle的类对象
  handle_->data = this;
  HandleScope scope(env->isolate());
  // 关联object和this对象，后续通过unwrap使用
  Wrap(object, this);
  // 插入handle队列
  env->handle_wrap_queue()->PushBack(this);
}


HandleWrap::~HandleWrap() {
  CHECK(persistent().IsEmpty());
}


void HandleWrap::OnClose(uv_handle_t* handle) {
  // handle关联的c++对象，该c++对象封装了handle
  HandleWrap* wrap = static_cast<HandleWrap*>(handle->data);
  Environment* env = wrap->env();
  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());

  // The wrap object should still be there.
  CHECK_EQ(wrap->persistent().IsEmpty(), false);
  CHECK(wrap->state_ >= kClosing && wrap->state_ <= kClosingWithCallback);

  const bool have_close_callback = (wrap->state_ == kClosingWithCallback);
  wrap->state_ = kClosed;
  // 执行js回调
  if (have_close_callback)
    wrap->MakeCallback(env->onclose_string(), 0, nullptr);
  // 解除关联关系
  ClearWrap(wrap->object());
  wrap->persistent().Reset();
  delete wrap;
}


}  // namespace node
