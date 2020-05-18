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

// A bit simpler than readable streams.
// Implement an async ._write(chunk, encoding, cb), and it'll handle all
// the drain event emission and buffering.

'use strict';

module.exports = Writable;
Writable.WritableState = WritableState;

const util = require('util');
const internalUtil = require('internal/util');
const Stream = require('stream');
const { Buffer } = require('buffer');
const destroyImpl = require('internal/streams/destroy');
const { getHighWaterMark } = require('internal/streams/state');
const errors = require('internal/errors');

util.inherits(Writable, Stream);

function nop() {}

function WritableState(options, stream) {
  options = options || {};

  // Duplex streams are both readable and writable, but share
  // the same options object.
  // However, some cases require setting options to different
  // values for the readable and the writable sides of the duplex stream.
  // These options can be provided separately as readableXXX and writableXXX.
  var isDuplex = stream instanceof Stream.Duplex;

  // object stream flag to indicate whether or not this stream
  // contains buffers or objects.
  // 数据模式
  this.objectMode = !!options.objectMode;
  // 全双工的流默认共享objectMode配置，用户可以自己配置成非共享
  if (isDuplex)
    this.objectMode = this.objectMode || !!options.writableObjectMode;

  // the point at which write() starts returning false
  // Note: 0 is a valid value, means that we always return false if
  // the entire buffer is not flushed immediately on write()
  this.highWaterMark = getHighWaterMark(this, options, 'writableHighWaterMark',
                                        isDuplex);

  // if _final has been called
  this.finalCalled = false;

  // drain event flag. 是否需要触发drain事件，重新驱动生产者
  this.needDrain = false;
  // at the start of calling end()
  // 正在执行end流程
  this.ending = false;
  // when end() has been called, and returned
  // 是否执行完end流程
  this.ended = false;
  // when 'finish' is emitted
  this.finished = false;

  // has it been destroyed
  this.destroyed = false;

  // should we decode strings into buffers before passing to _write?
  // this is here so that some node-core streams can optimize string
  // handling at a lower level.
  var noDecode = options.decodeStrings === false;
  this.decodeStrings = !noDecode;

  // Crypto is kind of old and crusty.  Historically, its default string
  // encoding is 'binary' so we have to make this configurable.
  // Everything else in the universe uses 'utf8', though.
  this.defaultEncoding = options.defaultEncoding || 'utf8';

  // not an actual buffer we keep track of, but a measurement
  // of how much we're waiting to get pushed to some underlying
  // socket or file.
  // 待写入的数据长度或对象数
  this.length = 0;

  // a flag to see when we're in the middle of a write.
  this.writing = false;

  // when true all writes will be buffered until .uncork() call
  // 加塞，缓存生产者的数据，停止往底层写入
  this.corked = 0;

  // a flag to be able to tell if the onwrite cb is called immediately,
  // or on a later tick.  We set this to true at first, because any
  // actions that shouldn't happen until "later" should generally also
  // not happen before the first write call.
  this.sync = true;

  // a flag to know if we're processing previously buffered items, which
  // may call the _write() callback in the same tick, so that we don't
  // end up in an overlapped onwrite situation.
  // 是否正在处理缓存的数据
  this.bufferProcessing = false;

  // the callback that's passed to _write(chunk,cb)
  this.onwrite = onwrite.bind(undefined, stream);

  // the callback that the user supplies to write(chunk,encoding,cb)
  // 每次执行写入时对应的回调
  this.writecb = null;

  // the amount that is being written when _write is called.
  // 执行write的时候，本次写入的数据长度或对象是数
  this.writelen = 0;
  // 第一个缓存的buffer
  this.bufferedRequest = null;
  // 最后一个缓存的buffer
  this.lastBufferedRequest = null;

  // number of pending user-supplied write callbacks
  // this must be 0 before 'finish' can be emitted
  // 待执行的回调函数个数
  this.pendingcb = 0;

  // emit prefinish if the only thing we're waiting for is _write cbs
  // This is relevant for synchronous Transform streams
  // 是否已经触发过prefinished事件
  this.prefinished = false;

  // True if the error was already emitted and should not be thrown again
  // 是否已经触发过error事件
  this.errorEmitted = false;

  // count buffered requests
  // 缓存的buffer数
  this.bufferedRequestCount = 0;

  // allocate the first CorkedRequest, there is always
  // one allocated and free to use, and we maintain at most two
  var corkReq = { next: null, entry: null, finish: undefined };
  corkReq.finish = onCorkedFinish.bind(undefined, corkReq, this);
  // 空闲的节点，可用于缓存数据
  this.corkedRequestsFree = corkReq;
}
// 获取缓存的buffer数据
WritableState.prototype.getBuffer = function getBuffer() {
  var current = this.bufferedRequest;
  var out = [];
  while (current) {
    out.push(current);
    current = current.next;
  }
  return out;
};

