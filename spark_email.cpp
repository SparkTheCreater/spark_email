/**
 * @file    spark_email.cpp
 * @brief   阻塞 SMTP 发信。先排好 MIME，再一问一答把信交出去。
 *
 * 叶子一律 Base64，每 57 个原文字节一行，出来正好 76 列。
 * 这样正文全是短的 7bit 行，也不依赖服务器的 8BITMIME。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "spark_email.h"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

const unsigned long long kTooBig = ~0ull;

enum Payload
{
    PAY_NONE = 0,
    PAY_MEM = 1,
    PAY_FD = 2
}
;

char AsciiLower(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

bool EqualFold(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (AsciiLower(a[i]) != AsciiLower(b[i]))
        {
            return false;
        }
    }
    return true;
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

std::string Trim(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
    {
        a += 1;
    }
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
    {
        b -= 1;
    }
    return s.substr(a, b - a);
}

bool AddSize(unsigned long long *acc, unsigned long long n)
{
    if (n == kTooBig || *acc > kTooBig - n)
    {
        return false;
    }
    *acc += n;
    return true;
}

/* 不含换行。编码字和 AUTH 用它。 */
size_t EncodeRaw(const unsigned char *p, size_t n, char *out)
{
    static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n)
    {
        unsigned int v = ((unsigned int)p[i] << 16) | ((unsigned int)p[i + 1] << 8) | (unsigned int)p[i + 2];
        out[o] = kTab[(v >> 18) & 63];
        out[o + 1] = kTab[(v >> 12) & 63];
        out[o + 2] = kTab[(v >> 6) & 63];
        out[o + 3] = kTab[v & 63];
        o += 4;
        i += 3;
    }
    if (i < n)
    {
        unsigned int v = (unsigned int)p[i] << 16;
        if (i + 1 < n)
        {
            v |= (unsigned int)p[i + 1] << 8;
        }
        out[o] = kTab[(v >> 18) & 63];
        out[o + 1] = kTab[(v >> 12) & 63];
        if (i + 1 < n)
        {
            out[o + 2] = kTab[(v >> 6) & 63];
        }
        else
        {
            out[o + 2] = '=';
        }
        out[o + 3] = '=';
        o += 4;
    }
    return o;
}

std::string Base64Raw(const unsigned char *p, size_t n)
{
    std::string out;
    if (n == 0)
    {
        return out;
    }
    out.resize(4 * ((n + 2) / 3));
    size_t m = EncodeRaw(p, n, &out[0]);
    out.resize(m);
    return out;
}

/* 57 字节变成 76 字符再加 CRLF。末段不足 57 时短一行。 */
unsigned long long Base64Len(unsigned long long n)
{
    if (n == 0)
    {
        return 0;
    }
    unsigned long long full = n / 57;
    unsigned long long rem = n % 57;
    if (full > kTooBig / 78)
    {
        return kTooBig;
    }
    unsigned long long out = full * 78;
    if (rem > 0)
    {
        unsigned long long add = ((rem + 2) / 3) * 4 + 2;
        if (out > kTooBig - add)
        {
            return kTooBig;
        }
        out += add;
    }
    return out;
}

bool NeedEncode(const std::string &s)
{
    for (size_t i = 0; i < s.size(); ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e)
        {
            return true;
        }
    }
    return false;
}

/* 在 UTF-8 字符边界上切，最多 max 个字节。非法字节单独前进，避免卡死。 */
size_t Utf8Span(const char *s, size_t len, size_t max)
{
    size_t i = 0;
    while (i < len && i < max)
    {
        unsigned char c = (unsigned char)s[i];
        size_t need = 1;
        if (c >= 0x80)
        {
            if ((c & 0xe0) == 0xc0)
            {
                need = 2;
            }
            else if ((c & 0xf0) == 0xe0)
            {
                need = 3;
            }
            else if ((c & 0xf8) == 0xf0)
            {
                need = 4;
            }
            else
            {
                break;
            }
        }
        if (i + need > len || i + need > max)
        {
            break;
        }
        bool ok = true;
        for (size_t k = 1; k < need; ++k)
        {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xc0) != 0x80)
            {
                ok = false;
            }
        }
        if (!ok)
        {
            break;
        }
        i += need;
    }
    if (i == 0 && len > 0 && max > 0)
    {
        return 1;
    }
    return i;
}

/* 每 45 个 UTF-8 字节一个编码字，续行以空格开头。单个编码字不超过 75 字符。 */
std::string EncodeWords(const std::string &s)
{
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i < s.size())
    {
        size_t n = Utf8Span(s.data() + i, s.size() - i, 45);
        if (n == 0)
        {
            n = 1;
        }
        char raw[80];
        size_t m = EncodeRaw((const unsigned char *)s.data() + i, n, raw);
        if (!first)
        {
            out += "\r\n ";
        }
        out += "=?UTF-8?B?";
        out.append(raw, m);
        out += "?=";
        first = false;
        i += n;
    }
    return out;
}

std::string FormatPhrase(const std::string &name)
{
    if (NeedEncode(name) || name.size() > 60)
    {
        return EncodeWords(name);
    }
    bool plain = name[0] != ' ' && name[name.size() - 1] != ' ';
    for (size_t i = 0; plain && i < name.size(); ++i)
    {
        char c = name[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
        if (!alnum)
        {
            plain = false;
        }
    }
    if (plain)
    {
        return name;
    }
    std::string out = "\"";
    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '\\' || name[i] == '"')
        {
            out += '\\';
        }
        out += name[i];
    }
    out += '"';
    return out;
}

std::string FormatParam(const std::string &name)
{
    if (NeedEncode(name) || name.size() > 60)
    {
        return "\"" + EncodeWords(name) + "\"";
    }
    std::string out = "\"";
    for (size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '\\' || name[i] == '"')
        {
            out += '\\';
        }
        out += name[i];
    }
    out += '"';
    return out;
}

std::string FormatSubject(const std::string &s)
{
    if (!NeedEncode(s) && s.size() + 9 <= 78)
    {
        return s;
    }
    if (s.empty())
    {
        return "";
    }
    return EncodeWords(s);
}

