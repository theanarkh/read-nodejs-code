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

'use strict';

module.exports = Readable;
Readable.ReadableState = ReadableState;

const EE = require('events');
const Stream = require('stream');
const { Buffer } = require('buffer');
const util = require('util');
const debug = util.debuglog('stream');
const BufferList = require('internal/streams/BufferList');
const destroyImpl = require('internal/streams/destroy');
const { getHighWaterMark } = require('internal/streams/state');
const errors = require('internal/errors');
const ReadableAsyncIterator = require('internal/streams/async_iterator');
const { emitExperimentalWarning } = require('internal/util');
var StringDecoder;

util.inherits(Readable, Stream);

const kProxyEvents = ['error', 'close', 'destroy', 'pause', 'resume'];

function prependListener(emitter, event, fn) {
  // Sadly this is not cacheable as some libraries bundle their own
  // event emitter implementation with them.
  if (typeof emitter.prependListener === 'function')
    return emitter.prependListener(event, fn);

  // This is a hack to make sure that our error handler is attached before any
  // userland ones.  NEVER DO THIS. This is here only because this code needs
  // to continue to work with older versions of Node.js that do not include
  // the prependListener() method. The goal is to eventually remove this hack.
  if (!emitter._events || !emitter._events[event])
    emitter.on(event, fn);
  else if (Array.isArray(emitter._events[event]))
    emitter._events[event].unshift(fn);
  else
    emitter._events[event] = [fn, emitter._events[event]];
}

function ReadableState(options, stream) {
  options = options || {};

  // Duplex streams are both readable and writable, but share
  // the same options object.
  // However, some cases require setting options to different
  // values for the readable and the writable sides of the duplex stream.
  // These options can be provided separately as readableXXX and writableXXX.
  // 是否是双向流
  var isDuplex = stream instanceof Stream.Duplex;

  // object stream flag. Used to make read(n) ignore n and to
  // make all the buffer merging and length checks go away
  // 数据模式
  this.objectMode = !!options.objectMode;
  // 双向流的时候，设置读端的模式
  if (isDuplex)
    this.objectMode = this.objectMode || !!options.readableObjectMode;
  // 读到highWaterMark个字节则停止，对象模式的话则是16个对象
  // the point at which it stops calling _read() to fill the buffer
  // Note: 0 is a valid value, means "don't call _read preemptively ever"
  this.highWaterMark = getHighWaterMark(this, options, 'readableHighWaterMark',
                                        isDuplex);

  // A linked list is used to store data chunks instead of an array because the
  // linked list can remove elements from the beginning faster than
  // array.shift()
  // 存储数据的缓冲区
  this.buffer = new BufferList();
  // 可读数据的长度
  this.length = 0;
  // 管道的目的源和个数
  this.pipes = null;
  this.pipesCount = 0;
  // 工作模式
  this.flowing = null;
  // 流是否已经结束
  this.ended = false;
  // 是否触发过end事件了
  this.endEmitted = false;
  // 是否正在读取数据
  this.reading = false;

  // a flag to be able to tell if the event 'readable'/'data' is emitted
  // immediately, or on a later tick.  We set this to true at first, because
  // any actions that shouldn't happen until "later" should generally also
  // not happen before the first read call.
  this.sync = true;

  // whenever we return null, then we set a flag to say
  // that we're awaiting a 'readable' event emission.
  // 是否需要触发readable事件
  this.needReadable = false;
  // 是否触发了readable事件
  this.emittedReadable = false;
  // 是否监听了readable事件
  this.readableListening = false;
  // 是否正在执行resume的过程
  this.resumeScheduled = false;

  // has it been destroyed
  // 流是否已销毁
  this.destroyed = false;

  // Crypto is kind of old and crusty.  Historically, its default string
  // encoding is 'binary' so we have to make this configurable.
  // Everything else in the universe uses 'utf8', though.
  // 数据编码格式
  this.defaultEncoding = options.defaultEncoding || 'utf8';

  // the number of writers that are awaiting a drain event in .pipe()s
  // 在管道化中，有多少个写者已经达到阈值，需要等待触发drain事件,awaitDrain记录达到阈值的写者个数
  this.awaitDrain = 0;

  // if true, a maybeReadMore has been scheduled
  // 执行maybeReadMore函数的时候，设置为true
  this.readingMore = false;

  this.decoder = null;
  this.encoding = null;
  if (options.encoding) {
    if (!StringDecoder)
      StringDecoder = require('string_decoder').StringDecoder;
    this.decoder = new StringDecoder(options.encoding);
    this.encoding = options.encoding;
  }
}

