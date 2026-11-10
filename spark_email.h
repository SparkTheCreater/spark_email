/**
 * @file    spark_email.h
 * @brief   阻塞 SMTP 发信。一次调用完成一封信，返回时连接已关闭。
 *
 * spark_email.cpp 用 C++11 编译，链接 -lssl -lcrypto -pthread。
 * 字符串按 UTF-8。结构体全零即默认：465、隐式 TLS、校验证书、每次等待 30 秒。
 */
#ifndef SPARK_EMAIL_H
#define SPARK_EMAIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum
{
    SPARK_EMAIL_OK = 0,
    /* 参数、附件、证书、认证、5xx。原样重发还会失败。 */
    SPARK_EMAIL_ERR = -1,
    /* 解析、连接、读写超时、4xx。稍后可以重发。 */
    SPARK_EMAIL_AGAIN = -11
}
SparkEmailStatus;

typedef enum
{
    /* 465 走隐式 TLS，其他端口必须 STARTTLS。 */
    SPARK_EMAIL_TLS_AUTO = 0,
    SPARK_EMAIL_TLS_IMPLICIT = 1,
    SPARK_EMAIL_TLS_STARTTLS = 2,
    /* 明文，只用于内网中继。 */
    SPARK_EMAIL_TLS_NONE = 3
}
SparkEmailTls;

typedef struct SparkEmailServer
{
    const char *host;
    /* 0 时按 tls 取 465、587 或 25。 */
    int port;
    /* SparkEmailTls。 */
    int tls;
    /* 完整邮箱。为空则不认证。 */
    const char *username;
    /* QQ、163 填授权码。 */
    const char *password;
    /* 为空用系统证书库。 */
    const char *ca_file;
    /* 非零则不校验证书。 */
    int insecure;
    /* 连接和每次等待的上限，毫秒。0 为 30000。 */
    int timeout_ms;
    /*
     * 可空。每个 SMTP 命令和响应调用一次。
     * line 只在回调期间有效。认证内容是星号。
     */
    void (*trace)(void *user, const char *line);
    void *trace_user;
}
SparkEmailServer;

/* 图片、视频、普通文件都用它。 */
typedef struct SparkEmailFile
{
    /* data 为空时读这个文件。 */
    const char *path;
    /* 非空时取内存里的 size 字节，例如刚编好的 JPEG。 */
    const void *data;
    size_t size;
    /* 收件人看到的文件名。为空则取 path 末段。内存数据必须填写。 */
    const char *name;
    /* 为空则按 name 的扩展名查表。 */
    const char *type;
    /*
     * 非空，且 html 里写了 cid:<cid> 时，作为内嵌图。
     * 否则仍是附件。视频不会放进正文。
     */
    const char *cid;
}
SparkEmailFile;

typedef struct SparkEmailMessage
{
    /* "a@qq.com" 或 "名字 <a@qq.com>"。地址应与 username 一致。 */
    const char *from;
    /* 多个地址用逗号或分号隔开。显示名里的逗号要加引号。 */
    const char *to;
    const char *cc;
    /* 只出现在 RCPT TO 里。 */
    const char *bcc;
    const char *subject;
    /* 非 NULL 表示有纯文本，可以是空串。 */
    const char *text;
    /* 非 NULL 表示有 HTML，可以是空串。 */
    const char *html;
    const SparkEmailFile *files;
    size_t file_count;
}
SparkEmailMessage;

/*
 * 同步。失败时 err 写「哪一步 -> 原因」，err 可以为空。
 * 没有全局状态，可以多线程同时调用。
 * 结束点之后如果超时，返回 SPARK_EMAIL_AGAIN，信有可能已经投递。
 */
SparkEmailStatus spark_email_send(const SparkEmailServer *server, const SparkEmailMessage *message, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif
