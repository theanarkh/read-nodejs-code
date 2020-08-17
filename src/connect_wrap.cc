#include "connect_wrap.h"

#include "env-inl.h"
#include "req_wrap-inl.h"
#include "util-inl.h"

namespace node {

using v8::Local;
using v8::Object;


ConnectWrap::ConnectWrap(Environment* env,
    Local<Object> req_wrap_obj,
    AsyncWrap::ProviderType provider) : ReqWrap(env, req_wrap_obj, provider) {
  // req_wrap_obj的internalField指向this
  Wrap(req_wrap_obj, this);
}

// 析构时解除关联关系，object不再指向this
ConnectWrap::~ConnectWrap() {
  ClearWrap(object());
}

}  // namespace node
