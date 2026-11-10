/**
 * @file    demo_main.cpp
 * @brief   调用 spark_email_send。主机、账号、地址和正文都在运行时传入。
 *
 *   demo_main --host smtp.example.com --user a@example.com \
 *     --from "名字 <a@example.com>" --to b@example.com \
 *     --subject 主题 --text 正文 --html "<p>正文</p>" \
 *     --file 图片.jpg|snap.jpg|image/jpeg|snap|0 \
 *     --file 视频.mp4|视频.mp4|||0 \
 *     --file 记录.txt||||0
 *
 * 授权码用环境变量 SPARK_EMAIL_PASSWORD，或 --password。
 * --file 的五段是路径、显示名、类型、Content-ID、是否读入内存（1 表示读入）。
 */
#include "spark_email.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{

struct HeldFile
{
    std::string path;
    std::string name;
    std::string type;
    std::string cid;
    std::vector<unsigned char> bytes;
    bool in_memory;

    HeldFile()
        : in_memory(false)
    {
    }
}
;

struct Opt
{
    std::string host;
    int port;
    int tls;
    std::string user;
    std::string password;
    std::string ca;
    int insecure;
    int timeout_ms;
    std::string from;
    std::string to;
    std::string cc;
    std::string bcc;
    std::string subject;
    std::string text;
    std::string html;
    bool has_text;
    bool has_html;
    std::vector<HeldFile> files;

    Opt()
        : port(0)
        , tls(SPARK_EMAIL_TLS_AUTO)
        , insecure(0)
        , timeout_ms(0)
        , has_text(false)
        , has_html(false)
    {
    }
}
;

const char *OrNull(const std::string &s)
{
    if (s.empty())
    {
        return NULL;
    }
    return s.c_str();
}

void Usage(void)
{
    fprintf(stderr,
        "usage: demo_main --host HOST --from ADDR (--to ADDR | --cc ADDR | --bcc ADDR)\n"
        "  [--port N] [--tls auto|implicit|starttls|none] [--user ADDR]\n"
        "  [--password SECRET] [--ca FILE] [--insecure] [--timeout MS]\n"
        "  [--subject TEXT] [--text TEXT] [--html HTML]\n"
        "  [--file PATH|NAME|TYPE|CID|MEM]\n"
        "password defaults to SPARK_EMAIL_PASSWORD\n");
}

int TlsOf(const char *name)
{
    if (strcmp(name, "auto") == 0)
    {
        return SPARK_EMAIL_TLS_AUTO;
    }
    if (strcmp(name, "implicit") == 0)
    {
        return SPARK_EMAIL_TLS_IMPLICIT;
    }
    if (strcmp(name, "starttls") == 0)
    {
        return SPARK_EMAIL_TLS_STARTTLS;
    }
    if (strcmp(name, "none") == 0)
    {
        return SPARK_EMAIL_TLS_NONE;
    }
    return -1;
}

bool LoadBytes(const std::string &path, std::vector<unsigned char> *out)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == NULL)
    {
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return false;
    }
    long n = ftell(fp);
    if (n < 0 || fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return false;
    }
    out->resize((size_t)n);
    size_t got = fread(out->empty() ? NULL : &(*out)[0], 1, (size_t)n, fp);
    fclose(fp);
    return got == (size_t)n;
}

bool AddFile(Opt *opt, const char *spec)
{
    std::string fields[5];
    int index = 0;
    std::string cur;
    for (size_t i = 0; spec[i] != 0; ++i)
    {
        if (spec[i] == '|' && index < 4)
        {
            fields[index] = cur;
            cur.clear();
            index += 1;
            continue;
        }
        cur += spec[i];
    }
    fields[index] = cur;
    HeldFile held;
    held.path = fields[0];
    held.name = fields[1];
    held.type = fields[2];
    held.cid = fields[3];
    held.in_memory = fields[4] == "1";
    if (held.in_memory && !LoadBytes(held.path, &held.bytes))
    {
        fprintf(stderr, "read %s failed\n", held.path.c_str());
        return false;
    }
    opt->files.push_back(held);
    return true;
}

bool Take(int argc, char **argv, int *i, const char **val)
{
    if (*i + 1 >= argc)
    {
        return false;
    }
    *i += 1;
    *val = argv[*i];
    return true;
}

}