std::string NormalizeCrlf(const char *s)
{
    std::string out;
    if (s == nullptr)
    {
        return out;
    }
    for (size_t i = 0; s[i] != 0; ++i)
    {
        if (s[i] == '\r')
        {
            out += "\r\n";
            if (s[i + 1] == '\n')
            {
                i += 1;
            }
        }
        else if (s[i] == '\n')
        {
            out += "\r\n";
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

std::string ExtOf(const std::string &name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
    {
        return "";
    }
    size_t slash = name.rfind('/');
    if (slash != std::string::npos && slash > dot)
    {
        return "";
    }
    std::string ext = name.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
    {
        ext[i] = AsciiLower(ext[i]);
    }
    return ext;
}

std::string LookupType(const std::string &name)
{
    static const char *kExt[] =
    {
        "jpg", "image/jpeg",
        "jpeg", "image/jpeg",
        "png", "image/png",
        "gif", "image/gif",
        "webp", "image/webp",
        "bmp", "image/bmp",
        "svg", "image/svg+xml",
        "ico", "image/x-icon",
        "heic", "image/heic",
        "tif", "image/tiff",
        "tiff", "image/tiff",
        "mp4", "video/mp4",
        "m4v", "video/mp4",
        "mov", "video/quicktime",
        "avi", "video/x-msvideo",
        "mkv", "video/x-matroska",
        "webm", "video/webm",
        "mpeg", "video/mpeg",
        "mpg", "video/mpeg",
        "wmv", "video/x-ms-wmv",
        "flv", "video/x-flv",
        "3gp", "video/3gpp",
        "ts", "video/mp2t",
        "mp3", "audio/mpeg",
        "wav", "audio/wav",
        "ogg", "audio/ogg",
        "m4a", "audio/mp4",
        "flac", "audio/flac",
        "aac", "audio/aac",
        "pdf", "application/pdf",
        "txt", "text/plain",
        "csv", "text/csv",
        "html", "text/html",
        "htm", "text/html",
        "json", "application/json",
        "xml", "application/xml",
        "zip", "application/zip",
        "gz", "application/gzip",
        "tgz", "application/gzip",
        "7z", "application/x-7z-compressed",
        "rar", "application/vnd.rar",
        "tar", "application/x-tar",
        "doc", "application/msword",
        "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "xls", "application/vnd.ms-excel",
        "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        "ppt", "application/vnd.ms-powerpoint",
        "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        nullptr, nullptr
    }
    ;
    std::string ext = ExtOf(name);
    if (ext.empty())
    {
        return "application/octet-stream";
    }
    for (size_t i = 0; kExt[i] != nullptr; i += 2)
    {
        if (ext == kExt[i])
        {
            return kExt[i + 1];
        }
    }
    return "application/octet-stream";
}

bool IsTokenChar(char c)
{
    if ((unsigned char)c <= 0x20 || (unsigned char)c >= 0x7f)
    {
        return false;
    }
    const char *no = "()<>@,;:\\\"/[]?=";
    for (size_t i = 0; no[i] != 0; ++i)
    {
        if (no[i] == c)
        {
            return false;
        }
    }
    return true;
}

bool ValidType(const std::string &type)
{
    size_t slash = type.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= type.size())
    {
        return false;
    }
    if (type.find('/', slash + 1) != std::string::npos)
    {
        return false;
    }
    for (size_t i = 0; i < type.size(); ++i)
    {
        if (i == slash)
        {
            continue;
        }
        if (!IsTokenChar(type[i]))
        {
            return false;
        }
    }
    return true;
}

bool IsAtext(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
    {
        return true;
    }
    const char *ok = "!#$%&'*+-/=?^_`{|}~";
    for (size_t i = 0; ok[i] != 0; ++i)
    {
        if (ok[i] == c)
        {
            return true;
        }
    }
    return false;
}

bool ValidLocal(const std::string &s)
{
    if (s.empty() || s.size() > 64 || s[0] == '.' || s[s.size() - 1] == '.')
    {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '.')
        {
            if (s[i - 1] == '.')
            {
                return false;
            }
            continue;
        }
        if (!IsAtext(s[i]))
        {
            return false;
        }
    }
    return true;
}

bool ValidDomain(const std::string &s)
{
    if (s.empty() || s.size() > 253 || s[0] == '.' || s[s.size() - 1] == '.')
    {
        return false;
    }
    size_t label = 0;
    bool hyphen = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '.')
        {
            if (label == 0 || label > 63 || hyphen)
            {
                return false;
            }
            label = 0;
            hyphen = false;
            continue;
        }
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (c == '-')
        {
            if (label == 0)
            {
                return false;
            }
            hyphen = true;
            label += 1;
            continue;
        }
        if (!alnum)
        {
            return false;
        }
        hyphen = false;
        label += 1;
    }
    if (label == 0 || label > 63 || hyphen)
    {
        return false;
    }
    return true;
}

bool ValidEmail(const std::string &email)
{
    size_t at = email.find('@');
    if (at == std::string::npos || email.find('@', at + 1) != std::string::npos)
    {
        return false;
    }
    return ValidLocal(email.substr(0, at)) && ValidDomain(email.substr(at + 1));
}

std::string Unquote(const std::string &s)
{
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size() - 1)
        {
            out += s[i + 1];
            i += 1;
            continue;
        }
        out += s[i];
    }
    return out;
}

struct Addr
{
    std::string name;
    std::string email;
}
;

/* 逗号和分号分隔。引号和尖括号里的分隔符不算。 */
std::vector<std::string> SplitList(const char *text)
{
    std::vector<std::string> out;
    if (text == nullptr || text[0] == 0)
    {
        return out;
    }
    std::string cur;
    bool quote = false;
    bool angle = false;
    for (size_t i = 0; text[i] != 0; ++i)
    {
        char c = text[i];
        if (c == '"' && (i == 0 || text[i - 1] != '\\'))
        {
            quote = !quote;
            cur += c;
            continue;
        }
        if (!quote && c == '<')
        {
            angle = true;
        }
        if (!quote && c == '>')
        {
            angle = false;
        }
        if (!quote && !angle && (c == ',' || c == ';'))
        {
            std::string item = Trim(cur);
            if (!item.empty())
            {
                out.push_back(item);
            }
            cur.clear();
            continue;
        }
        cur += c;
    }
    std::string item = Trim(cur);
    if (!item.empty())
    {
        out.push_back(item);
    }
    return out;
}

bool ParseAddr(const std::string &raw, Addr *addr, std::string *bad)
{
    std::string s = Trim(raw);
    if (s.empty())
    {
        *bad = "empty";
        return false;
    }
    size_t lt = s.rfind('<');
    if (lt != std::string::npos)
    {
        size_t gt = s.find('>', lt);
        if (gt == std::string::npos || gt + 1 != s.size())
        {
            *bad = s;
            return false;
        }
        addr->email = Trim(s.substr(lt + 1, gt - lt - 1));
        addr->name = Trim(s.substr(0, lt));
        if (addr->name.size() >= 2 && addr->name[0] == '"' && addr->name[addr->name.size() - 1] == '"')
        {
            addr->name = Unquote(addr->name);
        }
    }
    else
    {
        addr->email = s;
        addr->name.clear();
    }
    if (!ValidEmail(addr->email))
    {
        *bad = s;
        return false;
    }
    return true;
}

std::string FormatMailbox(const Addr &addr)
{
    if (addr.name.empty())
    {
        return addr.email;
    }
    return FormatPhrase(addr.name) + " <" + addr.email + ">";
}

std::string AddressHeader(const char *field, const std::vector<Addr> &list)
{
    std::string out = field;
    out += ": ";
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i > 0)
        {
            out += ",\r\n ";
        }
        out += FormatMailbox(list[i]);
    }
    out += "\r\n";
    return out;
}

/* 星期和月份写死英文，避免 LC_TIME 把信头变成非 ASCII。 */
std::string DateLine()
{
    static const char *kWday[] =
    {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    }
    ;
    static const char *kMon[] =
    {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    }
    ;
    time_t now = time(nullptr);
    tm local;
    memset(&local, 0, sizeof local);
    if (localtime_r(&now, &local) == nullptr)
    {
        return "Date: Thu, 01 Jan 1970 00:00:00 +0000\r\n";
    }
    int wday = local.tm_wday;
    int mon = local.tm_mon;
    if (wday < 0 || wday > 6)
    {
        wday = 0;
    }
    if (mon < 0 || mon > 11)
    {
        mon = 0;
    }
    long off = local.tm_gmtoff;
    char sign = '+';
    if (off < 0)
    {
        sign = '-';
        if (off < -9999L * 3600L)
        {
            off = 0;
        }
        else
        {
            off = -off;
        }
    }
    int hh = (int)(off / 3600);
    int mm = (int)((off % 3600) / 60);
    if (hh > 99)
    {
        hh = 99;
    }
    char buf[128];
    snprintf(buf, sizeof buf, "Date: %s, %02d %s %04d %02d:%02d:%02d %c%02d%02d\r\n",
        kWday[wday], local.tm_mday, kMon[mon], local.tm_year + 1900,
        local.tm_hour, local.tm_min, local.tm_sec, sign, hh, mm);
    return buf;
}

std::string Hex(const unsigned char *p, size_t n)
{
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        out[i * 2] = kHex[p[i] >> 4];
        out[i * 2 + 1] = kHex[p[i] & 0xf];
    }
    return out;
}

bool RandHex(size_t bytes, std::string *out)
{
    unsigned char raw[32];
    if (bytes == 0 || bytes > sizeof raw)
    {
        return false;
    }
    if (RAND_bytes(raw, (int)bytes) != 1)
    {
        return false;
    }
    *out = Hex(raw, bytes);
    return true;
}

