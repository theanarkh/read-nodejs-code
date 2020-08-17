#ifndef SRC_REQ_WRAP_INL_H_
#define SRC_REQ_WRAP_INL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "req_wrap.h"
#include "async_wrap-inl.h"
#include "env-inl.h"
#include "util-inl.h"

namespace node {
/*
  object对象的internalField指向ReqWrap族对象，
  object对应的是js层一次请求的封装，ReqWrap族对象是对libuv的handle族结构体
  的请求。libuv的请求族结构体的data指针指向封装了该handle结构体的c++对象
*/
template <typename T>
ReqWrap<T>::ReqWrap(Environment* env,
                    v8::Local<v8::Object> object,
                    AsyncWrap::ProviderType provider)
    : AsyncWrap(env, object, provider) {

  // FIXME(bnoordhuis) The fact that a reinterpret_cast is needed is
  // arguably a good indicator that there should be more than one queue.
  // 保存到env的请求队列
  env->req_wrap_queue()->PushBack(reinterpret_cast<ReqWrap<uv_req_t>*>(this));
}

template <typename T>
ReqWrap<T>::~ReqWrap() {
  CHECK_EQ(req_.data, this);  // Assert that someone has called Dispatched().
  CHECK_EQ(false, persistent().IsEmpty());
  persistent().Reset();
}
// 保存上下文，libuv的请求族结构体的data指针指向封装了该结构体的c++对象
template <typename T>
void ReqWrap<T>::Dispatched() {
  req_.data = this;
}

}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_REQ_WRAP_INL_H_
