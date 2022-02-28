#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "threadpoll.h"

#define DOWNLOAD_THREAD_COUNT_MAX 8 // 超过8个，curl return err 7
#define RANGE_SIZE 64
#define IMI_CA_FILE_PATH "/etc/ssl/certs/ca-certificates.crt"

struct task_args_t
{
    size_t id;
    CURL *curl;
    size_t offset;
    size_t length;
    size_t have_download_len;
};

static time_t time_header;
static unsigned char *file_buf = NULL;
static int download_complete_count;

static size_t get_file_size(char *url)
{
    double file_size;
    CURL *curl_header = NULL;

    curl_header = curl_easy_init();
    curl_easy_setopt(curl_header, CURLOPT_URL, url);
    curl_easy_setopt(curl_header, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl_header, CURLOPT_NOBODY, 1);

    if (curl_easy_perform(curl_header) == CURLE_OK)
    {
        curl_easy_getinfo(curl_header, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &file_size);
    }
    else
    {
        fprintf(stderr, "Cannot get file size!\n");
        return 0;
    }

    return (size_t)file_size;
}

static size_t file_buf_get_cb(void *buffer, size_t size, size_t nmemb, void *user_para)
{
    struct task_args_t *task_arg = (struct task_args_t *)user_para;

    memcpy(file_buf + task_arg->offset + task_arg->have_download_len, (unsigned char *)buffer, size * nmemb);
    task_arg->have_download_len += size * nmemb;

    return size * nmemb;
}

static void *curl_download_task(void *arg)
{
    int ret;

    if (arg == NULL)
    {
        fprintf(stderr, "curl_download_task, arg error");
    }
    struct task_args_t *task_arg = (struct task_args_t *)arg;

    ret = curl_easy_perform(task_arg->curl);
    if (ret != 0)
    {
        fprintf(stderr, "curl ret:%d Something wrong with curl\n", ret);
    }

    if (task_arg->curl)
    {
        curl_easy_cleanup(task_arg->curl);
    }

    download_complete_count += 1;

    if (task_arg)
    {
        free(task_arg);
    }

    return NULL;
}

int main(int argc, char *argv[])
{
    int ret = 0;
    int i;
    size_t all_download_size;
    size_t every_download_size;
    size_t last_download_size;
    char range[RANGE_SIZE];
    threadpoll_t *thp = NULL;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s [url] [filename]\n", argv[0]);
        return -1;
    }

    time(&time_header);
    printf("\n-----------------------------------------------\n%s--------------- start download! --------------\n\n", asctime(gmtime(&time_header)));

    all_download_size = get_file_size(argv[1]);
    if (all_download_size == 0)
    {
        return -1;
    }
    printf("file size is %lu\n", all_download_size);
    file_buf = (unsigned char *)malloc(all_download_size);

    every_download_size = all_download_size / DOWNLOAD_THREAD_COUNT_MAX;
    last_download_size = every_download_size + all_download_size % DOWNLOAD_THREAD_COUNT_MAX;
    printf("every thread download %lu, last thread download %lu\n", every_download_size, last_download_size);

    download_complete_count = 0;
    thp = threadpoll_create(3, 35, 50);
    for (i = 0; i < DOWNLOAD_THREAD_COUNT_MAX; i++)
    {
        struct task_args_t *task_arg = (struct task_args_t *)malloc(sizeof(struct task_args_t));
        task_arg->id = i;
        task_arg->curl = curl_easy_init();
        task_arg->offset = i * every_download_size;
        if (i != (DOWNLOAD_THREAD_COUNT_MAX - 1))
        {
            task_arg->length = every_download_size;
        }
        else
        {
            task_arg->length = last_download_size;
        }
        task_arg->have_download_len = 0;

        memset(range, 0, RANGE_SIZE);
        snprintf(range, RANGE_SIZE, "%lu-%lu", task_arg->offset, task_arg->offset + task_arg->length - 1);

        curl_easy_setopt(task_arg->curl, CURLOPT_URL, argv[1]); // 设置远程主机的url地址
        curl_easy_setopt(task_arg->curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(task_arg->curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(task_arg->curl, CURLOPT_CONNECTTIMEOUT, 10); // 设置连接超时时间，单位s
        curl_easy_setopt(task_arg->curl, CURLOPT_TIMEOUT, 420L);      // 设置下载超时时间，单位s
        curl_easy_setopt(task_arg->curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(task_arg->curl, CURLOPT_WRITEFUNCTION, file_buf_get_cb); // 设置回调函数来保存接收数据
        curl_easy_setopt(task_arg->curl, CURLOPT_WRITEDATA, (void *)task_arg);
        curl_easy_setopt(task_arg->curl, CURLOPT_CAINFO, IMI_CA_FILE_PATH);
        // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);							  // 打开详细信息
        curl_easy_setopt(task_arg->curl, CURLOPT_RANGE, range);

        threadpoll_add_task(thp, curl_download_task, (void *)task_arg);
    }

    while (1)
    {
        if (download_complete_count == DOWNLOAD_THREAD_COUNT_MAX)
        {
            break;
        }
        sleep(1);
    }

    time(&time_header);
    printf("\n----------------------------------------------\n%s------------- download complete! -------------\n\n", asctime(gmtime(&time_header)));

    FILE *fp = fopen(argv[2], "w");
    if(fp ==NULL){
        fprintf(stderr, "cannot open %s\n", argv[2]);
    }
    fwrite(file_buf, sizeof(unsigned char), all_download_size, fp);
    fclose(fp);

    if (file_buf)
    {
        free(file_buf);
    }

    return ret;
}