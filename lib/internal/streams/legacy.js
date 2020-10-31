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
  // 数据源对象
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
    // 目的流可继续写了，并且可读流可读，切换成自动读取模式
    if (source.readable && source.resume) {
      source.resume();
    }
  }
  // 监听drain事件，目的流可以消费数据了就会触发该事件
  dest.on('drain', ondrain);

  // If the 'end' option is not supplied, dest.end() will be called when
  // source gets the 'end' or 'close' events.  Only dest.end() once.
  /*
    1 dest._isStdio是true表示目的流是标准输出或标准错误（见process/stdio.js），
    2 配置的end字段代表可读流触发end或close事件时，是否自动关闭可写流，默认是自动关闭。
    如果配置了end是false，则可读流这两个事件触发时，我们需要自己关闭可写流。
    3 我们看到可读流的error事件触发时，可写流是不会被自动关闭的，需要我们自己监听可读流
    的error事件，然后手动关闭可写流。
    
    所以if的判断意思是不是标准输出或标准错误流，并且没有配置end是false的时候，会自动关闭可写流。
    而标准输出和标准错误流是在进程退出的时候才被关闭的。
  */
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
  // 可读流或者可写流触发error事件时的处理逻辑
  function onerror(er) {
    // 出错了，清除注册的事件，包括正在执行的onerror函数
    cleanup();
    // 如果用户没有监听流的error事件，则抛出错误，所以我们业务代码需要监听error事件
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
  // 源流关闭、数据读完、目的流关闭时清除注册的事件
  source.on('end', cleanup);
  source.on('close', cleanup);
  dest.on('close', cleanup);
  // 触发目的流的pipe事件
  dest.emit('pipe', source);

  // Allow for unix-like usage: A.pipe(B).pipe(C)
  return dest;
};

module.exports = Stream;