std::string BaseName(const char *path)
{
    const char *base = path;
    for (const char *q = path; *q != 0; ++q)
    {
        if (*q == '/')
        {
            base = q + 1;
        }
    }
    return base;
}

std::string NormCid(const char *cid)
{
    if (cid == nullptr)
    {
        return "";
    }
    std::string s = cid;
    if (s.size() >= 2 && s[0] == '<' && s[s.size() - 1] == '>')
    {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool ValidCid(const std::string &cid)
{
    if (cid.empty() || cid.size() > 200)
    {
        return false;
    }
    for (size_t i = 0; i < cid.size(); ++i)
    {
        unsigned char u = (unsigned char)cid[i];
        if (u <= 0x20 || u >= 0x7f)
        {
            return false;
        }
        if (cid[i] == '<' || cid[i] == '>' || cid[i] == '"' || cid[i] == '\\')
        {
            return false;
        }
    }
    return true;
}

/* 只有正文真的写了 cid:xxx，这张图才进 multipart/related。 */
bool CidReferenced(const char *html, const std::string &cid)
{
    if (html == nullptr || cid.empty())
    {
        return false;
    }
    for (size_t i = 0; html[i] != 0; ++i)
    {
        if (html[i + 1] == 0 || html[i + 2] == 0 || html[i + 3] == 0)
        {
            break;
        }
        if (AsciiLower(html[i]) != 'c' || AsciiLower(html[i + 1]) != 'i' || AsciiLower(html[i + 2]) != 'd' || html[i + 3] != ':')
        {
            continue;
        }
        const char *q = html + i + 4;
        if (*q == '<')
        {
            q += 1;
        }
        if (strncmp(q, cid.c_str(), cid.size()) != 0)
        {
            continue;
        }
        char next = q[cid.size()];
        if (next == 0 || next == '>' || next == '"' || next == '\'' || next == ' ' || next == '\t' || next == ')' || next == '\\')
        {
            return true;
        }
    }
    return false;
}

bool ValidHost(const char *host)
{
    if (host == nullptr || host[0] == 0)
    {
        return false;
    }
    size_t n = 0;
    for (const char *p = host; *p != 0; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c >= 0x7f)
        {
            return false;
        }
        n += 1;
        if (n > 253)
        {
            return false;
        }
    }
    return true;
}

bool IsIpLiteral(const char *host)
{
    in_addr a4;
    in6_addr a6;
    if (inet_pton(AF_INET, host, &a4) == 1)
    {
        return true;
    }
    if (inet_pton(AF_INET6, host, &a6) == 1)
    {
        return true;
    }
    return false;
}

struct Part
{
    std::string headers;
    std::string media;
    std::string boundary;
    std::string owned;
    std::string label;
    std::vector<Part> kids;
    const unsigned char *ext;
    size_t ext_size;
    size_t file_size;
    int kind;
    int fd;
    bool multi;

    Part()
        : ext(nullptr)
        , ext_size(0)
        , file_size(0)
        , kind(PAY_NONE)
        , fd(-1)
        , multi(false)
    {
    }
}
;

struct Piece
{
    std::string literal;
    std::string owned;
    std::string label;
    const unsigned char *ext;
    size_t ext_size;
    size_t file_size;
    int kind;
    int fd;

    Piece()
        : ext(nullptr)
        , ext_size(0)
        , file_size(0)
        , kind(PAY_NONE)
        , fd(-1)
    {
    }
}
;

struct FdList
{
    std::vector<int> v;

    FdList()
    {
    }

    ~FdList()
    {
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i] >= 0)
            {
                close(v[i]);
                v[i] = -1;
            }
        }
    }

    FdList(const FdList &) = delete;
    FdList &operator=(const FdList &) = delete;
}
;

struct Mail
{
    std::vector<Piece> pieces;
    FdList fds;
    std::vector<std::string> rcpt;
    std::string from;
    unsigned long long bytes;

    Mail()
        : bytes(0)
    {
    }
}
;

struct Build
{
    Mail mail;
    SparkEmailStatus status;
    std::string error;
    bool failed;
    std::vector<std::string> bounds;

    Build()
        : status(SPARK_EMAIL_ERR)
        , failed(false)
    {
    }

    void Fail(SparkEmailStatus st, const std::string &msg)
    {
        if (failed)
        {
            return;
        }
        failed = true;
        status = st;
        if (msg.empty())
        {
            error = "failed";
        }
        else
        {
            error = msg;
        }
    }
}
;

bool NewBound(Build *b, std::string *out)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        unsigned char raw[12];
        if (RAND_bytes(raw, 12) != 1)
        {
            b->Fail(SPARK_EMAIL_ERR, "random -> unavailable");
            return false;
        }
        std::string bound = "----";
        bound += Hex(raw, 12);
        bool used = false;
        for (size_t i = 0; i < b->bounds.size(); ++i)
        {
            if (b->bounds[i] == bound)
            {
                used = true;
            }
        }
        if (!used)
        {
            b->bounds.push_back(bound);
            *out = bound;
            return true;
        }
    }
    b->Fail(SPARK_EMAIL_ERR, "random -> unavailable");
    return false;
}

std::string TextHeaders(const char *media)
{
    std::string h = "Content-Type: ";
    h += media;
    h += "; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n";
    return h;
}

std::string FileHeaders(const std::string &type, const std::string &name, bool inl, const std::string &cid)
{
    std::string h = "Content-Type: ";
    h += type;
    h += ";\r\n name=";
    h += FormatParam(name);
    h += "\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: ";
    h += inl ? "inline" : "attachment";
    h += ";\r\n filename=";
    h += FormatParam(name);
    h += "\r\n";
    if (inl)
    {
        h += "Content-ID: <";
        h += cid;
        h += ">\r\n";
    }
    return h;
}

Part MakeText(const char *media, const std::string &body)
{
    Part part;
    part.media = media;
    part.kind = PAY_MEM;
    part.owned = body;
    part.headers = TextHeaders(media);
    return part;
}

bool MakeMulti(Build *b, const char *media, const char *type_param, const std::vector<Part> &kids, Part *out)
{
    std::string bound;
    if (!NewBound(b, &bound))
    {
        return false;
    }
    out->multi = true;
    out->media = media;
    out->boundary = bound;
    out->kids = kids;
    out->headers = "Content-Type: ";
    out->headers += media;
    if (type_param != nullptr && type_param[0] != 0)
    {
        out->headers += "; type=\"";
        out->headers += type_param;
        out->headers += "\"";
    }
    out->headers += "; boundary=\"";
    out->headers += bound;
    out->headers += "\"\r\n";
    return true;
}

bool LoadFile(Build *b, const SparkEmailFile *file, const char *html, Part *part, bool *inl)
{
    std::string cid = NormCid(file->cid);
    if (!cid.empty() && !ValidCid(cid))
    {
        b->Fail(SPARK_EMAIL_ERR, "cid -> invalid");
        return false;
    }
    std::string name;
    if (file->data != nullptr)
    {
        if (file->name == nullptr || file->name[0] == 0)
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> missing name");
            return false;
        }
        name = file->name;
        part->kind = PAY_MEM;
        part->ext = (const unsigned char *)file->data;
        part->ext_size = file->size;
    }
    else
    {
        if (file->path == nullptr || file->path[0] == 0)
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> missing path");
            return false;
        }
        int fd = open(file->path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            b->Fail(SPARK_EMAIL_ERR, std::string("open -> ") + file->path + ": " + strerror(errno));
            return false;
        }
        b->mail.fds.v.push_back(fd);
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        {
            b->Fail(SPARK_EMAIL_ERR, std::string("open -> ") + file->path + ": not a file");
            return false;
        }
        if (st.st_size < 0 || (unsigned long long)st.st_size > (unsigned long long)(size_t)-1)
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> too large");
            return false;
        }
        part->kind = PAY_FD;
        part->fd = fd;
        part->file_size = (size_t)st.st_size;
        part->label = file->path;
        if (file->name != nullptr && file->name[0] != 0)
        {
            name = file->name;
        }
        else
        {
            name = BaseName(file->path);
        }
        if (name.empty())
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> missing name");
            return false;
        }
    }
    std::string type;
    if (file->type != nullptr && file->type[0] != 0)
    {
        type = file->type;
        if (!ValidType(type))
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> bad type");
            return false;
        }
    }
    else
    {
        type = LookupType(name);
    }
    *inl = !cid.empty() && CidReferenced(html, cid);
    part->media = type;
    part->headers = FileHeaders(type, name, *inl, cid);
    return true;
}