function Readable(options) {
  if (!(this instanceof Readable))
    return new Readable(options);

  this._readableState = new ReadableState(options, this);

  // legacy
  this.readable = true;

  if (options) {
    if (typeof options.read === 'function')
      this._read = options.read;

    if (typeof options.destroy === 'function')
      this._destroy = options.destroy;
  }

  Stream.call(this);
}

Object.defineProperty(Readable.prototype, 'destroyed', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get() {
    if (this._readableState === undefined) {
      return false;
    }
    return this._readableState.destroyed;
  },
  set(value) {
    // we ignore the value if the stream
    // has not been initialized yet
    if (!this._readableState) {
      return;
    }

    // backward compatibility, the user is explicitly
    // managing destroyed
    this._readableState.destroyed = value;
  }
});

Readable.prototype.destroy = destroyImpl.destroy;
Readable.prototype._undestroy = destroyImpl.undestroy;
Readable.prototype._destroy = function(err, cb) {
  this.push(null);
  cb(err);
};

// Manually shove something into the read() buffer.
// This returns true if the highWaterMark has not been hit yet,
// similar to how Writable.write() returns true if you should
// write() some more.
Readable.prototype.push = function(chunk, encoding) {
  var state = this._readableState;
  var skipChunkCheck;

  if (!state.objectMode) {
    if (typeof chunk === 'string') {
      encoding = encoding || state.defaultEncoding;
      if (encoding !== state.encoding) {
        chunk = Buffer.from(chunk, encoding);
        encoding = '';
      }
      skipChunkCheck = true;
    }
  } else {
    skipChunkCheck = true;
  }

  return readableAddChunk(this, chunk, encoding, false, skipChunkCheck);
};

// Unshift should *always* be something directly out of read()
Readable.prototype.unshift = function(chunk) {
  return readableAddChunk(this, chunk, null, true, false);
};

function readableAddChunk(stream, chunk, encoding, addToFront, skipChunkCheck) {
  var state = stream._readableState;
  // push null代表流结束
  if (chunk === null) {
    state.reading = false;
    onEofChunk(stream, state);
  } else {
    var er;
    // 校验内容
    if (!skipChunkCheck)
      er = chunkInvalid(state, chunk);
    if (er) {
      stream.emit('error', er);
    } else if (state.objectMode || chunk && chunk.length > 0) {
      if (typeof chunk !== 'string' &&
          !state.objectMode &&
          Object.getPrototypeOf(chunk) !== Buffer.prototype) {
        chunk = Stream._uint8ArrayToBuffer(chunk);
      }

      if (addToFront) {
        if (state.endEmitted)
          stream.emit('error',
                      new errors.Error('ERR_STREAM_UNSHIFT_AFTER_END_EVENT'));
        else
          addChunk(stream, state, chunk, true);
      } else if (state.ended) {
        // 
        stream.emit('error', new errors.Error('ERR_STREAM_PUSH_AFTER_EOF'));
      } else {
        state.reading = false;
        // 字符编码处理
        if (state.decoder && !encoding) {
          chunk = state.decoder.write(chunk);
          // 对象模式或者有效字符串则追加，否则是不需要处理的字符串则不需要追加
          if (state.objectMode || chunk.length !== 0)
            addChunk(stream, state, chunk, false);
          else
            maybeReadMore(stream, state);
        } else {
          addChunk(stream, state, chunk, false);
        }
      }
    } else if (!addToFront) {
      state.reading = false;
    }
  }

  return needMoreData(state);
}

function addChunk(stream, state, chunk, addToFront) {
  // 是流模式并且缓存没有数据，则直接触发data事件
  if (state.flowing && state.length === 0 && !state.sync) {
    stream.emit('data', chunk);
  } else {
    // update the buffer info.
    // 否则先把数据缓存起来
    state.length += state.objectMode ? 1 : chunk.length;
    if (addToFront)
      state.buffer.unshift(chunk);
    else
      state.buffer.push(chunk);

    if (state.needReadable)
      emitReadable(stream);
  }
  maybeReadMore(stream, state);
}

function chunkInvalid(state, chunk) {
  var er;
  if (!Stream._isUint8Array(chunk) &&
      typeof chunk !== 'string' &&
      chunk !== undefined &&
      !state.objectMode) {
    er = new errors.TypeError('ERR_INVALID_ARG_TYPE',
                              'chunk', ['string', 'Buffer', 'Uint8Array']);
  }
  return er;
}


// if it's past the high water mark, we can push in some more.
// Also, if we have no data yet, we can stand some
// more bytes.  This is to work around cases where hwm=0,
// such as the repl.  Also, if the push() triggered a
// readable event, and the user called read(largeNumber) such that
// needReadable was set, then we ought to push more, so that another
// 'readable' event will be triggered.
function needMoreData(state) {
  return !state.ended &&
         (state.needReadable ||
          state.length < state.highWaterMark ||
          state.length === 0);
}

Readable.prototype.isPaused = function() {
  return this._readableState.flowing === false;
};

// backwards compatibility.
Readable.prototype.setEncoding = function(enc) {
  if (!StringDecoder)
    StringDecoder = require('string_decoder').StringDecoder;
  this._readableState.decoder = new StringDecoder(enc);
  this._readableState.encoding = enc;
  return this;
};

// Don't raise the hwm > 8MB
const MAX_HWM = 0x800000;
function computeNewHighWaterMark(n) {
  // 不能超过阈值
  if (n >= MAX_HWM) {
    n = MAX_HWM;
  } else {
    // Get the next highest power of 2 to prevent increasing hwm excessively in
    // tiny amounts
    n--;
    n |= n >>> 1;
    n |= n >>> 2;
    n |= n >>> 4;
    n |= n >>> 8;
    n |= n >>> 16;
    n++;
  }
  return n;
}

// This function is designed to be inlinable, so please take care when making
// changes to the function body.
function howMuchToRead(n, state) {
  // 流结束并且也没有缓存的数据了，或者要读取的大小是0
  if (n <= 0 || (state.length === 0 && state.ended))
    return 0;
  // 对象模式，逐个读
  if (state.objectMode)
    return 1;
  // n是NAN
  if (n !== n) {
    // Only flow one buffer at a time
    // 流模式并且还有缓存的数据，则逐个buffer读取，返回每个buffer里缓存的数据大小
    if (state.flowing && state.length)
      return state.buffer.head.data.length;
    else
      // 否则返回缓存的长度，一次性读取，也可能当前缓存数据的大小是0
      return state.length;
  }
  // If we're asking for more than the current hwm, then raise the hwm.
  // 要读的大于当前阈值，在小于硬阈值的情况下，更新阈值
  if (n > state.highWaterMark)
    state.highWaterMark = computeNewHighWaterMark(n);
  // 要读取的比缓存的少，则直接返回
  if (n <= state.length)
    return n;
  // Don't have enough
  // 要读取的比缓存的多，如果流还没结束则返回0，等待readable事件，后续再读
  if (!state.ended) {
    state.needReadable = true;
    return 0;
  }
  // 要读的比缓存的多，但是流已经结束，则返回缓存里的
  return state.length;
}