Object.defineProperty(WritableState.prototype, 'buffer', {
  get: internalUtil.deprecate(function() {
    return this.getBuffer();
  }, '_writableState.buffer is deprecated. Use _writableState.getBuffer ' +
     'instead.', 'DEP0003')
});

// Test _writableState for inheritance to account for Duplex streams,
// whose prototype chain only points to Readable.
var realHasInstance;
if (typeof Symbol === 'function' && Symbol.hasInstance) {
  realHasInstance = Function.prototype[Symbol.hasInstance];
  Object.defineProperty(Writable, Symbol.hasInstance, {
    value: function(object) {
      if (realHasInstance.call(this, object))
        return true;
      if (this !== Writable)
        return false;

      return object && object._writableState instanceof WritableState;
    }
  });
} else {
  realHasInstance = function(object) {
    return object instanceof this;
  };
}

function Writable(options) {
  // Writable ctor is applied to Duplexes, too.
  // `realHasInstance` is necessary because using plain `instanceof`
  // would return false, as no `_writableState` property is attached.

  // Trying to use the custom `instanceof` for Writable here will also break the
  // Node.js LazyTransform implementation, which has a non-trivial getter for
  // `_writableState` that would lead to infinite recursion.
  if (!(realHasInstance.call(Writable, this)) &&
      !(this instanceof Stream.Duplex)) {
    return new Writable(options);
  }

  this._writableState = new WritableState(options, this);

  // legacy.
  // 可写
  this.writable = true;
  // 支持用户自定义的钩子
  if (options) {
    if (typeof options.write === 'function')
      this._write = options.write;

    if (typeof options.writev === 'function')
      this._writev = options.writev;

    if (typeof options.destroy === 'function')
      this._destroy = options.destroy;

    if (typeof options.final === 'function')
      this._final = options.final;
  }

  Stream.call(this);
}

// Otherwise people can pipe Writable streams, which is just wrong.
// 写流是生产者
Writable.prototype.pipe = function() {
  this.emit('error', new errors.Error('ERR_STREAM_CANNOT_PIPE'));
};

// 执行end方法后再执行write，触发error事件并且执行write函数的回调（即调用write的参数）
function writeAfterEnd(stream, cb) {
  var er = new errors.Error('ERR_STREAM_WRITE_AFTER_END');
  // TODO: defer error events consistently everywhere, not just the cb
  stream.emit('error', er);
  process.nextTick(cb, er);
}

// Checks that a user-supplied chunk is valid, especially for the particular
// mode the stream is in. Currently this means that `null` is never accepted
// and undefined/non-string values are only allowed in object mode.
// 校验写入的数据
function validChunk(stream, state, chunk, cb) {
  var valid = true;
  var er = false;
 // 为空或者非对象模式传的不是字符串类型的数据
  if (chunk === null) {
    er = new errors.TypeError('ERR_STREAM_NULL_VALUES');
  } else if (typeof chunk !== 'string' && !state.objectMode) {
    er = new errors.TypeError('ERR_INVALID_ARG_TYPE', 'chunk',
                              ['string', 'Buffer']);
  }
  // 触发error事件，执行write函数的回调
  if (er) {
    stream.emit('error', er);
    process.nextTick(cb, er);
    valid = false;
  }
  return valid;
}
// 用户调用的接口，可重写
Writable.prototype.write = function(chunk, encoding, cb) {
  var state = this._writableState;
  var ret = false;
  var isBuf = !state.objectMode && Stream._isUint8Array(chunk);
  // 转成buffer格式
  if (isBuf && Object.getPrototypeOf(chunk) !== Buffer.prototype) {
    chunk = Stream._uint8ArrayToBuffer(chunk);
  }
  // 参数处理，传了数据和回调，没有传编码类型
  if (typeof encoding === 'function') {
    cb = encoding;
    encoding = null;
  }
  // 是buffer类型则设置成buffer，否则如果没传则取默认编码
  if (isBuf)
    encoding = 'buffer';
  else if (!encoding)
    encoding = state.defaultEncoding;

  if (typeof cb !== 'function')
    cb = nop;
  // 正在执行end，再执行write，报错
  if (state.ending)
    writeAfterEnd(this, cb);
  else if (isBuf || validChunk(this, state, chunk, cb)) {
    // 待执行的回调数加一，即cb
    state.pendingcb++;
    // 写入或缓存，见该函数
    ret = writeOrBuffer(this, state, isBuf, chunk, encoding, cb);
  }

  return ret;
};
// 阻塞写入，执行该函数返回后的数据都缓存在内存里
Writable.prototype.cork = function() {
  var state = this._writableState;

  state.corked++;
};
// 阻塞写入的次数减一，只有当corked是0才能真正解除
Writable.prototype.uncork = function() {
  var state = this._writableState;

  if (state.corked) {
    state.corked--;
    // 没有在写入，corked=0即可以写入了；也没有在追加buffer，并且在这期间写入了buffer，则处理buffer，写入
    if (!state.writing &&
        !state.corked &&
        !state.bufferProcessing &&
        state.bufferedRequest)
      clearBuffer(this, state);
  }
};
// 设置流的编码
Writable.prototype.setDefaultEncoding = function setDefaultEncoding(encoding) {
  // node::ParseEncoding() requires lower case.
  if (typeof encoding === 'string')
    encoding = encoding.toLowerCase();
    // 校验合法性
    if (!Buffer.isEncoding(encoding))
    throw new errors.TypeError('ERR_UNKNOWN_ENCODING', encoding);
  // 设置
  this._writableState.defaultEncoding = encoding;
  return this;
};