void EmitBody(const Part &part, std::vector<Piece> *out)
{
    if (!part.multi)
    {
        Piece piece;
        piece.kind = part.kind;
        piece.owned = part.owned;
        piece.ext = part.ext;
        piece.ext_size = part.ext_size;
        piece.fd = part.fd;
        piece.file_size = part.file_size;
        piece.label = part.label;
        out->push_back(piece);
        return;
    }
    for (size_t i = 0; i < part.kids.size(); ++i)
    {
        Piece pre;
        pre.literal = "--";
        pre.literal += part.boundary;
        pre.literal += "\r\n";
        pre.literal += part.kids[i].headers;
        pre.literal += "\r\n";
        out->push_back(pre);
        EmitBody(part.kids[i], out);
    }
    Piece end;
    end.literal = "--";
    end.literal += part.boundary;
    end.literal += "--\r\n";
    out->push_back(end);
}

bool ParseList(Build *b, const char *text, std::vector<Addr> *out)
{
    std::vector<std::string> items = SplitList(text);
    if (out->size() + items.size() > 1024)
    {
        b->Fail(SPARK_EMAIL_ERR, "recipients -> too many");
        return false;
    }
    for (size_t i = 0; i < items.size(); ++i)
    {
        Addr addr;
        std::string bad;
        if (!ParseAddr(items[i], &addr, &bad))
        {
            b->Fail(SPARK_EMAIL_ERR, "address -> " + bad);
            return false;
        }
        out->push_back(addr);
    }
    return true;
}

void PushRcpt(std::vector<std::string> *rcpt, const std::string &email)
{
    for (size_t i = 0; i < rcpt->size(); ++i)
    {
        if (EqualFold((*rcpt)[i], email))
        {
            return;
        }
    }
    rcpt->push_back(email);
}

unsigned long long PieceBytes(const Piece &piece)
{
    if (piece.kind == PAY_FD)
    {
        return Base64Len(piece.file_size);
    }
    if (piece.kind == PAY_MEM)
    {
        if (piece.ext != nullptr)
        {
            return Base64Len(piece.ext_size);
        }
        return Base64Len(piece.owned.size());
    }
    return 0;
}

bool Compose(Build *b, const SparkEmailServer *server, const SparkEmailMessage *message)
{
    if (server == nullptr || message == nullptr)
    {
        b->Fail(SPARK_EMAIL_ERR, "argument -> required");
        return false;
    }
    if (!ValidHost(server->host))
    {
        b->Fail(SPARK_EMAIL_ERR, "host -> invalid");
        return false;
    }
    if (server->port < 0 || server->port > 65535)
    {
        b->Fail(SPARK_EMAIL_ERR, "port -> invalid");
        return false;
    }
    if (server->tls < SPARK_EMAIL_TLS_AUTO || server->tls > SPARK_EMAIL_TLS_NONE)
    {
        b->Fail(SPARK_EMAIL_ERR, "tls -> invalid");
        return false;
    }
    if (server->timeout_ms < 0)
    {
        b->Fail(SPARK_EMAIL_ERR, "timeout -> invalid");
        return false;
    }
    if (message->file_count > 1024 || (message->file_count > 0 && message->files == nullptr))
    {
        b->Fail(SPARK_EMAIL_ERR, "file -> invalid");
        return false;
    }

    std::vector<std::string> from_items = SplitList(message->from);
    if (from_items.size() != 1)
    {
        b->Fail(SPARK_EMAIL_ERR, from_items.empty() ? "from -> required" : "from -> one address");
        return false;
    }
    Addr from;
    std::string bad;
    if (!ParseAddr(from_items[0], &from, &bad))
    {
        b->Fail(SPARK_EMAIL_ERR, "address -> " + bad);
        return false;
    }
    std::vector<Addr> to;
    std::vector<Addr> cc;
    std::vector<Addr> bcc;
    if (!ParseList(b, message->to, &to) || !ParseList(b, message->cc, &cc) || !ParseList(b, message->bcc, &bcc))
    {
        return false;
    }
    if (to.empty() && cc.empty() && bcc.empty())
    {
        b->Fail(SPARK_EMAIL_ERR, "recipients -> required");
        return false;
    }
    b->mail.from = from.email;
    for (size_t i = 0; i < to.size(); ++i)
    {
        PushRcpt(&b->mail.rcpt, to[i].email);
    }
    for (size_t i = 0; i < cc.size(); ++i)
    {
        PushRcpt(&b->mail.rcpt, cc[i].email);
    }
    for (size_t i = 0; i < bcc.size(); ++i)
    {
        PushRcpt(&b->mail.rcpt, bcc[i].email);
    }

    std::vector<Part> inlines;
    std::vector<Part> atts;
    for (size_t i = 0; i < message->file_count; ++i)
    {
        Part part;
        bool inl = false;
        if (!LoadFile(b, &message->files[i], message->html, &part, &inl))
        {
            return false;
        }
        if (inl)
        {
            inlines.push_back(part);
        }
        else
        {
            atts.push_back(part);
        }
    }

    bool has_text = message->text != nullptr;
    bool has_html = message->html != nullptr;
    Part body;
    if (has_text && has_html)
    {
        std::vector<Part> kids;
        kids.push_back(MakeText("text/plain", NormalizeCrlf(message->text)));
        kids.push_back(MakeText("text/html", NormalizeCrlf(message->html)));
        if (!MakeMulti(b, "multipart/alternative", nullptr, kids, &body))
        {
            return false;
        }
    }
    else if (has_html)
    {
        body = MakeText("text/html", NormalizeCrlf(message->html));
    }
    else
    {
        body = MakeText("text/plain", NormalizeCrlf(has_text ? message->text : ""));
    }
    if (!inlines.empty())
    {
        std::vector<Part> kids;
        kids.push_back(body);
        for (size_t i = 0; i < inlines.size(); ++i)
        {
            kids.push_back(inlines[i]);
        }
        Part wrapped;
        if (!MakeMulti(b, "multipart/related", body.media.c_str(), kids, &wrapped))
        {
            return false;
        }
        body = wrapped;
    }
    if (!atts.empty())
    {
        std::vector<Part> kids;
        kids.push_back(body);
        for (size_t i = 0; i < atts.size(); ++i)
        {
            kids.push_back(atts[i]);
        }
        Part wrapped;
        if (!MakeMulti(b, "multipart/mixed", nullptr, kids, &wrapped))
        {
            return false;
        }
        body = wrapped;
    }

    std::string id;
    if (!RandHex(16, &id))
    {
        b->Fail(SPARK_EMAIL_ERR, "random -> unavailable");
        return false;
    }
    std::string domain = from.email.substr(from.email.find('@') + 1);
    std::string head = DateLine();
    head += "From: ";
    head += FormatMailbox(from);
    head += "\r\n";
    /* 只有密送时也要有 To，否则有的客户端把信当成残缺的。Bcc 本身不写。 */
    if (to.empty() && cc.empty())
    {
        head += "To: undisclosed-recipients:;\r\n";
    }
    else if (!to.empty())
    {
        head += AddressHeader("To", to);
    }
    if (!cc.empty())
    {
        head += AddressHeader("Cc", cc);
    }
    head += "Subject: ";
    head += FormatSubject(message->subject != nullptr ? message->subject : "");
    head += "\r\nMessage-ID: <";
    head += id;
    head += "@";
    head += domain;
    head += ">\r\nMIME-Version: 1.0\r\n";
    head += body.headers;
    head += "\r\n";

    Piece first;
    first.literal = head;
    b->mail.pieces.push_back(first);
    EmitBody(body, &b->mail.pieces);

    unsigned long long bytes = 0;
    for (size_t i = 0; i < b->mail.pieces.size(); ++i)
    {
        if (!AddSize(&bytes, b->mail.pieces[i].literal.size()) || !AddSize(&bytes, PieceBytes(b->mail.pieces[i])))
        {
            b->Fail(SPARK_EMAIL_ERR, "file -> too large");
            return false;
        }
    }
    b->mail.bytes = bytes;
    return true;
}