// you can override either this method, or the async _read(n) below.
Readable.prototype.read = function(n) {
  debug('read', n);
  n = parseInt(n, 10);
  var state = this._readableState;
  var nOrig = n;
  // 执行了read，置触发readable事件的标记位false，这样下次再次触发readable事件，用户继续执行read
  if (n !== 0)
    state.emittedReadable = false;

  // if we're doing read(0) to trigger a readable event, but we
  // already have a bunch of data in the buffer, then just trigger
  // the 'readable' event and move on.
  // n是0，需要读取数据，但是缓存的数据过载或者流已经结束
  if (n === 0 &&
      state.needReadable &&
      (state.length >= state.highWaterMark || state.ended)) {
    debug('read: emitReadable', state.length, state.ended);
    // 流已经结束并且也没有缓存的数据了，触发end
    if (state.length === 0 && state.ended)
      endReadable(this);
    else
      // 没有结束或者还有缓存的数据，触发readable事件读取数据
      emitReadable(this);
    return null;
  }
  // 计算可读的大小
  n = howMuchToRead(n, state);

  // if we've ended, and we're now clear, then finish it up.
  // 如读取的大小是0并且流结束了，返回null
  if (n === 0 && state.ended) {
    // n=0不代表缓存没有数据了，见howMuchToRead，如果是缓存里没有数据了，则触发end事件
    if (state.length === 0)
      endReadable(this);
    return null;
  }

  // All the actual chunk generation logic needs to be
  // *below* the call to _read.  The reason is that in certain
  // synthetic stream cases, such as passthrough streams, _read
  // may be a completely synchronous operation which may change
  // the state of the read buffer, providing enough data when
  // before there was *not* enough.
  //
  // So, the steps are:
  // 1. Figure out what the state of things will be after we do
  // a read from the buffer.
  //
  // 2. If that resulting state will trigger a _read, then call _read.
  // Note that this may be asynchronous, or synchronous.  Yes, it is
  // deeply ugly to write APIs this way, but that still doesn't mean
  // that the Readable class should behave improperly, as streams are
  // designed to be sync/async agnostic.
  // Take note if the _read call is sync or async (ie, if the read call
  // has returned yet), so that we know whether or not it's safe to emit
  // 'readable' etc.
  //
  // 3. Actually pull the requested chunks out of the buffer and return.

  // if we need a readable event, then we need to do some reading.
  // 是否在等待数据 
  var doRead = state.needReadable;
  debug('need readable', doRead);

  // if we currently have less than the highWaterMark, then also read some
  // 如果缓存里没有数据或者读完后小于阈值了，则可以继续读
  if (state.length === 0 || state.length - n < state.highWaterMark) {
    doRead = true;
    debug('length less than watermark', doRead);
  }

  // however, if we've ended, then there's no point, and if we're already
  // reading, then it's unnecessary.
  // 流已经结束或者正在读
  if (state.ended || state.reading) {
    doRead = false;
    debug('reading or ended', doRead);
  } else if (doRead) {
    debug('do read');
    // 正在读取数据
    state.reading = true;
    state.sync = true;
    // if the length is currently zero, then we *need* a readable event.
    // 缓存里没有数据，则标记需要数据
    if (state.length === 0)
      state.needReadable = true;
    // call internal read method
    // 读取大小为阈值的数据
    this._read(state.highWaterMark);
    state.sync = false;
    // If _read pushed data synchronously, then `reading` will be false,
    // and we need to re-evaluate how much data we can return to the user.
    if (!state.reading)
      n = howMuchToRead(nOrig, state);
  }

  var ret;
  // 需要读取的大于0，则取读取数据到ret返回
  if (n > 0)
    ret = fromList(n, state);
  else
    ret = null;
  // 没有数据可读则置等待标记
  if (ret === null) {
    state.needReadable = true;
    n = 0;
  } else {
    // 减去刚读取的长度
    state.length -= n;
  }
  // 当前缓存中没有数据了
  if (state.length === 0) {
    // If we have nothing in the buffer, then we want to know
    // as soon as we *do* get something into the buffer.
    // 流还没有结束，则置等待数据标记
    if (!state.ended)
      state.needReadable = true;

    // If we tried to read() past the EOF, then emit end on the next tick.
    if (nOrig !== n && state.ended)
      endReadable(this);
  }
  // 读取数据触发data事件
  if (ret !== null)
    this.emit('data', ret);

  return ret;
};
// 读流结束，通过push(null)触发
function onEofChunk(stream, state) {
  // 已经结束则返回
  if (state.ended) return;
  // 字符编码处理
  if (state.decoder) {
    var chunk = state.decoder.end();
    if (chunk && chunk.length) {
      state.buffer.push(chunk);
      state.length += state.objectMode ? 1 : chunk.length;
    }
  }
  // 设结束标记
  state.ended = true;

  // emit 'readable' now to make sure it gets picked up.
  // 流结束，不需要读取数据了
  state.needReadable = false;
  // 当前没有在触发readable事件
  if (!state.emittedReadable) {
    state.emittedReadable = true;
    emitReadable_(stream);
  }
}

