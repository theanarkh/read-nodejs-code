'use strict';

// undocumented cb() API, needed for core, not for public API
function destroy(err, cb) {
  // 读流、写流、双向流
  const readableDestroyed = this._readableState &&
    this._readableState.destroyed;
  const writableDestroyed = this._writableState &&
    this._writableState.destroyed;
  // 流是否已经销毁，是则直接执行回调
  if (readableDestroyed || writableDestroyed) {
    // 传了cb，则执行，可选地传入err，用户定义的err
    if (cb) {
      cb(err);
    } else if (err &&
               (!this._writableState || !this._writableState.errorEmitted)) {
      // 传了err，是读流或者没有触发过error事件的写流，则触发error事件
      process.nextTick(emitErrorNT, this, err);
    }
    return this;
  }

  // we set destroyed to true before firing error callbacks in order
  // to make it re-entrance safe in case destroy() is called within callbacks
  // 还没有销毁则开始销毁流程
  if (this._readableState) {
    this._readableState.destroyed = true;
  }

  // if this is a duplex stream mark the writable part as destroyed as well
  if (this._writableState) {
    this._writableState.destroyed = true;
  }
  // 用户可以自定义_destroy函数
  this._destroy(err || null, (err) => {
    // 没有cb但是有error，则触发error事件
    if (!cb && err) {
      process.nextTick(emitErrorNT, this, err);
      // 可写流则标记已经触发过error事件
      if (this._writableState) {
        this._writableState.errorEmitted = true;
      }
    } else if (cb) { // 有cb或者没有err
      cb(err);
    }
  });

  return this;
}

function undestroy() {
  if (this._readableState) {
    this._readableState.destroyed = false;
    this._readableState.reading = false;
    this._readableState.ended = false;
    this._readableState.endEmitted = false;
  }

  if (this._writableState) {
    this._writableState.destroyed = false;
    this._writableState.ended = false;
    this._writableState.ending = false;
    this._writableState.finished = false;
    this._writableState.errorEmitted = false;
  }
}

function emitErrorNT(self, err) {
  self.emit('error', err);
}

module.exports = {
  destroy,
  undestroy
};