struct Reply
{
    int code;
    std::vector<std::string> lines;
    std::string text;

    Reply()
        : code(0)
    {
    }
}
;

struct Caps
{
    bool starttls;
    bool plain;
    bool login;
    bool has_size;
    unsigned long long size_limit;

    Caps()
        : starttls(false)
        , plain(false)
        , login(false)
        , has_size(false)
        , size_limit(0)
    {
    }
}
;

struct AddrGuard
{
    addrinfo *p;

    AddrGuard()
        : p(nullptr)
    {
    }

    ~AddrGuard()
    {
        if (p != nullptr)
        {
            freeaddrinfo(p);
            p = nullptr;
        }
    }

    AddrGuard(const AddrGuard &) = delete;
    AddrGuard &operator=(const AddrGuard &) = delete;
}
;

int ElapsedMs(const timespec &start)
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec = now.tv_sec - start.tv_sec;
    long nsec = now.tv_nsec - start.tv_nsec;
    if (nsec < 0)
    {
        sec -= 1;
        nsec += 1000000000L;
    }
    if (sec < 0)
    {
        return 0;
    }
    if (sec > 2000000L)
    {
        return INT_MAX;
    }
    long ms = sec * 1000L + nsec / 1000000L;
    if (ms > INT_MAX)
    {
        return INT_MAX;
    }
    return (int)ms;
}

int FinalTimeout(int ms)
{
    if (ms > INT_MAX / 4)
    {
        return INT_MAX;
    }
    return ms * 4;
}

std::string JoinLines(const std::vector<std::string> &lines)
{
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!text.empty())
        {
            text += " / ";
        }
        text += lines[i];
    }
    if (text.size() > 400)
    {
        text.resize(400);
    }
    return text;
}

std::vector<std::string> Tokens(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == ' ' || s[i] == '\t')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur += s[i];
        }
    }
    if (!cur.empty())
    {
        out.push_back(cur);
    }
    return out;
}

bool ParseUll(const std::string &s, unsigned long long *v)
{
    if (s.empty())
    {
        return false;
    }
    unsigned long long n = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] < '0' || s[i] > '9')
        {
            return false;
        }
        unsigned long long d = (unsigned long long)(s[i] - '0');
        if (n > (kTooBig - d) / 10)
        {
            return false;
        }
        n = n * 10 + d;
    }
    *v = n;
    return true;
}

void AddAuth(Caps *caps, const std::string &token)
{
    if (EqualFold(token, "PLAIN"))
    {
        caps->plain = true;
    }
    else if (EqualFold(token, "LOGIN"))
    {
        caps->login = true;
    }
}

void ParseCaps(const Reply &reply, Caps *caps)
{
    *caps = Caps();
    for (size_t i = 0; i < reply.lines.size(); ++i)
    {
        const std::string &line = reply.lines[i];
        if (line.size() < 4)
        {
            continue;
        }
        std::vector<std::string> tok = Tokens(line.substr(4));
        if (tok.empty())
        {
            continue;
        }
        std::string key = tok[0];
        std::string extra;
        size_t eq = key.find('=');
        if (eq != std::string::npos)
        {
            extra = key.substr(eq + 1);
            key = key.substr(0, eq);
        }
        if (EqualFold(key, "STARTTLS"))
        {
            caps->starttls = true;
        }
        else if (EqualFold(key, "SIZE"))
        {
            std::string num = extra;
            if (num.empty() && tok.size() >= 2)
            {
                num = tok[1];
            }
            unsigned long long v = 0;
            if (ParseUll(num, &v))
            {
                caps->has_size = true;
                caps->size_limit = v;
            }
        }
        else if (EqualFold(key, "AUTH"))
        {
            if (!extra.empty())
            {
                AddAuth(caps, extra);
            }
            for (size_t k = 1; k < tok.size(); ++k)
            {
                AddAuth(caps, tok[k]);
            }
        }
    }
}

std::string EhloName(int fd)
{
    sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t len = sizeof ss;
    if (getsockname(fd, (sockaddr *)&ss, &len) != 0)
    {
        return "[127.0.0.1]";
    }
    char ip[INET6_ADDRSTRLEN];
    if (ss.ss_family == AF_INET)
    {
        sockaddr_in *in = (sockaddr_in *)&ss;
        if (inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip) == nullptr)
        {
            return "[127.0.0.1]";
        }
        return std::string("[") + ip + "]";
    }
    if (ss.ss_family == AF_INET6)
    {
        sockaddr_in6 *in6 = (sockaddr_in6 *)&ss;
        if (inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof ip) == nullptr)
        {
            return "[IPv6:::1]";
        }
        return std::string("[IPv6:") + ip + "]";
    }
    return "[127.0.0.1]";
}