// Don't emit readable right away in sync mode, because this can trigger
// another read() call => stack overflow.  This way, it might trigger
// a nextTick recursion warning, but that's not so bad.
function emitReadable(stream) {
  var state = stream._readableState;
  state.needReadable = false;
  if (!state.emittedReadable) {
    debug('emitReadable', state.flowing);
    state.emittedReadable = true;
    process.nextTick(emitReadable_, stream);
  }
}

function emitReadable_(stream) {
  var state = stream._readableState;
  debug('emit readable');
  if (!state.destroyed && (state.length || state.ended)) {
    stream.emit('readable');
  }
  state.needReadable = !state.flowing && !state.ended;
  flow(stream);
}


// at this point, the user has presumably seen the 'readable' event,
// and called read() to consume some data.  that may have triggered
// in turn another _read(n) call, in which case reading = true if
// it's in progress.
// However, if we're not ended, or reading, and the length < hwm,
// then go ahead and try to read some more preemptively.
function maybeReadMore(stream, state) {
  // 当前不在执行readMore，则执行，设标记位
  if (!state.readingMore) {
    state.readingMore = true;
    process.nextTick(maybeReadMore_, stream, state);
  }
}

function maybeReadMore_(stream, state) {
  var len = state.length;
  // 当前是否不在读取数据，流还没结束，还没有达到阈值，则继续读
  while (!state.reading && !state.ended &&
         state.length < state.highWaterMark) {
    debug('maybeReadMore read 0');
    stream.read(0);
    // 读前读后数据不变，说明当前没有数据可读，退出
    if (len === state.length)
      // didn't get any data, stop spinning.
      break;
    else
      // 更新缓存里的数据长度，继续读
      len = state.length;
  }
  // readMore操作结束，恢复标记
  state.readingMore = false;
}

// abstract method.  to be overridden in specific implementation classes.
// call cb(er, data) where data is <= n in length.
// for virtual (non-string, non-buffer) streams, "length" is somewhat
// arbitrary, and perhaps not very meaningful.
Readable.prototype._read = function(n) {
  this.emit('error', new errors.Error('ERR_STREAM_READ_NOT_IMPLEMENTED'));
};