// 缓存待写入的buffer数据
Object.defineProperty(Writable.prototype, 'writableBuffer', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get: function() {
    return this._writableState && this._writableState.getBuffer();
  }
});
// 转编码
function decodeChunk(state, chunk, encoding) {
  if (!state.objectMode &&
      state.decodeStrings !== false &&
      typeof chunk === 'string') {
    chunk = Buffer.from(chunk, encoding);
  }
  return chunk;
}
// 阈值
Object.defineProperty(Writable.prototype, 'writableHighWaterMark', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get: function() {
    return this._writableState.highWaterMark;
  }
});

// if we're already writing something, then just put this
// in the queue, and wait our turn.  Otherwise, call _write
// If we return false, then we need a drain event, so set that flag.
// 写入数据或缓存在buffer里
function writeOrBuffer(stream, state, isBuf, chunk, encoding, cb) {
  if (!isBuf) {
    var newChunk = decodeChunk(state, chunk, encoding);
    if (chunk !== newChunk) {
      isBuf = true;
      encoding = 'buffer';
      chunk = newChunk;
    }
  }
  // 对象模式的算一个
  var len = state.objectMode ? 1 : chunk.length;
  // 更新待写入数据长度或对象个数
  state.length += len;
  // 待写入的长度超过了阈值
  var ret = state.length < state.highWaterMark;
  // we must ensure that previous needDrain will not be reset to false.
  // 超过了阈值，则设置需要等待drain事件标记
  if (!ret)
    state.needDrain = true;
  // 如果正在写或者设置了阻塞则先缓存数据
  if (state.writing || state.corked) {
    // 指向当前节点
    var last = state.lastBufferedRequest;
    // 插入新的头结点
    state.lastBufferedRequest = {
      chunk,
      encoding,
      isBuf,
      callback: cb,
      next: null
    };
    // 之前还有节点的话，新的头节点的next指针指向他，形成链表
    if (last) {
      last.next = state.lastBufferedRequest;
    } else {
      // 指向buffer链表，插入第一个buffer节点的时候执行到这
      state.bufferedRequest = state.lastBufferedRequest;
    }
    // 缓存的buffer个数加一
    state.bufferedRequestCount += 1;
  } else {
    // 直接写入
    doWrite(stream, state, false, len, chunk, encoding, cb);
  }
  // 返回数据是否写入成功，写入成功包括缓存下来
  return ret;
}

function doWrite(stream, state, writev, len, chunk, encoding, cb) {
  // 本次写入的数据长度
  state.writelen = len;
  // 写入后执行的回调
  state.writecb = cb;
  // 正在写入
  state.writing = true;
  // 同步写入的标记
  state.sync = true;
  if (writev)
    // chunk为缓存待写入的buffer节点数组
    stream._writev(chunk, state.onwrite);
  else
    // 执行用户定义的写函数，onwrite是nodejs定义的，在初始化的时候设置了该函数，即下面的onwrite函数
    stream._write(chunk, encoding, state.onwrite);
  state.sync = false;
}