std::string TlsMessage()
{
    unsigned long e = ERR_get_error();
    if (e == 0)
    {
        return "handshake failed";
    }
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

struct Conn
{
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
    int timeout_ms;
    bool timed_out;
    bool failed;
    bool insecure;
    bool can_quit;
    bool in_data;
    bool quit_sent;
    SparkEmailStatus status;
    std::string error;
    std::string rbuf;
    const char *step;
    Caps caps;
    void (*trace)(void *user, const char *line);
    void *trace_user;

    Conn()
        : fd(-1)
        , ssl(nullptr)
        , ctx(nullptr)
        , timeout_ms(30000)
        , timed_out(false)
        , failed(false)
        , insecure(false)
        , can_quit(false)
        , in_data(false)
        , quit_sent(false)
        , status(SPARK_EMAIL_ERR)
        , step("")
        , trace(nullptr)
        , trace_user(nullptr)
    {
    }

    ~Conn()
    {
        Close();
    }

    Conn(const Conn &) = delete;
    Conn &operator=(const Conn &) = delete;

    void Fail(SparkEmailStatus st, const std::string &msg);
    void Trace(const char *line);
    bool Wait(short ev);
    bool WriteAll(const char *p, size_t n);
    bool Fill();
    bool ReadLine(std::string *line);
    bool ReadReply(Reply *reply);
    bool SendLine(const std::string &cmd, const char *shown);
    bool Expect(int exact, int klass, const char *what);
    bool Connect(const char *host, int port);
    void TlsFail(int err);
    bool Handshake();
    bool SetupTls(const char *host, const char *ca, int insecure_flag);
    bool Ehlo(bool allow_helo);
    bool Auth(const char *user, const char *pass);
    bool WriteMessage(const Mail *mail);
    bool Deliver(const Mail *mail, const SparkEmailServer *server, int mode, int port);
    void Close();
}
;

void Conn::Fail(SparkEmailStatus st, const std::string &msg)
{
    if (failed)
    {
        return;
    }
    failed = true;
    status = st;
    std::string why = msg.empty() ? "failed" : msg;
    if (step != nullptr && step[0] != 0)
    {
        error = std::string(step) + " -> " + why;
    }
    else
    {
        error = why;
    }
}

void Conn::Trace(const char *line)
{
    if (trace == nullptr || line == nullptr)
    {
        return;
    }
    trace(trace_user, line);
}

bool Conn::Wait(short ev)
{
    if (failed)
    {
        return false;
    }
    for (;;)
    {
        pollfd p;
        memset(&p, 0, sizeof p);
        p.fd = fd;
        p.events = ev;
        int rc = poll(&p, 1, timeout_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            Fail(SPARK_EMAIL_AGAIN, strerror(errno));
            return false;
        }
        if (rc == 0)
        {
            timed_out = true;
            Fail(SPARK_EMAIL_AGAIN, "timeout");
            return false;
        }
        return true;
    }
}

void Conn::TlsFail(int err)
{
    if (failed || timed_out)
    {
        return;
    }
    if (err == SSL_ERROR_SYSCALL || err == SSL_ERROR_ZERO_RETURN)
    {
        if (err == SSL_ERROR_SYSCALL && errno != 0)
        {
            Fail(SPARK_EMAIL_AGAIN, strerror(errno));
        }
        else
        {
            Fail(SPARK_EMAIL_AGAIN, "connection closed");
        }
        return;
    }
    if (!insecure && ssl != nullptr)
    {
        long v = SSL_get_verify_result(ssl);
        if (v != X509_V_OK)
        {
            step = "certificate";
            Fail(SPARK_EMAIL_ERR, X509_verify_cert_error_string(v));
            return;
        }
    }
    Fail(SPARK_EMAIL_ERR, TlsMessage());
}

bool Conn::WriteAll(const char *p, size_t n)
{
    if (failed)
    {
        return false;
    }
    while (n > 0)
    {
        if (ssl != nullptr)
        {
            size_t chunk = n;
            if (chunk > 16384)
            {
                chunk = 16384;
            }
            int w = SSL_write(ssl, p, (int)chunk);
            if (w > 0)
            {
                p += w;
                n -= (size_t)w;
                continue;
            }
            int err = SSL_get_error(ssl, w);
            if (err == SSL_ERROR_WANT_READ)
            {
                if (!Wait(POLLIN))
                {
                    return false;
                }
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE)
            {
                if (!Wait(POLLOUT))
                {
                    return false;
                }
                continue;
            }
            if (err == SSL_ERROR_SYSCALL && errno == EINTR)
            {
                continue;
            }
            TlsFail(err);
            return false;
        }
        size_t chunk = n;
        if (chunk > 1048576)
        {
            chunk = 1048576;
        }
        ssize_t w = send(fd, p, chunk, MSG_NOSIGNAL);
        if (w > 0)
        {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
        {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!Wait(POLLOUT))
            {
                return false;
            }
            continue;
        }
        Fail(SPARK_EMAIL_AGAIN, w < 0 ? strerror(errno) : "connection closed");
        return false;
    }
    return true;
}

bool Conn::Fill()
{
    if (failed)
    {
        return false;
    }
    char tmp[2048];
    for (;;)
    {
        if (ssl != nullptr)
        {
            int n = SSL_read(ssl, tmp, (int)sizeof tmp);
            if (n > 0)
            {
                rbuf.append(tmp, (size_t)n);
                return true;
            }
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ)
            {
                if (!Wait(POLLIN))
                {
                    return false;
                }
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE)
            {
                if (!Wait(POLLOUT))
                {
                    return false;
                }
                continue;
            }
            if (err == SSL_ERROR_SYSCALL && errno == EINTR)
            {
                continue;
            }
            TlsFail(err);
            return false;
        }
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n > 0)
        {
            rbuf.append(tmp, (size_t)n);
            return true;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (!Wait(POLLIN))
            {
                return false;
            }
            continue;
        }
        Fail(SPARK_EMAIL_AGAIN, n < 0 ? strerror(errno) : "connection closed");
        return false;
    }
}

bool Conn::ReadLine(std::string *line)
{
    if (failed)
    {
        return false;
    }
    for (;;)
    {
        size_t pos = rbuf.find('\n');
        if (pos != std::string::npos)
        {
            *line = rbuf.substr(0, pos);
            rbuf.erase(0, pos + 1);
            if (!line->empty() && (*line)[line->size() - 1] == '\r')
            {
                line->erase(line->size() - 1);
            }
            return true;
        }
        if (rbuf.size() > 8192)
        {
            Fail(SPARK_EMAIL_ERR, "line too long");
            return false;
        }
        if (!Fill())
        {
            return false;
        }
    }
}

bool Conn::ReadReply(Reply *reply)
{
    reply->code = 0;
    reply->lines.clear();
    reply->text.clear();
    int expect = -1;
    size_t total = 0;
    for (;;)
    {
        std::string line;
        if (!ReadLine(&line))
        {
            return false;
        }
        Trace(line.c_str());
        total += line.size() + 1;
        if (total > 65536)
        {
            Fail(SPARK_EMAIL_ERR, "too long");
            return false;
        }
        if (line.size() < 3 || !IsDigit(line[0]) || !IsDigit(line[1]) || !IsDigit(line[2]))
        {
            Fail(SPARK_EMAIL_ERR, line);
            return false;
        }
        int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        bool more = false;
        if (line.size() >= 4)
        {
            if (line[3] == '-')
            {
                more = true;
            }
            else if (line[3] != ' ')
            {
                Fail(SPARK_EMAIL_ERR, line);
                return false;
            }
        }
        if (expect >= 0 && code != expect)
        {
            Fail(SPARK_EMAIL_ERR, line);
            return false;
        }
        expect = code;
        reply->lines.push_back(line);
        if (!more)
        {
            reply->code = code;
            reply->text = JoinLines(reply->lines);
            return true;
        }
    }
}

bool Conn::SendLine(const std::string &cmd, const char *shown)
{
    if (failed)
    {
        return false;
    }
    Trace(shown != nullptr ? shown : cmd.c_str());
    std::string wire = cmd;
    wire += "\r\n";
    return WriteAll(wire.data(), wire.size());
}

bool Conn::Expect(int exact, int klass, const char *what)
{
    step = what;
    Reply reply;
    if (!ReadReply(&reply))
    {
        return false;
    }
    bool ok = exact >= 0 ? reply.code == exact : reply.code / 100 == klass;
    if (!ok)
    {
        SparkEmailStatus st = reply.code / 100 == 4 ? SPARK_EMAIL_AGAIN : SPARK_EMAIL_ERR;
        Fail(st, reply.text);
        return false;
    }
    return true;
}

bool Conn::Connect(const char *host, int port)
{
    step = "resolve";
    char port_text[8];
    snprintf(port_text, sizeof port_text, "%d", port);
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_ADDRCONFIG;
    AddrGuard guard;
    int g = getaddrinfo(host, port_text, &hints, &guard.p);
    if (g != 0)
    {
        Fail(SPARK_EMAIL_AGAIN, gai_strerror(g));
        return false;
    }

    step = "connect";
    FdList pending;
    int chosen = -1;
    int last_err = 0;
    for (addrinfo *ai = guard.p; ai != nullptr; ai = ai->ai_next)
    {
        int one = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (one < 0)
        {
            last_err = errno;
            continue;
        }
        int nodelay = 1;
        setsockopt(one, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        int rc = connect(one, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0)
        {
            chosen = one;
            break;
        }
        if (errno == EINPROGRESS)
        {
            pending.v.push_back(one);
            continue;
        }
        last_err = errno;
        close(one);
    }
    if (chosen >= 0)
    {
        fd = chosen;
        return true;
    }
    if (pending.v.empty())
    {
        Fail(SPARK_EMAIL_AGAIN, last_err != 0 ? strerror(last_err) : "failed");
        return false;
    }

    timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!failed)
    {
        std::vector<pollfd> pfds;
        std::vector<size_t> map;
        for (size_t i = 0; i < pending.v.size(); ++i)
        {
            if (pending.v[i] < 0)
            {
                continue;
            }
            pollfd p;
            memset(&p, 0, sizeof p);
            p.fd = pending.v[i];
            p.events = POLLOUT;
            pfds.push_back(p);
            map.push_back(i);
        }
        if (pfds.empty())
        {
            Fail(SPARK_EMAIL_AGAIN, last_err != 0 ? strerror(last_err) : "failed");
            return false;
        }
        int remain = timeout_ms - ElapsedMs(start);
        if (remain <= 0)
        {
            timed_out = true;
            Fail(SPARK_EMAIL_AGAIN, "timeout");
            return false;
        }
        int rc = poll(&pfds[0], (nfds_t)pfds.size(), remain);
        if (rc < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            Fail(SPARK_EMAIL_AGAIN, strerror(errno));
            return false;
        }
        if (rc == 0)
        {
            timed_out = true;
            Fail(SPARK_EMAIL_AGAIN, "timeout");
            return false;
        }
        for (size_t k = 0; k < pfds.size(); ++k)
        {
            if (pfds[k].revents == 0)
            {
                continue;
            }
            size_t idx = map[k];
            int so = 0;
            socklen_t sl = sizeof so;
            if (getsockopt(pending.v[idx], SOL_SOCKET, SO_ERROR, &so, &sl) != 0)
            {
                so = errno;
            }
            if (so == 0 && (pfds[k].revents & POLLOUT) != 0)
            {
                fd = pending.v[idx];
                pending.v[idx] = -1;
                return true;
            }
            if (so != 0)
            {
                last_err = so;
            }
            close(pending.v[idx]);
            pending.v[idx] = -1;
        }
    }
    return false;
}