Readable.prototype.pipe = function(dest, pipeOpts) {
  var src = this;
  var state = this._readableState;

  switch (state.pipesCount) {
    case 0:
      state.pipes = dest;
      break;
    case 1:
      state.pipes = [state.pipes, dest];
      break;
    default:
      state.pipes.push(dest);
      break;
  }
  state.pipesCount += 1;
  debug('pipe count=%d opts=%j', state.pipesCount, pipeOpts);

  var doEnd = (!pipeOpts || pipeOpts.end !== false) &&
              dest !== process.stdout &&
              dest !== process.stderr;

  var endFn = doEnd ? onend : unpipe;
  if (state.endEmitted)
    process.nextTick(endFn);
  else
    src.once('end', endFn);

  dest.on('unpipe', onunpipe);
  function onunpipe(readable, unpipeInfo) {
    debug('onunpipe');
    if (readable === src) {
      if (unpipeInfo && unpipeInfo.hasUnpiped === false) {
        unpipeInfo.hasUnpiped = true;
        cleanup();
      }
    }
  }

  function onend() {
    debug('onend');
    dest.end();
  }

  // when the dest drains, it reduces the awaitDrain counter
  // on the source.  This would be more elegant with a .once()
  // handler in flow(), but adding and removing repeatedly is
  // too slow.
  var ondrain = pipeOnDrain(src);
  dest.on('drain', ondrain);

  var cleanedUp = false;
  function cleanup() {
    debug('cleanup');
    // cleanup event handlers once the pipe is broken
    dest.removeListener('close', onclose);
    dest.removeListener('finish', onfinish);
    dest.removeListener('drain', ondrain);
    dest.removeListener('error', onerror);
    dest.removeListener('unpipe', onunpipe);
    src.removeListener('end', onend);
    src.removeListener('end', unpipe);
    src.removeListener('data', ondata);

    cleanedUp = true;

    // if the reader is waiting for a drain event from this
    // specific writer, then it would cause it to never start
    // flowing again.
    // So, if this is awaiting a drain, then we just call it now.
    // If we don't know, then assume that we are waiting for one.
    if (state.awaitDrain &&
        (!dest._writableState || dest._writableState.needDrain))
      ondrain();
  }

  // If the user pushes more data while we're writing to dest then we'll end up
  // in ondata again. However, we only want to increase awaitDrain once because
  // dest will only emit one 'drain' event for the multiple writes.
  // => Introduce a guard on increasing awaitDrain.
  var increasedAwaitDrain = false;
  src.on('data', ondata);
  function ondata(chunk) {
    debug('ondata');
    increasedAwaitDrain = false;
    var ret = dest.write(chunk);
    debug('dest.write', ret);
    // 写入达到阈值，要等待drain事件，记录等待写端触发drain事件写者个数
    if (false === ret && !increasedAwaitDrain) {
      // If the user unpiped during `dest.write()`, it is possible
      // to get stuck in a permanently paused state if that write
      // also returned false.
      // => Check whether `dest` is still a piping destination.
      if (((state.pipesCount === 1 && state.pipes === dest) ||
           (state.pipesCount > 1 && state.pipes.indexOf(dest) !== -1)) &&
          !cleanedUp) {
        debug('false write response, pause', state.awaitDrain);
        state.awaitDrain++;
        increasedAwaitDrain = true;
      }
      // 源流暂停提供数据
      src.pause();
    }
  }

  // if the dest has an error, then stop piping into it.
  // however, don't suppress the throwing behavior for this.
  function onerror(er) {
    debug('onerror', er);
    unpipe();
    dest.removeListener('error', onerror);
    if (EE.listenerCount(dest, 'error') === 0)
      dest.emit('error', er);
  }

  // Make sure our error handler is attached before userland ones.
  prependListener(dest, 'error', onerror);

  // Both close and finish should trigger unpipe, but only once.
  function onclose() {
    dest.removeListener('finish', onfinish);
    unpipe();
  }
  dest.once('close', onclose);
  function onfinish() {
    debug('onfinish');
    dest.removeListener('close', onclose);
    unpipe();
  }
  dest.once('finish', onfinish);

  function unpipe() {
    debug('unpipe');
    src.unpipe(dest);
  }

  // tell the dest that it's being piped to
  dest.emit('pipe', src);

  // start the flow if it hasn't been started already.
  if (!state.flowing) {
    debug('pipe resume');
    src.resume();
  }

  return dest;
};

function pipeOnDrain(src) {
  return function() {
    var state = src._readableState;
    debug('pipeOnDrain', state.awaitDrain);
    if (state.awaitDrain)
      state.awaitDrain--;
    if (state.awaitDrain === 0 && EE.listenerCount(src, 'data')) {
      state.flowing = true;
      flow(src);
    }
  };
}