function onwriteError(stream, state, sync, er, cb) {
  --state.pendingcb;

  if (sync) {
    // defer the callback if we are being called synchronously
    // to avoid piling up things on the stack
    process.nextTick(cb, er);
    // this can emit finish, and it will always happen
    // after error
    process.nextTick(finishMaybe, stream, state);
    stream._writableState.errorEmitted = true;
    stream.emit('error', er);
  } else {
    // the caller expect this to happen before if
    // it is async
    cb(er);
    stream._writableState.errorEmitted = true;
    stream.emit('error', er);
    // this can emit finish, but finish must
    // always follow error
    finishMaybe(stream, state);
  }
}
// 数据写完后执行该函数，更新字段
function onwriteStateUpdate(state) {
  // 写完了，重置回调，还有多少单位的数据没有写入，数据写完，重置本次待写入的数据数为0
  state.writing = false;
  state.writecb = null;
  state.length -= state.writelen;
  state.writelen = 0;
}
// 写完时执行的回调
function onwrite(stream, er) {
  var state = stream._writableState;
  var sync = state.sync;
  // 本次写完时执行的回调
  var cb = state.writecb;
  // 重置内部字段的值
  onwriteStateUpdate(state);

  if (er)
    onwriteError(stream, state, sync, er, cb);
  else {
    // Check if we're actually ready to finish, but don't emit yet
    // 是否需要触发finish事件
    var finished = needFinish(stat);
    // 还不需要触发finish事件，并且没有设置阻塞标记，也不在处理buffer，并且有待处理的buffer，则处理buffer，进行写入
    if (!finished &&
        !state.corked &&
        !state.bufferProcessing &&
        state.bufferedRequest) {
      clearBuffer(stream, state);
    }
    // 执行afterWrite
    if (sync) {
      process.nextTick(afterWrite, stream, state, finished, cb);
    } else {
      afterWrite(stream, state, finished, cb);
    }
  }
}

function afterWrite(stream, state, finished, cb) {
  // 还没结束，看是否需要触发drain事件
  if (!finished)
    onwriteDrain(stream, state);
  // 准备执行用户回调，待执行的回调减一
  state.pendingcb--;
  cb();
  finishMaybe(stream, state);
}

// Must force callback to be called on nextTick, so that we don't
// emit 'drain' before the write() consumer gets the 'false' return
// value, and has a chance to attach a 'drain' listener.
function onwriteDrain(stream, state) {
  // 没有需要需要写了，并且流在阻塞中等待drain事件
  if (state.length === 0 && state.needDrain) {
    // 触发drain事件然后清空标记
    state.needDrain = false;
    stream.emit('drain');
  }
}

// if there's something in the buffer waiting, then process it
// 把缓存在buffer中的数据写入
function clearBuffer(stream, state) {
  // 正在处理buffer
  state.bufferProcessing = true;
  // 指向头结点
  var entry = state.bufferedRequest;
  // 快速写入，即一次把所有缓存的buffer都写入
  if (stream._writev && entry && entry.next) {
    // Fast case, write everything using _writev()
    var l = state.bufferedRequestCount;
    var buffer = new Array(l);
    // 最多两个节点
    var holder = state.corkedRequestsFree;
    holder.entry = entry;

    var count = 0;
    var allBuffers = true;
    // 把缓存的节点放到buffer数组中
    while (entry) {
      buffer[count] = entry;
      if (!entry.isBuf)
        allBuffers = false;
      entry = entry.next;
      count += 1;
    }
    buffer.allBuffers = allBuffers;

    doWrite(stream, state, true, state.length, buffer, '', holder.finish);

    // doWrite is almost always async, defer these to save a bit of time
    // as the hot path ends with doWrite
    // 待执行的cb加一，即holder.finish
    state.pendingcb++;
    // 清空缓存队列
    state.lastBufferedRequest = null;
    // 还有下一个节点则更新指针
    if (holder.next) {
      state.corkedRequestsFree = holder.next;
      holder.next = null;
    } else {
      // 没有下一个节点则恢复值，见初始化时的设置
      var corkReq = { next: null, entry: null, finish: undefined };
      corkReq.finish = onCorkedFinish.bind(undefined, corkReq, state);
      state.corkedRequestsFree = corkReq;
    }
    state.bufferedRequestCount = 0;
  } else {
    // 慢慢写，即一个个buffer写，写完后等需要执行用户的cb，驱动下一个写
    // Slow case, write chunks one-by-one
    while (entry) {
      var chunk = entry.chunk;
      var encoding = entry.encoding;
      var cb = entry.callback;
      var len = state.objectMode ? 1 : chunk.length;
      // 执行写入
      doWrite(stream, state, false, len, chunk, encoding, cb);
      entry = entry.next;
      // 处理完一个，减一
      state.bufferedRequestCount--;
      // if we didn't call the onwrite immediately, then
      // it means that we need to wait until it does.
      // also, that means that the chunk and cb are currently
      // being processed, so move the buffer counter past them.
      // 在onwrite里清除这个标记，onwrite依赖于用户执行，如果用户没调，或者不是同步调，则退出
      if (state.writing) {
        break;
      }
    }

    if (entry === null)
      state.lastBufferedRequest = null;
  }

  state.bufferedRequest = entry;
  // 处理完毕
  state.bufferProcessing = false;
}

