/* Based on src/http/ngx_http_parse.c from NGINX copyright Igor Sysoev
 *
 * Additional changes are licensed under the same terms as NGINX and
 * copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "http_parser.h"
#include <assert.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifndef ULLONG_MAX
# define ULLONG_MAX ((uint64_t) -1) /* 2^64-1 */
#endif

#ifndef MIN
# define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#ifndef ARRAY_SIZE
# define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef BIT_AT
# define BIT_AT(a, i)                                                \
  (!!((unsigned int) (a)[(unsigned int) (i) >> 3] &                  \
   (1 << ((unsigned int) (i) & 7))))
#endif

#ifndef ELEM_AT
# define ELEM_AT(a, i, v) ((unsigned int) (i) < ARRAY_SIZE(a) ? (a)[(i)] : (v))
#endif

#define SET_ERRNO(e)                                                 \
do {                                                                 \
  parser->http_errno = (e);                                          \
} while(0)

#define CURRENT_STATE() p_state
#define UPDATE_STATE(V) p_state = (enum state) (V);
#define RETURN(V)                                                    \
do {                                                                 \
  parser->state = CURRENT_STATE();                                   \
  return (V);                                                        \
} while (0);
#define REEXECUTE()                                                  \
  goto reexecute;                                                    \


#ifdef __GNUC__
# define LIKELY(X) __builtin_expect(!!(X), 1)
# define UNLIKELY(X) __builtin_expect(!!(X), 0)
#else
# define LIKELY(X) (X)
# define UNLIKELY(X) (X)
#endif