Readable.prototype.unpipe = function(dest) {
  var state = this._readableState;
  var unpipeInfo = { hasUnpiped: false };

  // if we're not piping anywhere, then do nothing.
  if (state.pipesCount === 0)
    return this;

  // just one destination.  most common case.
  if (state.pipesCount === 1) {
    // passed in one, but it's not the right one.
    if (dest && dest !== state.pipes)
      return this;

    if (!dest)
      dest = state.pipes;

    // got a match.
    state.pipes = null;
    state.pipesCount = 0;
    state.flowing = false;
    if (dest)
      dest.emit('unpipe', this, unpipeInfo);
    return this;
  }

  // slow case. multiple pipe destinations.

  if (!dest) {
    // remove all.
    var dests = state.pipes;
    var len = state.pipesCount;
    state.pipes = null;
    state.pipesCount = 0;
    state.flowing = false;

    for (var i = 0; i < len; i++)
      dests[i].emit('unpipe', this, unpipeInfo);
    return this;
  }

  // try to find the right one.
  var index = state.pipes.indexOf(dest);
  if (index === -1)
    return this;

  state.pipes.splice(index, 1);
  state.pipesCount -= 1;
  if (state.pipesCount === 1)
    state.pipes = state.pipes[0];

  dest.emit('unpipe', this, unpipeInfo);

  return this;
};

// set up data events if they are asked for
// Ensure readable listeners eventually get something
Readable.prototype.on = function(ev, fn) {
  const res = Stream.prototype.on.call(this, ev, fn);
  // 监听data事件的时候直接进入流模式
  if (ev === 'data') {
    // Start flowing on next tick if stream isn't explicitly paused
    if (this._readableState.flowing !== false)
      this.resume();
  } else if (ev === 'readable') {
    const state = this._readableState;
    // 流没有结束，并且没有监听readable事件
    if (!state.endEmitted && !state.readableListening) {
      // 正在监听readable事件和等待触发readable事件
      state.readableListening = state.needReadable = true;
      // 还没触发readable事件
      state.emittedReadable = false;
      // 是否正在执行read函数
      if (!state.reading) {
        // 没有在读，则准备执行读
        process.nextTick(nReadingNextTick, this);
      } else if (state.length) {
        // 正在读，并且缓存里有数据则触发readable事件
        emitReadable(this);
      }
    }
  }

  return res;
};
Readable.prototype.addListener = Readable.prototype.on;

function nReadingNextTick(self) {
  debug('readable nexttick read 0');
  self.read(0);
}

// pause() and resume() are remnants of the legacy readable stream API
// If the user uses them, then switch into old mode.
// 恢复流模式
Readable.prototype.resume = function() {
  var state = this._readableState;
  // 当前不是流模式
  if (!state.flowing) {
    debug('resume');
    // 进入流模式
    state.flowing = true;
    resume(this, state);
  }
  return this;
};

function resume(stream, state) {
  // 是否正在执行resume
  if (!state.resumeScheduled) {
    // 准备执行resume
    state.resumeScheduled = true;
    process.nextTick(resume_, stream, state);
  }
}

function resume_(stream, state) {
  debug('resume', state.reading);
  // 是否正在执行_read，不是的话执行read
  if (!state.reading) {
    stream.read(0);
  }
  // 恢复标记
  state.resumeScheduled = false;
  // 不需要等待drain事件了
  state.awaitDrain = 0;
  stream.emit('resume');
  // 开始驱动数据的读取
  flow(stream);
  //
  if (state.flowing && !state.reading)
    stream.read(0);
}
// 暂停流，并触发pause事件
Readable.prototype.pause = function() {
  debug('call pause flowing=%j', this._readableState.flowing);
  if (false !== this._readableState.flowing) {
    debug('pause');
    this._readableState.flowing = false;
    this.emit('pause');
  }
  return this;
};
// 驱动数据的读取
function flow(stream) {
  const state = stream._readableState;
  debug('flow', state.flowing);
  while (state.flowing && stream.read() !== null);
}