bool Conn::Handshake()
{
    step = "tls";
    for (;;)
    {
        int rc = SSL_connect(ssl);
        if (rc == 1)
        {
            return true;
        }
        int err = SSL_get_error(ssl, rc);
        if (err == SSL_ERROR_WANT_READ)
        {
            if (!Wait(POLLIN))
            {
                return false;
            }
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE)
        {
            if (!Wait(POLLOUT))
            {
                return false;
            }
            continue;
        }
        if (err == SSL_ERROR_SYSCALL && errno == EINTR)
        {
            continue;
        }
        TlsFail(err);
        return false;
    }
}

bool Conn::SetupTls(const char *host, const char *ca, int insecure_flag)
{
    step = "tls";
    insecure = insecure_flag != 0;
    ERR_clear_error();
    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr)
    {
        Fail(SPARK_EMAIL_ERR, TlsMessage());
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (insecure)
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }
    else
    {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        int ok = 0;
        if (ca != nullptr && ca[0] != 0)
        {
            ok = SSL_CTX_load_verify_locations(ctx, ca, nullptr);
        }
        else
        {
            ok = SSL_CTX_set_default_verify_paths(ctx);
        }
        if (ok != 1)
        {
            step = "certificate";
            Fail(SPARK_EMAIL_ERR, "cannot load trust store");
            return false;
        }
    }
    ssl = SSL_new(ctx);
    if (ssl == nullptr)
    {
        Fail(SPARK_EMAIL_ERR, TlsMessage());
        return false;
    }
    SSL_set_fd(ssl, fd);
    if (!IsIpLiteral(host))
    {
        if (SSL_set_tlsext_host_name(ssl, host) != 1)
        {
            Fail(SPARK_EMAIL_ERR, "sni");
            return false;
        }
    }
    if (!insecure && SSL_set1_host(ssl, host) != 1)
    {
        step = "certificate";
        Fail(SPARK_EMAIL_ERR, "host");
        return false;
    }
    return Handshake();
}

bool Conn::Ehlo(bool allow_helo)
{
    caps = Caps();
    std::string name = EhloName(fd);
    step = "EHLO";
    if (!SendLine("EHLO " + name, nullptr))
    {
        return false;
    }
    Reply reply;
    if (!ReadReply(&reply))
    {
        return false;
    }
    if (reply.code / 100 == 5 && allow_helo)
    {
        step = "HELO";
        if (!SendLine("HELO " + name, nullptr))
        {
            return false;
        }
        if (!ReadReply(&reply))
        {
            return false;
        }
        if (reply.code / 100 != 2)
        {
            SparkEmailStatus st = reply.code / 100 == 4 ? SPARK_EMAIL_AGAIN : SPARK_EMAIL_ERR;
            Fail(st, reply.text);
            return false;
        }
        caps = Caps();
        return true;
    }
    if (reply.code / 100 != 2)
    {
        SparkEmailStatus st = reply.code / 100 == 4 ? SPARK_EMAIL_AGAIN : SPARK_EMAIL_ERR;
        Fail(st, reply.text);
        return false;
    }
    ParseCaps(reply, &caps);
    return true;
}

bool Conn::Auth(const char *user, const char *pass)
{
    if (user == nullptr || user[0] == 0)
    {
        return true;
    }
    if (pass == nullptr)
    {
        pass = "";
    }
    step = "AUTH";
    if (caps.plain)
    {
        std::string raw;
        raw.push_back('\0');
        raw += user;
        raw.push_back('\0');
        raw += pass;
        std::string cmd = "AUTH PLAIN ";
        cmd += Base64Raw((const unsigned char *)raw.data(), raw.size());
        if (!SendLine(cmd, "AUTH PLAIN ***"))
        {
            return false;
        }
        return Expect(235, 2, "AUTH");
    }
    if (caps.login)
    {
        if (!SendLine("AUTH LOGIN", nullptr))
        {
            return false;
        }
        if (!Expect(334, 3, "AUTH"))
        {
            return false;
        }
        if (!SendLine(Base64Raw((const unsigned char *)user, strlen(user)), "***"))
        {
            return false;
        }
        if (!Expect(334, 3, "AUTH"))
        {
            return false;
        }
        if (!SendLine(Base64Raw((const unsigned char *)pass, strlen(pass)), "***"))
        {
            return false;
        }
        return Expect(235, 2, "AUTH");
    }
    Fail(SPARK_EMAIL_ERR, "not advertised");
    return false;
}

struct Writer
{
    Conn *c;
    bool bol;
    char buf[16384];
    size_t len;

    Writer()
        : c(nullptr)
        , bol(true)
        , len(0)
    {
    }

    bool Flush()
    {
        if (len == 0)
        {
            return true;
        }
        if (!c->WriteAll(buf, len))
        {
            return false;
        }
        len = 0;
        return true;
    }

    bool Push(char ch)
    {
        if (len == sizeof buf)
        {
            if (!Flush())
            {
                return false;
            }
        }
        buf[len] = ch;
        len += 1;
        return true;
    }

    /* 行首的点多写一个。缓冲按块写，避免大附件一行一次系统调用。 */
    bool Write(const char *p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            if (bol && p[i] == '.')
            {
                if (!Push('.'))
                {
                    return false;
                }
            }
            if (!Push(p[i]))
            {
                return false;
            }
            bol = p[i] == '\n';
        }
        return true;
    }

    bool WriteBase64(const unsigned char *data, size_t n)
    {
        size_t i = 0;
        while (i < n)
        {
            size_t m = n - i;
            if (m > 57)
            {
                m = 57;
            }
            char line[80];
            size_t got = EncodeRaw(data + i, m, line);
            line[got] = '\r';
            line[got + 1] = '\n';
            if (!Write(line, got + 2))
            {
                return false;
            }
            i += m;
        }
        return true;
    }

    bool WriteFile(int fd, size_t total, const char *label)
    {
        unsigned char block[57 * 64];
        size_t left = total;
        while (left > 0)
        {
            size_t want = sizeof block;
            if (want > left)
            {
                want = left;
            }
            if (want < left)
            {
                want -= want % 57;
            }
            size_t got = 0;
            while (got < want)
            {
                ssize_t n = read(fd, block + got, want - got);
                if (n < 0 && errno == EINTR)
                {
                    continue;
                }
                if (n <= 0)
                {
                    c->step = "read";
                    std::string why = label != nullptr ? label : "attachment";
                    if (n < 0)
                    {
                        why += ": ";
                        why += strerror(errno);
                    }
                    else
                    {
                        why += ": short file";
                    }
                    c->Fail(SPARK_EMAIL_ERR, why);
                    return false;
                }
                got += (size_t)n;
            }
            if (!WriteBase64(block, got))
            {
                return false;
            }
            left -= got;
        }
        return true;
    }
}
;