/* Run the notify callback FOR, returning ER if it fails */
#define CALLBACK_NOTIFY_(FOR, ER)                                    \
do {                                                                 \
  assert(HTTP_PARSER_ERRNO(parser) == HPE_OK);                       \
                                                                     \
  if (LIKELY(settings->on_##FOR)) {                                  \
    parser->state = CURRENT_STATE();                                 \
    if (UNLIKELY(0 != settings->on_##FOR(parser))) {                 \
      SET_ERRNO(HPE_CB_##FOR);                                       \
    }                                                                \
    UPDATE_STATE(parser->state);                                     \
                                                                     \
    /* We either errored above or got paused; get out */             \
    if (UNLIKELY(HTTP_PARSER_ERRNO(parser) != HPE_OK)) {             \
      return (ER);                                                   \
    }                                                                \
  }                                                                  \
} while (0)

/* Run the notify callback FOR and consume the current byte */
#define CALLBACK_NOTIFY(FOR)            CALLBACK_NOTIFY_(FOR, p - data + 1)

/* Run the notify callback FOR and don't consume the current byte */
#define CALLBACK_NOTIFY_NOADVANCE(FOR)  CALLBACK_NOTIFY_(FOR, p - data)

/* Run data callback FOR with LEN bytes, returning ER if it fails */
#define CALLBACK_DATA_(FOR, LEN, ER)                                 \
do {                                                                 \
  assert(HTTP_PARSER_ERRNO(parser) == HPE_OK);                       \
                                                                     \
  if (FOR##_mark) {                                                  \
    if (LIKELY(settings->on_##FOR)) {                                \
      parser->state = CURRENT_STATE();                               \
      if (UNLIKELY(0 !=                                              \
                   settings->on_##FOR(parser, FOR##_mark, (LEN)))) { \
        SET_ERRNO(HPE_CB_##FOR);                                     \
      }                                                              \
      UPDATE_STATE(parser->state);                                   \
                                                                     \
      /* We either errored above or got paused; get out */           \
      if (UNLIKELY(HTTP_PARSER_ERRNO(parser) != HPE_OK)) {           \
        return (ER);                                                 \
      }                                                              \
    }                                                                \
    FOR##_mark = NULL;                                               \
  }                                                                  \
} while (0)

/* Run the data callback FOR and consume the current byte */
#define CALLBACK_DATA(FOR)                                           \
    CALLBACK_DATA_(FOR, p - FOR##_mark, p - data + 1)

/* Run the data callback FOR and don't consume the current byte */
#define CALLBACK_DATA_NOADVANCE(FOR)                                 \
    CALLBACK_DATA_(FOR, p - FOR##_mark, p - data)

/* Set the mark FOR; non-destructive if mark is already set */
#define MARK(FOR)                                                    \
do {                                                                 \
  if (!FOR##_mark) {                                                 \
    FOR##_mark = p;                                                  \
  }                                                                  \
} while (0)

/* Don't allow the total size of the HTTP headers (including the status
 * line) to exceed HTTP_MAX_HEADER_SIZE.  This check is here to protect
 * embedders against denial-of-service attacks where the attacker feeds
 * us a never-ending header that the embedder keeps buffering.
 *
 * This check is arguably the responsibility of embedders but we're doing
 * it on the embedder's behalf because most won't bother and this way we
 * make the web a little safer.  HTTP_MAX_HEADER_SIZE is still far bigger
 * than any reasonable request or response so this should never affect
 * day-to-day operation.
 */
#define COUNT_HEADER_SIZE(V)                                         \
do {                                                                 \
  parser->nread += (V);                                              \
  if (UNLIKELY(parser->nread > (HTTP_MAX_HEADER_SIZE))) {            \
    SET_ERRNO(HPE_HEADER_OVERFLOW);                                  \
    goto error;                                                      \
  }                                                                  \
} while (0)


#define PROXY_CONNECTION "proxy-connection"
#define CONNECTION "connection"
#define CONTENT_LENGTH "content-length"
#define TRANSFER_ENCODING "transfer-encoding"
#define UPGRADE "upgrade"
#define CHUNKED "chunked"
#define KEEP_ALIVE "keep-alive"
#define CLOSE "close"

// 定义http方法#string="string","GET", "POST"...
static const char *method_strings[] =
  {
#define XX(num, name, string) #string,
  HTTP_METHOD_MAP(XX)
#undef XX
  };


/* Tokens as defined by rfc 2616. Also lowercases them.
 *        token       = 1*<any CHAR except CTLs or separators>
 *     separators     = "(" | ")" | "<" | ">" | "@"
 *                    | "," | ";" | ":" | "\" | <">
 *                    | "/" | "[" | "]" | "?" | "="
 *                    | "{" | "}" | SP | HT
 */
static const char tokens[256] = {
/*   0 nul    1 soh    2 stx    3 etx    4 eot    5 enq    6 ack    7 bel  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*   8 bs     9 ht    10 nl    11 vt    12 np    13 cr    14 so    15 si   */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  16 dle   17 dc1   18 dc2   19 dc3   20 dc4   21 nak   22 syn   23 etb */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  24 can   25 em    26 sub   27 esc   28 fs    29 gs    30 rs    31 us  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  32 sp    33  !    34  "    35  #    36  $    37  %    38  &    39  '  */
        0,      '!',      0,      '#',     '$',     '%',     '&',    '\'',
/*  40  (    41  )    42  *    43  +    44  ,    45  -    46  .    47  /  */
        0,       0,      '*',     '+',      0,      '-',     '.',      0,
/*  48  0    49  1    50  2    51  3    52  4    53  5    54  6    55  7  */
       '0',     '1',     '2',     '3',     '4',     '5',     '6',     '7',
/*  56  8    57  9    58  :    59  ;    60  <    61  =    62  >    63  ?  */
       '8',     '9',      0,       0,       0,       0,       0,       0,
/*  64  @    65  A    66  B    67  C    68  D    69  E    70  F    71  G  */
        0,      'a',     'b',     'c',     'd',     'e',     'f',     'g',
/*  72  H    73  I    74  J    75  K    76  L    77  M    78  N    79  O  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/*  80  P    81  Q    82  R    83  S    84  T    85  U    86  V    87  W  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/*  88  X    89  Y    90  Z    91  [    92  \    93  ]    94  ^    95  _  */
       'x',     'y',     'z',      0,       0,       0,      '^',     '_',
/*  96  `    97  a    98  b    99  c   100  d   101  e   102  f   103  g  */
       '`',     'a',     'b',     'c',     'd',     'e',     'f',     'g',
/* 104  h   105  i   106  j   107  k   108  l   109  m   110  n   111  o  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/* 112  p   113  q   114  r   115  s   116  t   117  u   118  v   119  w  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/* 120  x   121  y   122  z   123  {   124  |   125  }   126  ~   127 del */
       'x',     'y',     'z',      0,      '|',      0,      '~',       0 };


static const int8_t unhex[256] =
  {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  , 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1
  ,-1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1
  ,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };


#if HTTP_PARSER_STRICT
# define T(v) 0
#else
# define T(v) v
#endif


static const uint8_t normal_url_char[32] = {
/*   0 nul    1 soh    2 stx    3 etx    4 eot    5 enq    6 ack    7 bel  */
        0    |   0    |   0    |   0    |   0    |   0    |   0    |   0,
/*   8 bs     9 ht    10 nl    11 vt    12 np    13 cr    14 so    15 si   */
        0    | T(2)   |   0    |   0    | T(16)  |   0    |   0    |   0,
/*  16 dle   17 dc1   18 dc2   19 dc3   20 dc4   21 nak   22 syn   23 etb */
        0    |   0    |   0    |   0    |   0    |   0    |   0    |   0,
/*  24 can   25 em    26 sub   27 esc   28 fs    29 gs    30 rs    31 us  */
        0    |   0    |   0    |   0    |   0    |   0    |   0    |   0,
/*  32 sp    33  !    34  "    35  #    36  $    37  %    38  &    39  '  */
        0    |   2    |   4    |   0    |   16   |   32   |   64   |  128,
/*  40  (    41  )    42  *    43  +    44  ,    45  -    46  .    47  /  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  48  0    49  1    50  2    51  3    52  4    53  5    54  6    55  7  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  56  8    57  9    58  :    59  ;    60  <    61  =    62  >    63  ?  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |   0,
/*  64  @    65  A    66  B    67  C    68  D    69  E    70  F    71  G  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  72  H    73  I    74  J    75  K    76  L    77  M    78  N    79  O  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  80  P    81  Q    82  R    83  S    84  T    85  U    86  V    87  W  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  88  X    89  Y    90  Z    91  [    92  \    93  ]    94  ^    95  _  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/*  96  `    97  a    98  b    99  c   100  d   101  e   102  f   103  g  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/* 104  h   105  i   106  j   107  k   108  l   109  m   110  n   111  o  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/* 112  p   113  q   114  r   115  s   116  t   117  u   118  v   119  w  */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |  128,
/* 120  x   121  y   122  z   123  {   124  |   125  }   126  ~   127 del */
        1    |   2    |   4    |   8    |   16   |   32   |   64   |   0, };

#undef T

enum state
  { s_dead = 1 /* important that this is > 0 */

  , s_start_req_or_res
  , s_res_or_resp_H
  // 开始响应请求
  , s_start_res
  // 响应头的http和版本号
  , s_res_H
  , s_res_HT
  , s_res_HTT
  , s_res_HTTP
  , s_res_first_http_major
  , s_res_http_major
  , s_res_first_http_minor
  , s_res_http_minor
  // 开始设置响应报文状态码
  , s_res_first_status_code
  //  设置响应头状态码
  , s_res_status_code
  //  开始设置响应报文的状态字符串
  , s_res_status_start
  , s_res_status
  , s_res_line_almost_done

  , s_start_req
  // 请求方法
  , s_req_method
  // 方法和url之前的字符串
  , s_req_spaces_before_url
  // 协议
  , s_req_schema
  // 协议后面的第一个斜杠
  , s_req_schema_slash
  // 协议后面的第二个斜杠
  , s_req_schema_slash_slash
  // 开始解析服务器信息状态
  , s_req_server_start
  // 解析host
  , s_req_server
  // 解析@字符，@user:password
  , s_req_server_with_at
  // 解析路径
  , s_req_path
  // 开始解析query参数
  , s_req_query_string_start
  // 解析query参数
  , s_req_query_string
  // 开始解析锚点
  , s_req_fragment_start
  // 解析锚点
  , s_req_fragment
  // 开始解析协议版本
  , s_req_http_start
  // 解析HTTP四个字符
  , s_req_http_H
  , s_req_http_HT
  , s_req_http_HTT
  , s_req_http_HTTP
  // 解析http版本（主版本，次版本）
  , s_req_first_http_major
  , s_req_http_major
  , s_req_first_http_minor
  , s_req_http_minor
  // 解析完http包第一行
  , s_req_line_almost_done
  // 开始解析http头
  , s_header_field_start
  // http头的key
  , s_header_field
  // http头的值
  , s_header_value_discard_ws
  , s_header_value_discard_ws_almost_done
  , s_header_value_discard_lws
  // 开始解析http头的值
  , s_header_value_start
  // 解析http头的值
  , s_header_value
  , s_header_value_lws
  // 解析http头完毕
  , s_header_almost_done

  , s_chunk_size_start
  , s_chunk_size
  , s_chunk_parameters
  , s_chunk_size_almost_done

  , s_headers_almost_done
  , s_headers_done

  /* Important: 's_headers_done' must be the last 'header' state. All
   * states beyond this must be 'body' states. It is used for overflow
   * checking. See the PARSING_HEADER() macro.
   */

  , s_chunk_data
  , s_chunk_data_almost_done
  , s_chunk_data_done

  , s_body_identity
  , s_body_identity_eof

  , s_message_done
  };


#define PARSING_HEADER(state) (state <= s_headers_done)


enum header_states
  { h_general = 0
  , h_C
  , h_CO
  , h_CON

  , h_matching_connection
  , h_matching_proxy_connection
  , h_matching_content_length
  , h_matching_transfer_encoding
  , h_matching_upgrade

  , h_connection
  , h_content_length
  , h_transfer_encoding
  , h_upgrade

  , h_matching_transfer_encoding_chunked
  , h_matching_connection_token_start
  , h_matching_connection_keep_alive
  , h_matching_connection_close
  , h_matching_connection_upgrade
  , h_matching_connection_token

  , h_transfer_encoding_chunked
  , h_connection_keep_alive
  , h_connection_close
  , h_connection_upgrade
  };
// 代表host字段的状态
enum http_host_state
  {
    s_http_host_dead = 1
  // 开始解析用户密码状态
  , s_http_userinfo_start
  // 解析用户密码状态，即遇到了第一个符合的字符
  , s_http_userinfo
  // 开始解析host
  , s_http_host_start
  // 开始解析v6地址，host部分可能是ip
  , s_http_host_v6_start
  // 解析host状态
  , s_http_host
  // 解析ipv6状态
  , s_http_host_v6
  // 解析v6完毕
  , s_http_host_v6_end
  , s_http_host_v6_zone_start
  , s_http_host_v6_zone
  // 开始解析端口
  , s_http_host_port_start
  // 解析端口状态，即遇到了第一个符合的字符
  , s_http_host_port
};

/* Macros for character classes; depends on strict-mode  */
#define CR                  '\r'
#define LF                  '\n'
#define LOWER(c)            (unsigned char)(c | 0x20)
#define IS_ALPHA(c)         (LOWER(c) >= 'a' && LOWER(c) <= 'z')
#define IS_NUM(c)           ((c) >= '0' && (c) <= '9')
#define IS_ALPHANUM(c)      (IS_ALPHA(c) || IS_NUM(c))
#define IS_HEX(c)           (IS_NUM(c) || (LOWER(c) >= 'a' && LOWER(c) <= 'f'))
#define IS_MARK(c)          ((c) == '-' || (c) == '_' || (c) == '.' || \
  (c) == '!' || (c) == '~' || (c) == '*' || (c) == '\'' || (c) == '(' || \
  (c) == ')')
#define IS_USERINFO_CHAR(c) (IS_ALPHANUM(c) || IS_MARK(c) || (c) == '%' || \
  (c) == ';' || (c) == ':' || (c) == '&' || (c) == '=' || (c) == '+' || \
  (c) == '$' || (c) == ',')

#define STRICT_TOKEN(c)     (tokens[(unsigned char)c])

#if HTTP_PARSER_STRICT
#define TOKEN(c)            (tokens[(unsigned char)c])
#define IS_URL_CHAR(c)      (BIT_AT(normal_url_char, (unsigned char)c))
#define IS_HOST_CHAR(c)     (IS_ALPHANUM(c) || (c) == '.' || (c) == '-')
#else
#define TOKEN(c)            ((c == ' ') ? ' ' : tokens[(unsigned char)c])
#define IS_URL_CHAR(c)                                                         \
  (BIT_AT(normal_url_char, (unsigned char)c) || ((c) & 0x80))
#define IS_HOST_CHAR(c)                                                        \
  (IS_ALPHANUM(c) || (c) == '.' || (c) == '-' || (c) == '_')
#endif

/**
 * Verify that a char is a valid visible (printable) US-ASCII
 * character or %x80-FF
 **/
#define IS_HEADER_CHAR(ch)                                                     \
  (ch == CR || ch == LF || ch == 9 || ((unsigned char)ch > 31 && ch != 127))

#define start_state (parser->type == HTTP_REQUEST ? s_start_req : s_start_res)


#if HTTP_PARSER_STRICT
# define STRICT_CHECK(cond)                                          \
do {                                                                 \
  if (cond) {                                                        \
    SET_ERRNO(HPE_STRICT);                                           \
    goto error;                                                      \
  }                                                                  \
} while (0)
# define NEW_MESSAGE() (http_should_keep_alive(parser) ? start_state : s_dead)
#else
# define STRICT_CHECK(cond)
# define NEW_MESSAGE() start_state
#endif


/* Map errno values to strings for human-readable output */
#define HTTP_STRERROR_GEN(n, s) { "HPE_" #n, s },
static struct {
  const char *name;
  const char *description;
} http_strerror_tab[] = {
  HTTP_ERRNO_MAP(HTTP_STRERROR_GEN)
};
#undef HTTP_STRERROR_GEN

int http_message_needs_eof(const http_parser *parser);

/* Our URL parser.
 *
 * This is designed to be shared by http_parser_execute() for URL validation,
 * hence it has a state transition + byte-for-byte interface. In addition, it
 * is meant to be embedded in http_parser_parse_url(), which does the dirty
 * work of turning state transitions URL components for its API.
 *
 * This function should only be invoked with non-space characters. It is
 * assumed that the caller cares about (and can detect) the transition between
 * URL and non-URL states by looking for these.
 */
static enum state
parse_url_char(enum state s, const char ch)
{
  if (ch == ' ' || ch == '\r' || ch == '\n') {
    return s_dead;
  }

#if HTTP_PARSER_STRICT
  if (ch == '\t' || ch == '\f') {
    return s_dead;
  }
#endif

  switch (s) {
    // 准备解析http请求第一行的url部分
    case s_req_spaces_before_url:
      /* Proxied requests are followed by scheme of an absolute URI (alpha).
       * All methods except CONNECT are followed by '/' or '*'.
       */
      // 遇到/说明是相对url，进入解析路径状态
      if (ch == '/' || ch == '*') {
        return s_req_path;
      }
      // 遇到字母说明是绝对url，进入解析协议状态
      if (IS_ALPHA(ch)) {
        return s_req_schema;
      }
      // url不是以上字符开头则报错
      break;
    // 正在解析协议
    case s_req_schema:
      // 还是字母，则还是维护状态
      if (IS_ALPHA(ch)) {
        return s;
      }
      // 遇到冒号说明解析协议结束，进入解析第一个斜杆状态
      if (ch == ':') {
        return s_req_schema_slash;
      }

      break;
    // 解析第一个协议状态
    case s_req_schema_slash:
      // 遇到了一个斜杆，则进入解析第二个斜杆状态
      if (ch == '/') {
        return s_req_schema_slash_slash;
      }
      // 如果不是斜杆则报错
      break;
    // 解析第二个斜杆
    case s_req_schema_slash_slash:
      // 遇到的是一个斜杆，则进入解析host状态，否则报错
      if (ch == '/') {
        return s_req_server_start;
      }

      break;

    case s_req_server_with_at:
      if (ch == '@') {
        return s_dead;
      }

    /* FALLTHROUGH */
    // 解析服务器信息状态
    case s_req_server_start:
    case s_req_server:
      // 遇到斜杆，说明服务器信息解析完毕，进入解析路径状态
      if (ch == '/') {
        return s_req_path;
      }
      // 遇到？则变成开始解析query参数状态
      if (ch == '?') {
        return s_req_query_string_start;
      }
      // 遇到@则进入解析用户名密码状态
      if (ch == '@') {
        return s_req_server_with_at;
      }

      if (IS_USERINFO_CHAR(ch) || ch == '[' || ch == ']') {
        return s_req_server;
      }

      break;
    // 解析路径状态
    case s_req_path:
      // 合法字符，则维护状态
      if (IS_URL_CHAR(ch)) {
        return s;
      }
      // 遇到了？则进入解析query参数状态，遇到#则进入解析锚点状态
      switch (ch) {
        case '?':
          return s_req_query_string_start;

        case '#':
          return s_req_fragment_start;
      }

      break;
    // 解析query参数
    case s_req_query_string_start:
    case s_req_query_string:
      // 是合法的url字符，则维护状态
      if (IS_URL_CHAR(ch)) {
        return s_req_query_string;
      }

      switch (ch) {
        // 允许query字符里有？，当做数据处理
        case '?':
          /* allow extra '?' in query string */
          return s_req_query_string;
        // 遇到锚点则变成解析锚点状态
        case '#':
          return s_req_fragment_start;
      }

      break;
    // 开始解析锚点
    case s_req_fragment_start:
      // 是合法的url字符则变成解析锚点状态
      if (IS_URL_CHAR(ch)) {
        return s_req_fragment;
      }

      switch (ch) {
        // 锚点里允许?
        case '?':
          return s_req_fragment;
        // 允许#后面又是#
        case '#':
          return s;
      }

      break;
    // 解析锚点
    case s_req_fragment:
      // 合法字符则维护状态
      if (IS_URL_CHAR(ch)) {
        return s;
      }
      // 锚点里允许下面两个字符
      switch (ch) {
        case '?':
        case '#':
          return s;
      }

      break;

    default:
      break;
  }

  /* We should never fall out of the switch above unless there's an error */
  return s_dead;
}

/*
  settings 回调钩子
  data 待解析的数据
  len 数据长度
*/
size_t http_parser_execute (http_parser *parser,
                            const http_parser_settings *settings,
                            const char *data,
                            size_t len)
{
  char c, ch;
  int8_t unhex_val;
  const char *p = data;
  const char *header_field_mark = 0;
  const char *header_value_mark = 0;
  const char *url_mark = 0;
  const char *body_mark = 0;
  const char *status_mark = 0;
  enum state p_state = (enum state) parser->state;
  const unsigned int lenient = parser->lenient_http_headers;

  /* We're in an error state. Don't bother doing anything. */
  if (HTTP_PARSER_ERRNO(parser) != HPE_OK) {
    return 0;
  }

  if (len == 0) {
    switch (CURRENT_STATE()) {
      case s_body_identity_eof:
        /* Use of CALLBACK_NOTIFY() here would erroneously return 1 byte read if
         * we got paused.
         */
        CALLBACK_NOTIFY_NOADVANCE(message_complete);
        return 0;

      case s_dead:
      case s_start_req_or_res:
      case s_start_res:
      case s_start_req:
        return 0;

      default:
        SET_ERRNO(HPE_INVALID_EOF_STATE);
        return 1;
    }
  }


  if (CURRENT_STATE() == s_header_field)
    header_field_mark = data;
  if (CURRENT_STATE() == s_header_value)
    header_value_mark = data;
  switch (CURRENT_STATE()) {
  case s_req_path:
  case s_req_schema:
  case s_req_schema_slash:
  case s_req_schema_slash_slash:
  case s_req_server_start:
  case s_req_server:
  case s_req_server_with_at:
  case s_req_query_string_start:
  case s_req_query_string:
  case s_req_fragment_start:
  case s_req_fragment:
    url_mark = data;
    break;
  case s_res_status:
    status_mark = data;
    break;
  default:
    break;
  }

  for (p=data; p != data + len; p++) {
    ch = *p;
    // 正在解析http头
    if (PARSING_HEADER(CURRENT_STATE()))
      // http头是否过大，是则报错
      COUNT_HEADER_SIZE(1);

reexecute:
    switch (CURRENT_STATE()) {

      case s_dead:
        /* this state is used after a 'Connection: close' message
         * the parser will error out if it reads another message
         */
        if (LIKELY(ch == CR || ch == LF))
          break;

        SET_ERRNO(HPE_CLOSED_CONNECTION);
        goto error;
      // 开始解析收到的客户端http包或者服务器返回的回包
      case s_start_req_or_res:
      {
        // 跳过无效字符
        if (ch == CR || ch == LF)
          break;
        // 初始化标记
        parser->flags = 0;
        parser->content_length = ULLONG_MAX;
        // 遇到H
        if (ch == 'H') {
          // 进入s_res_or_resp_H
          UPDATE_STATE(s_res_or_resp_H);
          // 通知上层，触发on_message_begin回调
          CALLBACK_NOTIFY(message_begin);
        } else {
          // 不是H说明nodejs作为客户端的请求包，因为http回复包是以H开头的，但是以H开头的不一定是回复包，比如HEAD请求
          parser->type = HTTP_REQUEST;
          UPDATE_STATE(s_start_req);
          REEXECUTE();
        }

        break;
      }
      // 遇到了第一个H
      case s_res_or_resp_H:
        // 第二个是T，说明是回复包，设置状态为s_res_HT
        if (ch == 'T') {
          parser->type = HTTP_RESPONSE;
          UPDATE_STATE(s_res_HT);
        } else {
          // 第二个不是T也不是E说明报文无效，报错
          if (UNLIKELY(ch != 'E')) {
            SET_ERRNO(HPE_INVALID_CONSTANT);
            goto error;
          }
          // 以HE开头，说明是HEAD，请求包
          parser->type = HTTP_REQUEST;
          parser->method = HTTP_HEAD;
          parser->index = 2;
          // 进入解析method状态
          UPDATE_STATE(s_req_method);
        }
        break;
      // 开始解析响应包状态
      case s_start_res:
      {
        parser->flags = 0;
        parser->content_length = ULLONG_MAX;
        // 遇到了H，进入s_res_HT状态，等待T
        switch (ch) {
          case 'H':
            UPDATE_STATE(s_res_H);
            break;
          // 跳过换车换行字符
          case CR:
          case LF:
            break;

          default:
            SET_ERRNO(HPE_INVALID_CONSTANT);
            goto error;
        }

        CALLBACK_NOTIFY(message_begin);
        break;
      }
      // 解析回复包H状态
      case s_res_H:
        // 下一个应该是T，否则报错，是的话变成s_res_HT状态
        STRICT_CHECK(ch != 'T');
        UPDATE_STATE(s_res_HT);
        break;
      // 下面两个类似，即回复包的HTTP/1.X
      case s_res_HT:
        STRICT_CHECK(ch != 'T');
        UPDATE_STATE(s_res_HTT);
        break;

      case s_res_HTT:
        STRICT_CHECK(ch != 'P');
        UPDATE_STATE(s_res_HTTP);
        break;

      case s_res_HTTP:
        STRICT_CHECK(ch != '/');
        UPDATE_STATE(s_res_first_http_major);
        break;
      // 开始解析http版本状态
      case s_res_first_http_major:
        // 不是数字则报错
        if (UNLIKELY(ch < '0' || ch > '9')) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }
        // 算出数字，作为主版本号
        parser->http_major = ch - '0';
        UPDATE_STATE(s_res_http_major);
        break;

      /* major HTTP version or dot */
      // 解析主版本状态，目前http主版本是1，所以后面跟的就是.，这里支持版本号大于9的情况
      case s_res_http_major:
      {
        // 遇到.则变成开始解析次版本状态
        if (ch == '.') {
          UPDATE_STATE(s_res_first_http_minor);
          break;
        }
        // 版本号不是数字则报错
        if (!IS_NUM(ch)) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }
        // 版本号大于9的情况
        parser->http_major *= 10;
        parser->http_major += ch - '0';
        // 主版本号不能大于999
        if (UNLIKELY(parser->http_major > 999)) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        break;
      }

      /* first digit of minor HTTP version */
      // 开始解析次版本号
      case s_res_first_http_minor:
        // 不是数字则报错
        if (UNLIKELY(!IS_NUM(ch))) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }
        // 记录次版本号的第一位
        parser->http_minor = ch - '0';
        // 状态改成解析次版本号
        UPDATE_STATE(s_res_http_minor);
        break;

      /* minor HTTP version or end of request line */
      case s_res_http_minor:
      {
        // 次版本号后面是空格则解析结束，进入开始解析返回码状态
        if (ch == ' ') {
          UPDATE_STATE(s_res_first_status_code);
          break;
        }
        
        if (UNLIKELY(!IS_NUM(ch))) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }
        // 次版本号也支持大于9小于999
        parser->http_minor *= 10;
        parser->http_minor += ch - '0';

        if (UNLIKELY(parser->http_minor > 999)) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        break;
      }
      // 开始解析返回码
      case s_res_first_status_code:
      {
        // 返回码不是数字
        if (!IS_NUM(ch)) {
          // 返回码是空格则跳过
          if (ch == ' ') {
            break;
          }
          // 不是数字又不是空格则报错
          SET_ERRNO(HPE_INVALID_STATUS);
          goto error;
        }
        parser->status_code = ch - '0';
        UPDATE_STATE(s_res_status_code);
        break;
      }
      // 解析返回码状态
      case s_res_status_code:
      { 
        // 返回码第二个字符不是数字
        if (!IS_NUM(ch)) {
          switch (ch) {
            // 是空格说明状态码解析完毕，进入解析返回码描述状态
            case ' ':
              UPDATE_STATE(s_res_status_start);
              break;
            // 是回车，则说明http协议第一行准备结束，还差个换行
            case CR:
              UPDATE_STATE(s_res_line_almost_done);
              break;
            // 是换行说明第一行解析完毕，进入开始解析http头阶段，即返回码描述为空
            case LF:
              UPDATE_STATE(s_header_field_start);
              break;
            // 否则出错
            default:
              SET_ERRNO(HPE_INVALID_STATUS);
              goto error;
          }
          break;
        }
        // 是数字则记录
        parser->status_code *= 10;
        parser->status_code += ch - '0';
        // 状态码不能大于999
        if (UNLIKELY(parser->status_code > 999)) {
          SET_ERRNO(HPE_INVALID_STATUS);
          goto error;
        }

        break;
      }
      // 开始解析返回码描述
      case s_res_status_start:
      {
        // http头解析接近结束，还差个换行
        if (ch == CR) {
          UPDATE_STATE(s_res_line_almost_done);
          break;
        }
        // 返回码描述为空，直接进入开始解析http头状态
        if (ch == LF) {
          UPDATE_STATE(s_header_field_start);
          break;
        }
        // 
        MARK(status);
        // 进入解析返回码描述状态
        UPDATE_STATE(s_res_status);
        parser->index = 0;
        break;
      }
      // 解析返回码描述
      case s_res_status:
        // 遇到回车，还差一个换行则结束http第一行解析
        if (ch == CR) {
          UPDATE_STATE(s_res_line_almost_done);
          CALLBACK_DATA(status);
          break;
        }
        // 第一行解析结束，进入开始解析http头状态
        if (ch == LF) {
          UPDATE_STATE(s_header_field_start);
          CALLBACK_DATA(status);
          break;
        }

        break;
      // 还差个换行状态
      case s_res_line_almost_done:
        // 不是换行则报错
        STRICT_CHECK(ch != LF);
        // 进入开始解析http头状态
        UPDATE_STATE(s_header_field_start);
        break;
      // 开始解析http请求报文的第一行
      case s_start_req:
      {
        // 跳过
        if (ch == CR || ch == LF)
          break;
        parser->flags = 0;
        parser->content_length = ULLONG_MAX;
        // 不是字母则报错
        if (UNLIKELY(!IS_ALPHA(ch))) {
          SET_ERRNO(HPE_INVALID_METHOD);
          goto error;
        }

        parser->method = (enum http_method) 0;
        parser->index = 1;
        // 根据请求方法的第一个字母判断http方法
        switch (ch) {
          case 'A': parser->method = HTTP_ACL; break;
          case 'B': parser->method = HTTP_BIND; break;
          case 'C': parser->method = HTTP_CONNECT; /* or COPY, CHECKOUT */ break;
          case 'D': parser->method = HTTP_DELETE; break;
          case 'G': parser->method = HTTP_GET; break;
          case 'H': parser->method = HTTP_HEAD; break;
          case 'L': parser->method = HTTP_LOCK; /* or LINK */ break;
          case 'M': parser->method = HTTP_MKCOL; /* or MOVE, MKACTIVITY, MERGE, M-SEARCH, MKCALENDAR */ break;
          case 'N': parser->method = HTTP_NOTIFY; break;
          case 'O': parser->method = HTTP_OPTIONS; break;
          case 'P': parser->method = HTTP_POST;
            /* or PROPFIND|PROPPATCH|PUT|PATCH|PURGE */
            break;
          case 'R': parser->method = HTTP_REPORT; /* or REBIND */ break;
          case 'S': parser->method = HTTP_SUBSCRIBE; /* or SEARCH */ break;
          case 'T': parser->method = HTTP_TRACE; break;
          case 'U': parser->method = HTTP_UNLOCK; /* or UNSUBSCRIBE, UNBIND, UNLINK */ break;
          default:
            SET_ERRNO(HPE_INVALID_METHOD);
            goto error;
        }
        // 解析http方法状态
        UPDATE_STATE(s_req_method);

        CALLBACK_NOTIFY(message_begin);

        break;
      }
      // 解析http方法状态(已经解析完第一个字符)
      case s_req_method:
      {
        const char *matcher;
        // 非法字符
        if (UNLIKELY(ch == '\0')) {
          SET_ERRNO(HPE_INVALID_METHOD);
          goto error;
        }
        // 通过索引拿到一个http方法对应的字符串，比如GET
        matcher = method_strings[parser->method];
        // ch是空格并且刚好到达对应http方法字符串的尾巴，则解析完成，进入开始解析url状态
        if (ch == ' ' && matcher[parser->index] == '\0') {
          UPDATE_STATE(s_req_spaces_before_url);
        } else if (ch == matcher[parser->index]) { // 还没有到达预期字符串的尾巴，即还没匹配完
          ; /* nada */
        } else if (IS_ALPHA(ch)) { // 到达了预期字符串的尾巴，但是还没有出现空格，说明前面的匹配错误，重新判断

          switch (parser->method << 16 | parser->index << 8 | ch) {
#define XX(meth, pos, ch, new_meth) \
            case (HTTP_##meth << 16 | pos << 8 | ch): \
              parser->method = HTTP_##new_meth; break;
            // case (HTTP_POST << 16 | 1 << 8 | 'U'): parser->method = HTTP_PUT; break;
            XX(POST,      1, 'U', PUT)
            XX(POST,      1, 'A', PATCH)
            XX(CONNECT,   1, 'H', CHECKOUT)
            XX(CONNECT,   2, 'P', COPY)
            XX(MKCOL,     1, 'O', MOVE)
            XX(MKCOL,     1, 'E', MERGE)
            XX(MKCOL,     2, 'A', MKACTIVITY)
            XX(MKCOL,     3, 'A', MKCALENDAR)
            XX(SUBSCRIBE, 1, 'E', SEARCH)
            XX(REPORT,    2, 'B', REBIND)
            XX(POST,      1, 'R', PROPFIND)
            XX(PROPFIND,  4, 'P', PROPPATCH)
            XX(PUT,       2, 'R', PURGE)
            XX(LOCK,      1, 'I', LINK)
            XX(UNLOCK,    2, 'S', UNSUBSCRIBE)
            XX(UNLOCK,    2, 'B', UNBIND)
            XX(UNLOCK,    3, 'I', UNLINK)
#undef XX

            default:
              SET_ERRNO(HPE_INVALID_METHOD);
              goto error;
          }
        } else if (ch == '-' &&
                   parser->index == 1 &&
                   parser->method == HTTP_MKCOL) {
          parser->method = HTTP_MSEARCH;
        } else {
          SET_ERRNO(HPE_INVALID_METHOD);
          goto error;
        }

        ++parser->index;
        break;
      }
      // 解析完http方法，开始解析url
      case s_req_spaces_before_url:
      {
        if (ch == ' ') break;

        MARK(url);
        // 如果是connect方法，则直接进入解析服务器信息状态，即不解析协议
        if (parser->method == HTTP_CONNECT) {
          UPDATE_STATE(s_req_server_start);
        }
        // 进入解析url状态
        UPDATE_STATE(parse_url_char(CURRENT_STATE(), ch));
        if (UNLIKELY(CURRENT_STATE() == s_dead)) {
          SET_ERRNO(HPE_INVALID_URL);
          goto error;
        }

        break;
      }
      // 解析http(s)://，直接调用解析url函数
      case s_req_schema:
      case s_req_schema_slash:
      case s_req_schema_slash_slash:
      case s_req_server_start:
      {
        switch (ch) {
          /* No whitespace allowed here */
          case ' ':
          case CR:
          case LF:
            SET_ERRNO(HPE_INVALID_URL);
            goto error;
          default:
            UPDATE_STATE(parse_url_char(CURRENT_STATE(), ch));
            if (UNLIKELY(CURRENT_STATE() == s_dead)) {
              SET_ERRNO(HPE_INVALID_URL);
              goto error;
            }
        }

        break;
      }
      // 解析url中http://后面的信息
      case s_req_server:
      case s_req_server_with_at:
      case s_req_path:
      case s_req_query_string_start:
      case s_req_query_string:
      case s_req_fragment_start:
      case s_req_fragment:
      {
        switch (ch) {
          // 遇到空格则进入开始解析http版本状态
          case ' ':
            UPDATE_STATE(s_req_http_start);
            CALLBACK_DATA(url);
            break;
          // 直接遇到回车换行则使用默认http版本
          case CR:
          case LF:
            parser->http_major = 0;
            parser->http_minor = 9;
            // 遇到回车则还需要个换行，遇到换行则进入开始解析http头状态
            UPDATE_STATE((ch == CR) ?
              s_req_line_almost_done :
              s_header_field_start);
            CALLBACK_DATA(url);
            break;
          default:
            // 其他字符则继续解析url
            UPDATE_STATE(parse_url_char(CURRENT_STATE(), ch));
            if (UNLIKELY(CURRENT_STATE() == s_dead)) {
              SET_ERRNO(HPE_INVALID_URL);
              goto error;
            }
        }
        break;
      }
      // 开始解析请求头的http协议版本，下面的case都是在解析请求头的http版本
      case s_req_http_start:
        switch (ch) {
          case 'H':
            UPDATE_STATE(s_req_http_H);
            break;
          case ' ':
            break;
          default:
            SET_ERRNO(HPE_INVALID_CONSTANT);
            goto error;
        }
        break;

      case s_req_http_H:
        STRICT_CHECK(ch != 'T');
        UPDATE_STATE(s_req_http_HT);
        break;

      case s_req_http_HT:
        STRICT_CHECK(ch != 'T');
        UPDATE_STATE(s_req_http_HTT);
        break;

      case s_req_http_HTT:
        STRICT_CHECK(ch != 'P');
        UPDATE_STATE(s_req_http_HTTP);
        break;

      case s_req_http_HTTP:
        STRICT_CHECK(ch != '/');
        UPDATE_STATE(s_req_first_http_major);
        break;

      /* first digit of major HTTP version */
      case s_req_first_http_major:
        if (UNLIKELY(ch < '1' || ch > '9')) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        parser->http_major = ch - '0';
        UPDATE_STATE(s_req_http_major);
        break;

      /* major HTTP version or dot */
      case s_req_http_major:
      {
        if (ch == '.') {
          UPDATE_STATE(s_req_first_http_minor);
          break;
        }

        if (UNLIKELY(!IS_NUM(ch))) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        parser->http_major *= 10;
        parser->http_major += ch - '0';

        if (UNLIKELY(parser->http_major > 999)) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        break;
      }

      /* first digit of minor HTTP version */
      case s_req_first_http_minor:
        if (UNLIKELY(!IS_NUM(ch))) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        parser->http_minor = ch - '0';
        UPDATE_STATE(s_req_http_minor);
        break;

      /* minor HTTP version or end of request line */
      case s_req_http_minor:
      {
        if (ch == CR) {
          UPDATE_STATE(s_req_line_almost_done);
          break;
        }

        if (ch == LF) {
          UPDATE_STATE(s_header_field_start);
          break;
        }

        /* XXX allow spaces after digit? */

        if (UNLIKELY(!IS_NUM(ch))) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        parser->http_minor *= 10;
        parser->http_minor += ch - '0';

        if (UNLIKELY(parser->http_minor > 999)) {
          SET_ERRNO(HPE_INVALID_VERSION);
          goto error;
        }

        break;
      }

      /* end of request line */
      case s_req_line_almost_done:
      {
        if (UNLIKELY(ch != LF)) {
          SET_ERRNO(HPE_LF_EXPECTED);
          goto error;
        }

        UPDATE_STATE(s_header_field_start);
        break;
      }
      // 开始解析http头
      case s_header_field_start:
      {
        /*
          遇到回车，可能是一个http头结束，
          也可能是整个http头解析结束，这里假设是整个头结束，
          如果遇到合法http头字符，则再修改状态
        */
        if (ch == CR) {
          UPDATE_STATE(s_headers_almost_done);
          break;
        }
        // 同上
        if (ch == LF) {
          /* they might be just sending \n instead of \r\n so this would be
           * the second \n to denote the end of headers*/
          UPDATE_STATE(s_headers_almost_done);
          REEXECUTE();
        }
        // 拿到对应的字符
        c = TOKEN(ch);

        if (UNLIKELY(!c)) {
          SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
          goto error;
        }

        MARK(header_field);

        parser->index = 0;
        // 解析头部状态
        UPDATE_STATE(s_header_field);
        // 根据http头的key的第一个字符做某些判断
        switch (c) {
          case 'c':
            parser->header_state = h_C;
            break;

          case 'p':
            parser->header_state = h_matching_proxy_connection;
            break;

          case 't':
            parser->header_state = h_matching_transfer_encoding;
            break;

          case 'u':
            parser->header_state = h_matching_upgrade;
            break;

          default:
            parser->header_state = h_general;
            break;
        }
        break;
      }
      // 解析http头的key
      case s_header_field:
      {
        const char* start = p;
        for (; p != data + len; p++) {
          ch = *p;
          c = TOKEN(ch);

          if (!c)
            break;

          switch (parser->header_state) {
            // 一般的头，继续
            case h_general:
              break;
            // 特殊头
            case h_C:
              parser->index++;
              // 等于哦则更新状态为h_CO，否则恢复成一般的头
              parser->header_state = (c == 'o' ? h_CO : h_general);
              break;
            // 同上
            case h_CO:
              parser->index++;
              parser->header_state = (c == 'n' ? h_CON : h_general);
              break;
            // key是con
            case h_CON:
              parser->index++;
              // 根据下一个字符继续判断
              switch (c) {
                // conn
                case 'n':
                  parser->header_state = h_matching_connection;
                  break;
                // cont
                case 't':
                  parser->header_state = h_matching_content_length;
                  break;
                // 恢复为一般头
                default:
                  parser->header_state = h_general;
                  break;
              }
              break;

            /* connection */

            case h_matching_connection:
              parser->index++;
              // 不等于connecttion则恢复为一般头
              if (parser->index > sizeof(CONNECTION)-1
                  || c != CONNECTION[parser->index]) {
                parser->header_state = h_general;
              } else if (parser->index == sizeof(CONNECTION)-2) { 
                parser->header_state = h_connection;
              }
              break;

            /* proxy-connection */

            case h_matching_proxy_connection:
              parser->index++;
              if (parser->index > sizeof(PROXY_CONNECTION)-1
                  || c != PROXY_CONNECTION[parser->index]) {
                parser->header_state = h_general;
              } else if (parser->index == sizeof(PROXY_CONNECTION)-2) {
                parser->header_state = h_connection;
              }
              break;

            /* content-length */

            case h_matching_content_length:
              parser->index++;
              if (parser->index > sizeof(CONTENT_LENGTH)-1
                  || c != CONTENT_LENGTH[parser->index]) {
                parser->header_state = h_general;
              } else if (parser->index == sizeof(CONTENT_LENGTH)-2) {
                // 已经设置过了，重复头报错
                if (parser->flags & F_CONTENTLENGTH) {
                  SET_ERRNO(HPE_UNEXPECTED_CONTENT_LENGTH);
                  goto error;
                }
                parser->header_state = h_content_length;
                parser->flags |= F_CONTENTLENGTH;
              }
              break;

            /* transfer-encoding */

            case h_matching_transfer_encoding:
              parser->index++;
              if (parser->index > sizeof(TRANSFER_ENCODING)-1
                  || c != TRANSFER_ENCODING[parser->index]) {
                parser->header_state = h_general;
              } else if (parser->index == sizeof(TRANSFER_ENCODING)-2) {
                parser->header_state = h_transfer_encoding;
              }
              break;

            /* upgrade */

            case h_matching_upgrade:
              parser->index++;
              if (parser->index > sizeof(UPGRADE)-1
                  || c != UPGRADE[parser->index]) {
                parser->header_state = h_general;
              } else if (parser->index == sizeof(UPGRADE)-2) {
                parser->header_state = h_upgrade;
              }
              break;

            case h_connection:
            case h_content_length:
            case h_transfer_encoding:
            case h_upgrade:
              if (ch != ' ') parser->header_state = h_general;
              break;

            default:
              assert(0 && "Unknown header_state");
              break;
          }
        }

        COUNT_HEADER_SIZE(p - start);

        if (p == data + len) {
          --p;
          break;
        }
        // 遇到:则key解析结束，开始解析value
        if (ch == ':') {
          UPDATE_STATE(s_header_value_discard_ws);
          CALLBACK_DATA(header_field);
          break;
        }

        SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
        goto error;
      }

      case s_header_value_discard_ws:
        if (ch == ' ' || ch == '\t') break;

        if (ch == CR) {
          UPDATE_STATE(s_header_value_discard_ws_almost_done);
          break;
        }

        if (ch == LF) {
          UPDATE_STATE(s_header_value_discard_lws);
          break;
        }

        /* FALLTHROUGH */

      case s_header_value_start:
      {
        MARK(header_value);

        UPDATE_STATE(s_header_value);
        parser->index = 0;

        c = LOWER(ch);

        switch (parser->header_state) {
          case h_upgrade:
            parser->flags |= F_UPGRADE;
            parser->header_state = h_general;
            break;

          case h_transfer_encoding:
            /* looking for 'Transfer-Encoding: chunked' */
            if ('c' == c) {
              parser->header_state = h_matching_transfer_encoding_chunked;
            } else {
              parser->header_state = h_general;
            }
            break;

          case h_content_length:
            if (UNLIKELY(!IS_NUM(ch))) {
              SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
              goto error;
            }

            parser->content_length = ch - '0';
            break;

          case h_connection:
            /* looking for 'Connection: keep-alive' */
            if (c == 'k') {
              parser->header_state = h_matching_connection_keep_alive;
            /* looking for 'Connection: close' */
            } else if (c == 'c') {
              parser->header_state = h_matching_connection_close;
            } else if (c == 'u') {
              parser->header_state = h_matching_connection_upgrade;
            } else {
              parser->header_state = h_matching_connection_token;
            }
            break;

          /* Multi-value `Connection` header */
          case h_matching_connection_token_start:
            break;

          default:
            parser->header_state = h_general;
            break;
        }
        break;
      }

      case s_header_value:
      {
        const char* start = p;
        enum header_states h_state = (enum header_states) parser->header_state;
        for (; p != data + len; p++) {
          ch = *p;
          if (ch == CR) {
            UPDATE_STATE(s_header_almost_done);
            parser->header_state = h_state;
            CALLBACK_DATA(header_value);
            break;
          }

          if (ch == LF) {
            UPDATE_STATE(s_header_almost_done);
            COUNT_HEADER_SIZE(p - start);
            parser->header_state = h_state;
            CALLBACK_DATA_NOADVANCE(header_value);
            REEXECUTE();
          }

          if (!lenient && !IS_HEADER_CHAR(ch)) {
            SET_ERRNO(HPE_INVALID_HEADER_TOKEN);
            goto error;
          }

          c = LOWER(ch);

          switch (h_state) {
            case h_general:
            {
              const char* p_cr;
              const char* p_lf;
              size_t limit = data + len - p;

              limit = MIN(limit, HTTP_MAX_HEADER_SIZE);

              p_cr = (const char*) memchr(p, CR, limit);
              p_lf = (const char*) memchr(p, LF, limit);
              if (p_cr != NULL) {
                if (p_lf != NULL && p_cr >= p_lf)
                  p = p_lf;
                else
                  p = p_cr;
              } else if (UNLIKELY(p_lf != NULL)) {
                p = p_lf;
              } else {
                p = data + len;
              }
              --p;

              break;
            }

            case h_connection:
            case h_transfer_encoding:
              assert(0 && "Shouldn't get here.");
              break;

            case h_content_length:
            {
              uint64_t t;

              if (ch == ' ') break;

              if (UNLIKELY(!IS_NUM(ch))) {
                SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
                parser->header_state = h_state;
                goto error;
              }

              t = parser->content_length;
              t *= 10;
              t += ch - '0';

              /* Overflow? Test against a conservative limit for simplicity. */
              if (UNLIKELY((ULLONG_MAX - 10) / 10 < parser->content_length)) {
                SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
                parser->header_state = h_state;
                goto error;
              }

              parser->content_length = t;
              break;
            }

            /* Transfer-Encoding: chunked */
            case h_matching_transfer_encoding_chunked:
              parser->index++;
              if (parser->index > sizeof(CHUNKED)-1
                  || c != CHUNKED[parser->index]) {
                h_state = h_general;
              } else if (parser->index == sizeof(CHUNKED)-2) {
                h_state = h_transfer_encoding_chunked;
              }
              break;

            case h_matching_connection_token_start:
              /* looking for 'Connection: keep-alive' */
              if (c == 'k') {
                h_state = h_matching_connection_keep_alive;
              /* looking for 'Connection: close' */
              } else if (c == 'c') {
                h_state = h_matching_connection_close;
              } else if (c == 'u') {
                h_state = h_matching_connection_upgrade;
              } else if (STRICT_TOKEN(c)) {
                h_state = h_matching_connection_token;
              } else if (c == ' ' || c == '\t') {
                /* Skip lws */
              } else {
                h_state = h_general;
              }
              break;

            /* looking for 'Connection: keep-alive' */
            case h_matching_connection_keep_alive:
              parser->index++;
              if (parser->index > sizeof(KEEP_ALIVE)-1
                  || c != KEEP_ALIVE[parser->index]) {
                h_state = h_matching_connection_token;
              } else if (parser->index == sizeof(KEEP_ALIVE)-2) {
                h_state = h_connection_keep_alive;
              }
              break;

            /* looking for 'Connection: close' */
            case h_matching_connection_close:
              parser->index++;
              if (parser->index > sizeof(CLOSE)-1 || c != CLOSE[parser->index]) {
                h_state = h_matching_connection_token;
              } else if (parser->index == sizeof(CLOSE)-2) {
                h_state = h_connection_close;
              }
              break;

            /* looking for 'Connection: upgrade' */
            case h_matching_connection_upgrade:
              parser->index++;
              if (parser->index > sizeof(UPGRADE) - 1 ||
                  c != UPGRADE[parser->index]) {
                h_state = h_matching_connection_token;
              } else if (parser->index == sizeof(UPGRADE)-2) {
                h_state = h_connection_upgrade;
              }
              break;

            case h_matching_connection_token:
              if (ch == ',') {
                h_state = h_matching_connection_token_start;
                parser->index = 0;
              }
              break;

            case h_transfer_encoding_chunked:
              if (ch != ' ') h_state = h_general;
              break;

            case h_connection_keep_alive:
            case h_connection_close:
            case h_connection_upgrade:
              if (ch == ',') {
                if (h_state == h_connection_keep_alive) {
                  parser->flags |= F_CONNECTION_KEEP_ALIVE;
                } else if (h_state == h_connection_close) {
                  parser->flags |= F_CONNECTION_CLOSE;
                } else if (h_state == h_connection_upgrade) {
                  parser->flags |= F_CONNECTION_UPGRADE;
                }
                h_state = h_matching_connection_token_start;
                parser->index = 0;
              } else if (ch != ' ') {
                h_state = h_matching_connection_token;
              }
              break;

            default:
              UPDATE_STATE(s_header_value);
              h_state = h_general;
              break;
          }
        }
        parser->header_state = h_state;

        COUNT_HEADER_SIZE(p - start);

        if (p == data + len)
          --p;
        break;
      }
    
      case s_header_almost_done:
      {
        if (UNLIKELY(ch != LF)) {
          SET_ERRNO(HPE_LF_EXPECTED);
          goto error;
        }

        UPDATE_STATE(s_header_value_lws);
        break;
      }

      case s_header_value_lws:
      {
        if (ch == ' ' || ch == '\t') {
          UPDATE_STATE(s_header_value_start);
          REEXECUTE();
        }

        /* finished the header */
        switch (parser->header_state) {
          case h_connection_keep_alive:
            parser->flags |= F_CONNECTION_KEEP_ALIVE;
            break;
          case h_connection_close:
            parser->flags |= F_CONNECTION_CLOSE;
            break;
          case h_transfer_encoding_chunked:
            parser->flags |= F_CHUNKED;
            break;
          case h_connection_upgrade:
            parser->flags |= F_CONNECTION_UPGRADE;
            break;
          default:
            break;
        }

        UPDATE_STATE(s_header_field_start);
        REEXECUTE();
      }

      case s_header_value_discard_ws_almost_done:
      {
        STRICT_CHECK(ch != LF);
        UPDATE_STATE(s_header_value_discard_lws);
        break;
      }

      case s_header_value_discard_lws:
      {
        if (ch == ' ' || ch == '\t') {
          UPDATE_STATE(s_header_value_discard_ws);
          break;
        } else {
          switch (parser->header_state) {
            case h_connection_keep_alive:
              parser->flags |= F_CONNECTION_KEEP_ALIVE;
              break;
            case h_connection_close:
              parser->flags |= F_CONNECTION_CLOSE;
              break;
            case h_connection_upgrade:
              parser->flags |= F_CONNECTION_UPGRADE;
              break;
            case h_transfer_encoding_chunked:
              parser->flags |= F_CHUNKED;
              break;
            default:
              break;
          }

          /* header value was empty */
          MARK(header_value);
          UPDATE_STATE(s_header_field_start);
          CALLBACK_DATA_NOADVANCE(header_value);
          REEXECUTE();
        }
      }

      case s_headers_almost_done:
      {
        STRICT_CHECK(ch != LF);

        if (parser->flags & F_TRAILING) {
          /* End of a chunked request */
          UPDATE_STATE(s_message_done);
          CALLBACK_NOTIFY_NOADVANCE(chunk_complete);
          REEXECUTE();
        }

        /* Cannot use chunked encoding and a content-length header together
           per the HTTP specification. */
        if ((parser->flags & F_CHUNKED) &&
            (parser->flags & F_CONTENTLENGTH)) {
          SET_ERRNO(HPE_UNEXPECTED_CONTENT_LENGTH);
          goto error;
        }

        UPDATE_STATE(s_headers_done);

        /* Set this here so that on_headers_complete() callbacks can see it */
        parser->upgrade =
          ((parser->flags & (F_UPGRADE | F_CONNECTION_UPGRADE)) ==
           (F_UPGRADE | F_CONNECTION_UPGRADE) ||
           parser->method == HTTP_CONNECT);

        /* Here we call the headers_complete callback. This is somewhat
         * different than other callbacks because if the user returns 1, we
         * will interpret that as saying that this message has no body. This
         * is needed for the annoying case of recieving a response to a HEAD
         * request.
         *
         * We'd like to use CALLBACK_NOTIFY_NOADVANCE() here but we cannot, so
         * we have to simulate it by handling a change in errno below.
         */
        if (settings->on_headers_complete) {
          switch (settings->on_headers_complete(parser)) {
            case 0:
              break;

            case 2:
              parser->upgrade = 1;

            case 1:
              parser->flags |= F_SKIPBODY;
              break;

            default:
              SET_ERRNO(HPE_CB_headers_complete);
              RETURN(p - data); /* Error */
          }
        }

        if (HTTP_PARSER_ERRNO(parser) != HPE_OK) {
          RETURN(p - data);
        }

        REEXECUTE();
      }

      case s_headers_done:
      {
        int hasBody;
        STRICT_CHECK(ch != LF);

        parser->nread = 0;

        hasBody = parser->flags & F_CHUNKED ||
          (parser->content_length > 0 && parser->content_length != ULLONG_MAX);
        if (parser->upgrade && (parser->method == HTTP_CONNECT ||
                                (parser->flags & F_SKIPBODY) || !hasBody)) {
          /* Exit, the rest of the message is in a different protocol. */
          UPDATE_STATE(NEW_MESSAGE());
          CALLBACK_NOTIFY(message_complete);
          RETURN((p - data) + 1);
        }

        if (parser->flags & F_SKIPBODY) {
          UPDATE_STATE(NEW_MESSAGE());
          CALLBACK_NOTIFY(message_complete);
        } else if (parser->flags & F_CHUNKED) {
          /* chunked encoding - ignore Content-Length header */
          UPDATE_STATE(s_chunk_size_start);
        } else {
          if (parser->content_length == 0) {
            /* Content-Length header given but zero: Content-Length: 0\r\n */
            UPDATE_STATE(NEW_MESSAGE());
            CALLBACK_NOTIFY(message_complete);
          } else if (parser->content_length != ULLONG_MAX) {
            /* Content-Length header given and non-zero */
            UPDATE_STATE(s_body_identity);
          } else {
            if (!http_message_needs_eof(parser)) {
              /* Assume content-length 0 - read the next */
              UPDATE_STATE(NEW_MESSAGE());
              CALLBACK_NOTIFY(message_complete);
            } else {
              /* Read body until EOF */
              UPDATE_STATE(s_body_identity_eof);
            }
          }
        }

        break;
      }

      case s_body_identity:
      {
        uint64_t to_read = MIN(parser->content_length,
                               (uint64_t) ((data + len) - p));

        assert(parser->content_length != 0
            && parser->content_length != ULLONG_MAX);

        /* The difference between advancing content_length and p is because
         * the latter will automaticaly advance on the next loop iteration.
         * Further, if content_length ends up at 0, we want to see the last
         * byte again for our message complete callback.
         */
        MARK(body);
        parser->content_length -= to_read;
        p += to_read - 1;

        if (parser->content_length == 0) {
          UPDATE_STATE(s_message_done);

          /* Mimic CALLBACK_DATA_NOADVANCE() but with one extra byte.
           *
           * The alternative to doing this is to wait for the next byte to
           * trigger the data callback, just as in every other case. The
           * problem with this is that this makes it difficult for the test
           * harness to distinguish between complete-on-EOF and
           * complete-on-length. It's not clear that this distinction is
           * important for applications, but let's keep it for now.
           */
          CALLBACK_DATA_(body, p - body_mark + 1, p - data);
          REEXECUTE();
        }

        break;
      }

      /* read until EOF */
      case s_body_identity_eof:
        MARK(body);
        p = data + len - 1;

        break;

      case s_message_done:
        UPDATE_STATE(NEW_MESSAGE());
        CALLBACK_NOTIFY(message_complete);
        if (parser->upgrade) {
          /* Exit, the rest of the message is in a different protocol. */
          RETURN((p - data) + 1);
        }
        break;

      case s_chunk_size_start:
      {
        assert(parser->nread == 1);
        assert(parser->flags & F_CHUNKED);

        unhex_val = unhex[(unsigned char)ch];
        if (UNLIKELY(unhex_val == -1)) {
          SET_ERRNO(HPE_INVALID_CHUNK_SIZE);
          goto error;
        }

        parser->content_length = unhex_val;
        UPDATE_STATE(s_chunk_size);
        break;
      }

      case s_chunk_size:
      {
        uint64_t t;

        assert(parser->flags & F_CHUNKED);

        if (ch == CR) {
          UPDATE_STATE(s_chunk_size_almost_done);
          break;
        }

        unhex_val = unhex[(unsigned char)ch];

        if (unhex_val == -1) {
          if (ch == ';' || ch == ' ') {
            UPDATE_STATE(s_chunk_parameters);
            break;
          }

          SET_ERRNO(HPE_INVALID_CHUNK_SIZE);
          goto error;
        }

        t = parser->content_length;
        t *= 16;
        t += unhex_val;

        /* Overflow? Test against a conservative limit for simplicity. */
        if (UNLIKELY((ULLONG_MAX - 16) / 16 < parser->content_length)) {
          SET_ERRNO(HPE_INVALID_CONTENT_LENGTH);
          goto error;
        }

        parser->content_length = t;
        break;
      }

      case s_chunk_parameters:
      {
        assert(parser->flags & F_CHUNKED);
        /* just ignore this shit. TODO check for overflow */
        if (ch == CR) {
          UPDATE_STATE(s_chunk_size_almost_done);
          break;
        }
        break;
      }

      case s_chunk_size_almost_done:
      {
        assert(parser->flags & F_CHUNKED);
        STRICT_CHECK(ch != LF);

        parser->nread = 0;

        if (parser->content_length == 0) {
          parser->flags |= F_TRAILING;
          UPDATE_STATE(s_header_field_start);
        } else {
          UPDATE_STATE(s_chunk_data);
        }
        CALLBACK_NOTIFY(chunk_header);
        break;
      }

      case s_chunk_data:
      {
        uint64_t to_read = MIN(parser->content_length,
                               (uint64_t) ((data + len) - p));

        assert(parser->flags & F_CHUNKED);
        assert(parser->content_length != 0
            && parser->content_length != ULLONG_MAX);

        /* See the explanation in s_body_identity for why the content
         * length and data pointers are managed this way.
         */
        MARK(body);
        parser->content_length -= to_read;
        p += to_read - 1;

        if (parser->content_length == 0) {
          UPDATE_STATE(s_chunk_data_almost_done);
        }

        break;
      }

      case s_chunk_data_almost_done:
        assert(parser->flags & F_CHUNKED);
        assert(parser->content_length == 0);
        STRICT_CHECK(ch != CR);
        UPDATE_STATE(s_chunk_data_done);
        CALLBACK_DATA(body);
        break;

      case s_chunk_data_done:
        assert(parser->flags & F_CHUNKED);
        STRICT_CHECK(ch != LF);
        parser->nread = 0;
        UPDATE_STATE(s_chunk_size_start);
        CALLBACK_NOTIFY(chunk_complete);
        break;

      default:
        assert(0 && "unhandled state");
        SET_ERRNO(HPE_INVALID_INTERNAL_STATE);
        goto error;
    }
  }

  /* Run callbacks for any marks that we have leftover after we ran our of
   * bytes. There should be at most one of these set, so it's OK to invoke
   * them in series (unset marks will not result in callbacks).
   *
   * We use the NOADVANCE() variety of callbacks here because 'p' has already
   * overflowed 'data' and this allows us to correct for the off-by-one that
   * we'd otherwise have (since CALLBACK_DATA() is meant to be run with a 'p'
   * value that's in-bounds).
   */

  assert(((header_field_mark ? 1 : 0) +
          (header_value_mark ? 1 : 0) +
          (url_mark ? 1 : 0)  +
          (body_mark ? 1 : 0) +
          (status_mark ? 1 : 0)) <= 1);

  CALLBACK_DATA_NOADVANCE(header_field);
  CALLBACK_DATA_NOADVANCE(header_value);
  CALLBACK_DATA_NOADVANCE(url);
  CALLBACK_DATA_NOADVANCE(body);
  CALLBACK_DATA_NOADVANCE(status);

  RETURN(len);

error:
  if (HTTP_PARSER_ERRNO(parser) == HPE_OK) {
    SET_ERRNO(HPE_UNKNOWN);
  }

  RETURN(p - data);
}


/* Does the parser need to see an EOF to find the end of the message? */
int
http_message_needs_eof (const http_parser *parser)
{
  if (parser->type == HTTP_REQUEST) {
    return 0;
  }

  /* See RFC 2616 section 4.4 */
  if (parser->status_code / 100 == 1 || /* 1xx e.g. Continue */
      parser->status_code == 204 ||     /* No Content */
      parser->status_code == 304 ||     /* Not Modified */
      parser->flags & F_SKIPBODY) {     /* response to a HEAD request */
    return 0;
  }

  if ((parser->flags & F_CHUNKED) || parser->content_length != ULLONG_MAX) {
    return 0;
  }

  return 1;
}


int
http_should_keep_alive (const http_parser *parser)
{
  if (parser->http_major > 0 && parser->http_minor > 0) {
    /* HTTP/1.1 */
    if (parser->flags & F_CONNECTION_CLOSE) {
      return 0;
    }
  } else {
    /* HTTP/1.0 or earlier */
    if (!(parser->flags & F_CONNECTION_KEEP_ALIVE)) {
      return 0;
    }
  }

  return !http_message_needs_eof(parser);
}


const char *
http_method_str (enum http_method m)
{
  return ELEM_AT(method_strings, m, "<unknown>");
}


void
http_parser_init (http_parser *parser, enum http_parser_type t)
{
  void *data = parser->data; /* preserve application data */
  memset(parser, 0, sizeof(*parser));
  parser->data = data;
  parser->type = t;
  parser->state = (t == HTTP_REQUEST ? s_start_req : (t == HTTP_RESPONSE ? s_start_res : s_start_req_or_res));
  parser->http_errno = HPE_OK;
}

void
http_parser_settings_init(http_parser_settings *settings)
{
  memset(settings, 0, sizeof(*settings));
}

const char *
http_errno_name(enum http_errno err) {
  assert(((size_t) err) < ARRAY_SIZE(http_strerror_tab));
  return http_strerror_tab[err].name;
}

const char *
http_errno_description(enum http_errno err) {
  assert(((size_t) err) < ARRAY_SIZE(http_strerror_tab));
  return http_strerror_tab[err].description;
}
// s状态，遇到ch，判断下一个状态
static enum http_host_state
http_parse_host_char(enum http_host_state s, const char ch) {
  switch(s) {
    // 解析用户名密码状态
    case s_http_userinfo:
    case s_http_userinfo_start:
      // 遇到@则说明用户名密码解析完毕，进入开始解析host状态
      if (ch == '@') {
        return s_http_host_start;
      }
      // 不是@，是合法的用户名密码字符，则维护状态，否则报错
      if (IS_USERINFO_CHAR(ch)) {
        return s_http_userinfo;
      }
      break;
    // 开始解析host状态
    case s_http_host_start:
      // 遇到了[，进入开始解析ipv6格式
      if (ch == '[') {
        return s_http_host_v6_start;
      }
      // 是合法的host字符，则进入解析一般host格式状态
      if (IS_HOST_CHAR(ch)) {
        return s_http_host;
      }

      break;
    // 解析host状态
    case s_http_host:
      // 遇到host合法字符，则维护状态
      if (IS_HOST_CHAR(ch)) {
        return s_http_host;
      }
    /*
      正在解析host，遇到了一个不合法的host字符，再判断是不是“:”字符，
      是的话说明解析域名或ip部分解析，进入解析端口状态，case没有break
      的时候会一直往下执行，不管下面的case xxx是啥
    */
    /* FALLTHROUGH */
    case s_http_host_v6_end:
      // 解析完域名或ip部分，遇到“:”则进入开始解析端口状态
      if (ch == ':') {
        return s_http_host_port_start;
      }

      break;
    // 解析ipv6 host
    case s_http_host_v6:
      // 遇到]则进入解析ipv6结束状态
      if (ch == ']') {
        return s_http_host_v6_end;
      }
    // 开始解析ipv6，或者正在解析ipv6（上面的case）
    /* FALLTHROUGH */
    case s_http_host_v6_start:
      // 合法的ipv6字符
      if (IS_HEX(ch) || ch == ':' || ch == '.') {
        return s_http_host_v6;
      }
      // ipv6非第一个字符如果是%则进入zone_start状态
      if (s == s_http_host_v6 && ch == '%') {
        return s_http_host_v6_zone_start;
      }
      break;
    // 解析zone状态（ipv6的特性）
    case s_http_host_v6_zone:
      // 遇到]则进入解析ipv6结束状态
      if (ch == ']') {
        return s_http_host_v6_end;
      }
    // 开始解析zone，或者正在解析zone
    /* FALLTHROUGH */
    case s_http_host_v6_zone_start:
      /* RFC 6874 Zone ID consists of 1*( unreserved / pct-encoded) */
      // 合法的zone字符则维持状态或进入解析zone状态
      if (IS_ALPHANUM(ch) || ch == '%' || ch == '.' || ch == '-' || ch == '_' ||
          ch == '~') {
        return s_http_host_v6_zone;
      }
      break;
    // 开始解析端口或者正在解析端口，遇到合法的字符都进入正在解析端口状态
    case s_http_host_port:
    case s_http_host_port_start:
      if (IS_NUM(ch)) {
        return s_http_host_port;
      }

      break;

    default:
      break;
  }
  return s_http_host_dead;
}
// 解析host
static int
http_parse_host(const char * buf, struct http_parser_url *u, int found_at) {
  assert(u->field_set & (1 << UF_HOST));
  enum http_host_state s;

  const char *p;
  // 拿到host部分的数据
  size_t buflen = u->field_data[UF_HOST].off + u->field_data[UF_HOST].len;

  u->field_data[UF_HOST].len = 0;
  // 有没有用户密码部分，有则从解析用户密码信息状态开始，否则从解析host开始
  s = found_at ? s_http_userinfo_start : s_http_host_start;
  // 逐个遍历host部分的字符
  for (p = buf + u->field_data[UF_HOST].off; p < buf + buflen; p++) {
    // 根据当前字符*p，判断下一个状态
    enum http_host_state new_s = http_parse_host_char(s, *p);
    // 解析host出错
    if (new_s == s_http_host_dead) {
      return 1;
    }

    switch(new_s) {
      // 下一个状态是解析host
      case s_http_host:
        // 前一个状态和下一个不相等，说明刚刚进入解析host状态，记录开始字符
        if (s != s_http_host) {
          u->field_data[UF_HOST].off = p - buf;
        }
        // 长度累加
        u->field_data[UF_HOST].len++;
        break;
      // 同上
      case s_http_host_v6:
        if (s != s_http_host_v6) {
          u->field_data[UF_HOST].off = p - buf;
        }
        u->field_data[UF_HOST].len++;
        break;
      // 两个状态都属于host部分，内容长度累加
      case s_http_host_v6_zone_start:
      case s_http_host_v6_zone:
        u->field_data[UF_HOST].len++;
        break;
      // 进入解析端口状态
      case s_http_host_port:
        // 不相等说明刚进入该状态
        if (s != s_http_host_port) {
          // 记录数据位置和初始化长度
          u->field_data[UF_PORT].off = p - buf;
          u->field_data[UF_PORT].len = 0;
          // 设置有端口标记
          u->field_set |= (1 << UF_PORT);
        }
        u->field_data[UF_PORT].len++;
        break;
      // 用户名密码状态
      case s_http_userinfo:
        // 同上
        if (s != s_http_userinfo) {
          u->field_data[UF_USERINFO].off = p - buf ;
          u->field_data[UF_USERINFO].len = 0;
          u->field_set |= (1 << UF_USERINFO);
        }
        u->field_data[UF_USERINFO].len++;
        break;

      default:
        break;
    }
    s = new_s;
  }

  /* Make sure we don't end somewhere unexpected */
  // 最后一个状态如果是命中下面的case则报错
  switch (s) {
    case s_http_host_start:
    case s_http_host_v6_start:
    case s_http_host_v6:
    case s_http_host_v6_zone_start:
    case s_http_host_v6_zone:
    case s_http_host_port_start:
    case s_http_userinfo:
    case s_http_userinfo_start:
      return 1;
    default:
      break;
  }

  return 0;
}

void
http_parser_url_init(struct http_parser_url *u) {
  memset(u, 0, sizeof(*u));
}

/*
  buf url的内容
  buflen url长度
  is_connect 是否是connect方法
  u 保存解析结果
*/
int
http_parser_parse_url(const char *buf, size_t buflen, int is_connect,
                      struct http_parser_url *u)
{
  enum state s;
  const char *p;
  enum http_parser_url_fields uf, old_uf;
  int found_at = 0;
  // 初始化结构体的字段
  u->port = u->field_set = 0;
  // connect方法的话直接解析host部分，否则解析方法后面的url
  s = is_connect ? s_req_server_start : s_req_spaces_before_url;
  old_uf = UF_MAX;
  // 遍历字符串
  for (p = buf; p < buf + buflen; p++) {
    // s是当前状态，遇到*p对应的字符后，返回下一个状态
    s = parse_url_char(s, *p);

    /* Figure out the next field that we're operating on */
    switch (s) {
      // 解析出错
      case s_dead:
        return 1;

      /* Skip delimeters */
      case s_req_schema_slash:
      case s_req_schema_slash_slash:
      case s_req_server_start:
      case s_req_query_string_start:
      case s_req_fragment_start:
        continue;
      // 开始解析协议，即遇到协议内容的第一个字符了
      case s_req_schema:
        uf = UF_SCHEMA;
        break;
      // 遇到了@字符
      case s_req_server_with_at:
        found_at = 1;

      /* FALLTROUGH */
      // 遇到了@或者遇到了服务器信息相关的字符则说明有host
      case s_req_server:
        uf = UF_HOST;
        break;
      // 有路径
      case s_req_path:
        uf = UF_PATH;
        break;
      // 有query参数
      case s_req_query_string:
        uf = UF_QUERY;
        break;
      // 有锚点
      case s_req_fragment:
        uf = UF_FRAGMENT;
        break;

      default:
        assert(!"Unexpected state");
        return 1;
    }

    /* Nothing's changed; soldier on */
    // old_uf代表上一个状态，状态不变，该字段的内容长度加一
    if (uf == old_uf) {
      u->field_data[uf].len++;
      continue;
    }
    // 否则说明进入了下一个状态，p是当前字符（即导致状态变更的字符），off记录下一个状态的起始字符的地址
    u->field_data[uf].off = p - buf;
    // 刚进入下一个状态，内容长度是1
    u->field_data[uf].len = 1;
    // url中有某个字符，设置标记
    u->field_set |= (1 << uf);
    // 记录上一个状态
    old_uf = uf;
  }

  /* host must be present if there is a schema */
  /* parsing http:///toto will fail */
  // 有协议，没有host，则报错
  if ((u->field_set & (1 << UF_SCHEMA)) &&
      (u->field_set & (1 << UF_HOST)) == 0) {
    return 1;
  }
  // 有host则解析host
  if (u->field_set & (1 << UF_HOST)) {
    if (http_parse_host(buf, u, found_at) != 0) {
      return 1;
    }
  }

  /* CONNECT requests can only contain "hostname:port" */
  // connect方法只能设置两个字段
  if (is_connect && u->field_set != ((1 << UF_HOST)|(1 << UF_PORT))) {
    return 1;
  }
  // 如果设置了端口，则校验端口
  if (u->field_set & (1 << UF_PORT)) {
    /* Don't bother with endp; we've already validated the string */
    unsigned long v = strtoul(buf + u->field_data[UF_PORT].off, NULL, 10);

    /* Ports have a max value of 2^16 */
    if (v > 0xffff) {
      return 1;
    }

    u->port = (uint16_t) v;
  }

  return 0;
}

void
http_parser_pause(http_parser *parser, int paused) {
  /* Users should only be pausing/unpausing a parser that is not in an error
   * state. In non-debug builds, there's not much that we can do about this
   * other than ignore it.
   */
  if (HTTP_PARSER_ERRNO(parser) == HPE_OK ||
      HTTP_PARSER_ERRNO(parser) == HPE_PAUSED) {
    SET_ERRNO((paused) ? HPE_PAUSED : HPE_OK);
  } else {
    assert(0 && "Attempting to pause parser in error state");
  }
}

int
http_body_is_final(const struct http_parser *parser) {
    return parser->state == s_message_done;
}

unsigned long
http_parser_version(void) {
  return HTTP_PARSER_VERSION_MAJOR * 0x10000 |
         HTTP_PARSER_VERSION_MINOR * 0x00100 |
         HTTP_PARSER_VERSION_PATCH * 0x00001;
}