int main(int argc, char **argv)
{
    Opt opt;
    for (int i = 1; i < argc; ++i)
    {
        const char *key = argv[i];
        const char *val = NULL;
        if (strcmp(key, "--help") == 0)
        {
            Usage();
            return 0;
        }
        if (strcmp(key, "--insecure") == 0)
        {
            opt.insecure = 1;
            continue;
        }
        if (!Take(argc, argv, &i, &val))
        {
            Usage();
            return 1;
        }
        if (strcmp(key, "--host") == 0)
        {
            opt.host = val;
        }
        else if (strcmp(key, "--port") == 0)
        {
            opt.port = atoi(val);
        }
        else if (strcmp(key, "--tls") == 0)
        {
            opt.tls = TlsOf(val);
            if (opt.tls < 0)
            {
                Usage();
                return 1;
            }
        }
        else if (strcmp(key, "--user") == 0)
        {
            opt.user = val;
        }
        else if (strcmp(key, "--password") == 0)
        {
            opt.password = val;
        }
        else if (strcmp(key, "--ca") == 0)
        {
            opt.ca = val;
        }
        else if (strcmp(key, "--timeout") == 0)
        {
            opt.timeout_ms = atoi(val);
        }
        else if (strcmp(key, "--from") == 0)
        {
            opt.from = val;
        }
        else if (strcmp(key, "--to") == 0)
        {
            opt.to = val;
        }
        else if (strcmp(key, "--cc") == 0)
        {
            opt.cc = val;
        }
        else if (strcmp(key, "--bcc") == 0)
        {
            opt.bcc = val;
        }
        else if (strcmp(key, "--subject") == 0)
        {
            opt.subject = val;
        }
        else if (strcmp(key, "--text") == 0)
        {
            opt.text = val;
            opt.has_text = true;
        }
        else if (strcmp(key, "--html") == 0)
        {
            opt.html = val;
            opt.has_html = true;
        }
        else if (strcmp(key, "--file") == 0)
        {
            if (!AddFile(&opt, val))
            {
                return 1;
            }
        }
        else
        {
            Usage();
            return 1;
        }
    }

    if (opt.host.empty() || opt.from.empty() || (opt.to.empty() && opt.cc.empty() && opt.bcc.empty()))
    {
        Usage();
        return 1;
    }
    if (opt.password.empty())
    {
        const char *env = getenv("SPARK_EMAIL_PASSWORD");
        if (env != NULL)
        {
            opt.password = env;
        }
    }

    static unsigned char empty_byte = 0;
    std::vector<SparkEmailFile> files;
    files.reserve(opt.files.size());
    for (size_t i = 0; i < opt.files.size(); ++i)
    {
        HeldFile *held = &opt.files[i];
        SparkEmailFile file;
        memset(&file, 0, sizeof file);
        if (held->in_memory)
        {
            file.data = held->bytes.empty() ? &empty_byte : &held->bytes[0];
            file.size = held->bytes.size();
        }
        else
        {
            file.path = OrNull(held->path);
        }
        file.name = OrNull(held->name);
        file.type = OrNull(held->type);
        file.cid = OrNull(held->cid);
        files.push_back(file);
    }

    SparkEmailServer server;
    SparkEmailMessage message;
    memset(&server, 0, sizeof server);
    memset(&message, 0, sizeof message);
    server.host = opt.host.c_str();
    server.port = opt.port;
    server.tls = opt.tls;
    server.username = OrNull(opt.user);
    server.password = OrNull(opt.password);
    server.ca_file = OrNull(opt.ca);
    server.insecure = opt.insecure;
    server.timeout_ms = opt.timeout_ms;
    message.from = opt.from.c_str();
    message.to = OrNull(opt.to);
    message.cc = OrNull(opt.cc);
    message.bcc = OrNull(opt.bcc);
    message.subject = OrNull(opt.subject);
    message.text = opt.has_text ? opt.text.c_str() : NULL;
    message.html = opt.has_html ? opt.html.c_str() : NULL;
    message.files = files.empty() ? NULL : &files[0];
    message.file_count = files.size();

    char err[1024];
    SparkEmailStatus status = spark_email_send(&server, &message, err, sizeof err);
    printf("status=%d\nerr=%s\n", (int)status, err);
    if (status == SPARK_EMAIL_OK)
    {
        return 0;
    }
    if (status == SPARK_EMAIL_AGAIN)
    {
        return 2;
    }
    return 1;
}
