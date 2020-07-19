const EE = require('events');
const util = require('util');
// 流的基类
function Stream() {
  EE.call(this);
}
// 继承事件订阅分发的能力
util.inherits(Stream, EE);
// 只提供了一个方法
Stream.prototype.pipe = function(dest, options) {
  var source = this;

  function ondata(chunk) {
    // 源流有数据到达，并且目的流可写
    if (dest.writable) {
      // 目的流过载并且源流实现了pause方法，那就暂停可读流的读取操作，等待目的流触发drain事件
      if (false === dest.write(chunk) && source.pause) {
        source.pause();
      }
    }
  }
  // 监听data事件，可读流有数据的时候，会触发data事件
  source.on('data', ondata);
  function ondrain() {
    // 目的流可写了，并且可读流可读，切换成自动读取模式
    if (source.readable && source.resume) {
      source.resume();
    }
  }
  // 监听drain事件，目的流可以消费数据了就会触发该事件
  dest.on('drain', ondrain);

  // If the 'end' option is not supplied, dest.end() will be called when
  // source gets the 'end' or 'close' events.  Only dest.end() once.
  // 目的流不是标准输出或标准错误，并且end不等于false
  if (!dest._isStdio && (!options || options.end !== false)) {
    // 源流没有数据可读了，执行end回调
    source.on('end', onend);
    // 源流关闭了，执行close回调
    source.on('close', onclose);
  }

  var didOnEnd = false;
  function onend() {
    if (didOnEnd) return;
    didOnEnd = true;
	// 执行目的流的end，说明写数据完毕
    dest.end();
  }

 
  function onclose() {
    if (didOnEnd) return;
    didOnEnd = true;
    // 销毁目的流
    if (typeof dest.destroy === 'function') dest.destroy();
  }

  // don't leave dangling pipes when there are errors.
  function onerror(er) {
    // 出错了，清除注册的事件
    cleanup();
    // 如果没有监听流的error事件，则抛出错误
    if (EE.listenerCount(this, 'error') === 0) {
      throw er; // Unhandled stream error in pipe.
    }
  }

  source.on('error', onerror);
  dest.on('error', onerror);

  // remove all the event listeners that were added.
  function cleanup() {
    source.removeListener('data', ondata);
    dest.removeListener('drain', ondrain);

    source.removeListener('end', onend);
    source.removeListener('close', onclose);

    source.removeListener('error', onerror);
    dest.removeListener('error', onerror);

    source.removeListener('end', cleanup);
    source.removeListener('close', cleanup);

    dest.removeListener('close', cleanup);
  }
  // 源流关闭或者没有数据可读时，清除注册的事件
  source.on('end', cleanup);
  source.on('close', cleanup);
  // 目的流关闭了也清除他注册的事件
  dest.on('close', cleanup);
  // 触发目的流的pipe事件
  dest.emit('pipe', source);

  // Allow for unix-like usage: A.pipe(B).pipe(C)
  return dest;
};

module.exports = Stream;
