'use strict';

const errors = require('internal/errors');

function getHighWaterMark(state, options, duplexKey, isDuplex) {
  // 用户定义的阈值
  let hwm = options.highWaterMark;
  // 用户定义了，则校验是否合法
  if (hwm != null) {
    if (typeof hwm !== 'number' || !(hwm >= 0))
      throw new errors.TypeError('ERR_INVALID_OPT_VALUE', 'highWaterMark', hwm);
    return Math.floor(hwm);
  } else if (isDuplex) {// 用户没有定义公共的阈值，即读写流公用的阈值
    // 用户是否定义了单独的阈值，比如读流的阈值或者写流的阈值
    hwm = options[duplexKey];
    // 用户有定义
    if (hwm != null) {
      if (typeof hwm !== 'number' || !(hwm >= 0))
        throw new errors.TypeError('ERR_INVALID_OPT_VALUE', duplexKey, hwm);
      return Math.floor(hwm);
    }
  }

  // Default value 默认值
  return state.objectMode ? 16 : 16 * 1024;
}

module.exports = {
  getHighWaterMark
};