Writable.prototype._write = function(chunk, encoding, cb) {
  cb(new errors.Error('ERR_METHOD_NOT_IMPLEMENTED', '_transform'));
};

Writable.prototype._writev = null;

Writable.prototype.end = function(chunk, encoding, cb) {
  var state = this._writableState;

  if (typeof chunk === 'function') {
    cb = chunk;
    chunk = null;
    encoding = null;
  } else if (typeof encoding === 'function') {
    cb = encoding;
    encoding = null;
  }
  // 最后一次写入的机会
  if (chunk !== null && chunk !== undefined)
    this.write(chunk, encoding);

  // .end() fully uncorks
  if (state.corked) {
    // 置1，为了uncork能正确执行,可以有机会写入缓存的数据
    state.corked = 1;
    this.uncork();
  }

  // ignore unnecessary end() calls.
  if (!state.ending)
    endWritable(this, state, cb);
};
// 待写的数据长度
Object.defineProperty(Writable.prototype, 'writableLength', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get() {
    return this._writableState.length;
  }
});

function needFinish(state) {
  // 执行了end函数则设置ending=true，当前没有数据需要写入了，也没有缓存的数据，还没有出发finish，没有正在进行写入
  return (state.ending &&
          state.length === 0 &&
          state.bufferedRequest === null &&
          !state.finished &&
          !state.writing);
}
function callFinal(stream, state) {
  // 执行用户的final函数
  stream._final((err) => {
    // 执行了callFinal函数，cb减一
    state.pendingcb--;
    if (err) {
      stream.emit('error', err);
    }
    // 执行prefinish
    state.prefinished = true;
    stream.emit('prefinish');
    // 是否可以触发finish事件
    finishMaybe(stream, state);
  });
}
function prefinish(stream, state) {
  // 还没触发prefinish并且没有执行finalcall
  if (!state.prefinished && !state.finalCalled) {
    // 用户传了final函数则，待执行回调数加一，即callFinal，否则直接触发prefinish
    if (typeof stream._final === 'function') {
      state.pendingcb++;
      state.finalCalled = true;
      process.nextTick(callFinal, stream, state);
    } else {
      state.prefinished = true;
      stream.emit('prefinish');
    }
  }
}

function finishMaybe(stream, state) {
  // 流是否已经结束
  var need = needFinish(state);
  // 是则先处理prefinish事件
  if (need) {
    prefinish(stream, state);
    // 如果没有待执行的回调，则触发finish事件
    if (state.pendingcb === 0) {
      state.finished = true;
      stream.emit('finish');
    }
  }
  return need;
}

function endWritable(stream, state, cb) {
  // 正在执行end函数
  state.ending = true;
  finishMaybe(stream, state);
  if (cb) {
    // 已经触发了finish事件则下一个tick直接执行cb，否则等待finish事件
    if (state.finished)
      process.nextTick(cb);
    else
      stream.once('finish', cb);
  }
  // 流结束，流不可写
  state.ended = true;
  stream.writable = false;
}
// 一次性写入后逐个执行用户的回调
function onCorkedFinish(corkReq, state, err) {
  // corkReq.entry执行当前处理的buffer链表头结点
  var entry = corkReq.entry;
  corkReq.entry = null;
  // 遍历执行用户传入的回调回调
  while (entry) {
    var cb = entry.callback;
    state.pendingcb--;
    cb(err);
    entry = entry.next;
  }

  // reuse the free corkReq.
  state.corkedRequestsFree.next = corkReq;
}

Object.defineProperty(Writable.prototype, 'destroyed', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get() {
    if (this._writableState === undefined) {
      return false;
    }
    return this._writableState.destroyed;
  },
  set(value) {
    // we ignore the value if the stream
    // has not been initialized yet
    if (!this._writableState) {
      return;
    }

    // backward compatibility, the user is explicitly
    // managing destroyed
    this._writableState.destroyed = value;
  }
});

Writable.prototype.destroy = destroyImpl.destroy;
Writable.prototype._undestroy = destroyImpl.undestroy;
Writable.prototype._destroy = function(err, cb) {
  this.end();
  cb(err);
};