// wrap an old-style stream as the async data source.
// This is *not* part of the readable stream interface.
// It is an ugly unfortunate mess of history.
Readable.prototype.wrap = function(stream) {
  var state = this._readableState;
  var paused = false;

  stream.on('end', () => {
    debug('wrapped end');
    if (state.decoder && !state.ended) {
      var chunk = state.decoder.end();
      if (chunk && chunk.length)
        this.push(chunk);
    }

    this.push(null);
  });

  stream.on('data', (chunk) => {
    debug('wrapped data');
    if (state.decoder)
      chunk = state.decoder.write(chunk);

    // don't skip over falsy values in objectMode
    if (state.objectMode && (chunk === null || chunk === undefined))
      return;
    else if (!state.objectMode && (!chunk || !chunk.length))
      return;

    var ret = this.push(chunk);
    if (!ret) {
      paused = true;
      stream.pause();
    }
  });

  // proxy all the other methods.
  // important when wrapping filters and duplexes.
  for (var i in stream) {
    if (this[i] === undefined && typeof stream[i] === 'function') {
      this[i] = function(method) {
        return function() {
          return stream[method].apply(stream, arguments);
        };
      }(i);
    }
  }

  // proxy certain important events.
  for (var n = 0; n < kProxyEvents.length; n++) {
    stream.on(kProxyEvents[n], this.emit.bind(this, kProxyEvents[n]));
  }

  // when we try to consume some more bytes, simply unpause the
  // underlying stream.
  this._read = (n) => {
    debug('wrapped _read', n);
    if (paused) {
      paused = false;
      stream.resume();
    }
  };

  return this;
};

Readable.prototype[Symbol.asyncIterator] = function() {
  emitExperimentalWarning('Readable[Symbol.asyncIterator]');

  return new ReadableAsyncIterator(this);
};

Object.defineProperty(Readable.prototype, 'readableHighWaterMark', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get: function() {
    return this._readableState.highWaterMark;
  }
});

Object.defineProperty(Readable.prototype, 'readableBuffer', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get: function() {
    return this._readableState && this._readableState.buffer;
  }
});

Object.defineProperty(Readable.prototype, 'readableFlowing', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get: function() {
    return this._readableState.flowing;
  },
  set: function(state) {
    if (this._readableState) {
      this._readableState.flowing = state;
    }
  }
});

// exposed for testing purposes only.
Readable._fromList = fromList;

Object.defineProperty(Readable.prototype, 'readableLength', {
  // making it explicit this property is not enumerable
  // because otherwise some prototype manipulation in
  // userland will fail
  enumerable: false,
  get() {
    return this._readableState.length;
  }
});

// Pluck off n bytes from an array of buffers.
// Length is the combined lengths of all the buffers in the list.
// This function is designed to be inlinable, so please take care when making
// changes to the function body.
function fromList(n, state) {
  // nothing buffered
  // 缓存里没有数据
  if (state.length === 0)
    return null;

  var ret;
  // 对象模式则返回一个buffer
  if (state.objectMode)
    ret = state.buffer.shift();
  // 读取的是0或者大于缓存里的
  else if (!n || n >= state.length) {
    // read it all, truncate the list
    // 获取全部数据返回，见bufferList
    if (state.decoder)
      ret = state.buffer.join('');
    else if (state.buffer.length === 1)
      ret = state.buffer.first();
    else
      ret = state.buffer.concat(state.length);
    // 重置bufferList指针和字段
    state.buffer.clear();
  } else {
    // read part of list
    // 读取部分数据，可能跨多个buffer
    ret = state.buffer.consume(n, state.decoder);
  }

  return ret;
}

function endReadable(stream) {
  var state = stream._readableState;

  debug('endReadable', state.endEmitted);
  // 还没有触发过end
  if (!state.endEmitted) {
    // 流结束标记
    state.ended = true;
    process.nextTick(endReadableNT, state, stream);
  }
}

function endReadableNT(state, stream) {
  debug('endReadableNT', state.endEmitted, state.length);

  // Check that we didn't get one last unshift.
  // 还没有触发过end并且没有缓存的数据了
  if (!state.endEmitted && state.length === 0) {
    // 触发了end事件，流不可读
    state.endEmitted = true;
    stream.readable = false;
    stream.emit('end');
  }
}