bool Conn::WriteMessage(const Mail *mail)
{
    step = "DATA";
    Writer writer;
    writer.c = this;
    for (size_t i = 0; i < mail->pieces.size(); ++i)
    {
        const Piece &piece = mail->pieces[i];
        if (!piece.literal.empty() && !writer.Write(piece.literal.data(), piece.literal.size()))
        {
            return false;
        }
        if (piece.kind == PAY_MEM)
        {
            if (piece.ext != nullptr)
            {
                if (!writer.WriteBase64(piece.ext, piece.ext_size))
                {
                    return false;
                }
            }
            else if (!piece.owned.empty())
            {
                if (!writer.WriteBase64((const unsigned char *)piece.owned.data(), piece.owned.size()))
                {
                    return false;
                }
            }
        }
        else if (piece.kind == PAY_FD)
        {
            if (!writer.WriteFile(piece.fd, piece.file_size, piece.label.c_str()))
            {
                return false;
            }
        }
    }
    if (!writer.Flush())
    {
        return false;
    }
    /* 结束点必须独占一行，而且这一行本身不再做点转义。 */
    if (!writer.bol && !WriteAll("\r\n", 2))
    {
        return false;
    }
    return true;
}

bool Conn::Deliver(const Mail *mail, const SparkEmailServer *server, int mode, int port)
{
    bool implicit = mode == SPARK_EMAIL_TLS_IMPLICIT || (mode == SPARK_EMAIL_TLS_AUTO && port == 465);
    bool starttls = !implicit && mode != SPARK_EMAIL_TLS_NONE;
    if (implicit && !SetupTls(server->host, server->ca_file, server->insecure))
    {
        return false;
    }
    if (!Expect(220, 2, "greeting"))
    {
        return false;
    }
    can_quit = true;
    if (!Ehlo(true))
    {
        return false;
    }
    if (starttls)
    {
        if (!caps.starttls)
        {
            step = "STARTTLS";
            Fail(SPARK_EMAIL_ERR, "not advertised");
            return false;
        }
        step = "STARTTLS";
        if (!SendLine("STARTTLS", nullptr))
        {
            return false;
        }
        if (!Expect(220, 2, "STARTTLS"))
        {
            return false;
        }
        /* 220 后面如果还剩明文，说明已经读进了握手字节。 */
        if (!rbuf.empty())
        {
            step = "STARTTLS";
            Fail(SPARK_EMAIL_ERR, "trailing data");
            return false;
        }
        can_quit = false;
        if (!SetupTls(server->host, server->ca_file, server->insecure))
        {
            return false;
        }
        can_quit = true;
        if (!Ehlo(true))
        {
            return false;
        }
    }
    if (!Auth(server->username, server->password))
    {
        return false;
    }
    if (caps.has_size && caps.size_limit > 0 && mail->bytes > caps.size_limit)
    {
        step = "SIZE";
        Fail(SPARK_EMAIL_ERR, "message too large");
        return false;
    }
    std::string mailfrom = "MAIL FROM:<";
    mailfrom += mail->from;
    mailfrom += ">";
    step = "MAIL FROM";
    if (!SendLine(mailfrom, nullptr) || !Expect(-1, 2, "MAIL FROM"))
    {
        return false;
    }
    for (size_t i = 0; i < mail->rcpt.size(); ++i)
    {
        std::string rcpt = "RCPT TO:<";
        rcpt += mail->rcpt[i];
        rcpt += ">";
        step = "RCPT TO";
        if (!SendLine(rcpt, nullptr) || !Expect(-1, 2, "RCPT TO"))
        {
            return false;
        }
    }
    step = "DATA";
    if (!SendLine("DATA", nullptr) || !Expect(354, 3, "DATA"))
    {
        return false;
    }
    in_data = true;
    if (!WriteMessage(mail) || !WriteAll(".\r\n", 3))
    {
        return false;
    }
    int saved = timeout_ms;
    timeout_ms = FinalTimeout(saved);
    step = ".";
    Reply reply;
    bool got = ReadReply(&reply);
    timeout_ms = saved;
    if (got)
    {
        in_data = false;
    }
    if (!got)
    {
        return false;
    }
    if (reply.code / 100 != 2)
    {
        SparkEmailStatus st = reply.code / 100 == 4 ? SPARK_EMAIL_AGAIN : SPARK_EMAIL_ERR;
        Fail(st, reply.text);
        return false;
    }
    return true;
}

void Conn::Close()
{
    if (fd >= 0 && can_quit && !in_data && !quit_sent)
    {
        quit_sent = true;
        if (ssl != nullptr)
        {
            SSL_write(ssl, "QUIT\r\n", 6);
        }
        else
        {
            send(fd, "QUIT\r\n", 6, MSG_NOSIGNAL | MSG_DONTWAIT);
        }
    }
    if (ssl != nullptr)
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = nullptr;
    }
    if (ctx != nullptr)
    {
        SSL_CTX_free(ctx);
        ctx = nullptr;
    }
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
}

struct SigpipeGuard
{
    sigset_t old;
    bool have;
    bool was_pending;

    SigpipeGuard()
        : have(false)
        , was_pending(false)
    {
        sigset_t pending;
        sigemptyset(&pending);
        if (sigpending(&pending) == 0)
        {
            was_pending = sigismember(&pending, SIGPIPE) == 1;
        }
        sigset_t block;
        sigemptyset(&block);
        sigaddset(&block, SIGPIPE);
        have = pthread_sigmask(SIG_BLOCK, &block, &old) == 0;
    }

    ~SigpipeGuard()
    {
        if (!have)
        {
            return;
        }
        if (!was_pending)
        {
            sigset_t pending;
            sigemptyset(&pending);
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
            {
                sigset_t one;
                sigemptyset(&one);
                sigaddset(&one, SIGPIPE);
                timespec zero;
                zero.tv_sec = 0;
                zero.tv_nsec = 0;
                sigtimedwait(&one, nullptr, &zero);
            }
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }
}
;

void CopyOut(char *dst, size_t n, const std::string &s)
{
    if (dst == nullptr || n == 0)
    {
        return;
    }
    size_t m = s.size();
    if (m >= n)
    {
        m = n - 1;
    }
    if (m > 0)
    {
        memcpy(dst, s.data(), m);
    }
    dst[m] = 0;
}

int ResolvePort(const SparkEmailServer *server)
{
    if (server->port != 0)
    {
        return server->port;
    }
    if (server->tls == SPARK_EMAIL_TLS_STARTTLS)
    {
        return 587;
    }
    if (server->tls == SPARK_EMAIL_TLS_NONE)
    {
        return 25;
    }
    return 465;
}

}

extern "C" SparkEmailStatus spark_email_send(const SparkEmailServer *server, const SparkEmailMessage *message, char *err, size_t err_size)
{
    if (err != nullptr && err_size > 0)
    {
        err[0] = 0;
    }
    SigpipeGuard guard;
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    Build build;
    if (!Compose(&build, server, message))
    {
        CopyOut(err, err_size, build.error);
        return build.status;
    }

    Conn conn;
    conn.trace = server->trace;
    conn.trace_user = server->trace_user;
    conn.timeout_ms = server->timeout_ms > 0 ? server->timeout_ms : 30000;
    int port = ResolvePort(server);
    if (!conn.Connect(server->host, port) || !conn.Deliver(&build.mail, server, server->tls, port))
    {
        CopyOut(err, err_size, conn.error);
        return conn.status;
    }

    conn.quit_sent = true;
    conn.step = "QUIT";
    if (!conn.SendLine("QUIT", nullptr))
    {
        conn.failed = false;
        conn.status = SPARK_EMAIL_OK;
        conn.error.clear();
    }
    return SPARK_EMAIL_OK;
}
