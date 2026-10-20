/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Fetches a reply's speech as MP3. See muse_tts.h for what the session does
 * with it.
 *
 * The provider is the keyless Google Translate endpoint, which needs no
 * credentials and answers with an MP3 straight away; tts_url() and tts_lang()
 * below are the whole of it, so another provider (Azure Speech, say, with its
 * key in sdkconfig) is a change to those two.
 *
 * Cancelling can't wait for a blocking GET to return, so each fetch carries the
 * epoch it was started in: muse_tts_abort() just moves the epoch on, and a
 * worker whose epoch has passed stops reading, writes nothing more and exits.
 * It suspends itself at the end rather than self-deleting (vTaskDeleteWithCaps
 * asks to be called from another task), and the next muse_tts_begin() reaps it.
 */

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "muse_text.h"
#include "muse_tts.h"

static const char *TAG = "muse_tts";

#define BUF_BYTES (32 * 1024)      /* ~4 s at the 64 kbps the endpoint returns */
#define SEG_MAX 150                /* bytes per request; the endpoint caps it near 200 characters */
#define READ_CHUNK 1024
#define URL_MAX (SEG_MAX * 3 + 128)
#define HTTP_TIMEOUT_MS 8000
#define SEND_WAIT_MS 50            /* how long a full buffer is waited on before looking at the epoch */

/* A browser's, which the endpoint wants to see. */
#define UA "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 " \
           "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

typedef struct {
    char *text;
    uint32_t epoch;
} job_t;

static StreamBufferHandle_t s_buf;
static TaskHandle_t s_task;
static volatile uint32_t s_epoch = 1;
static volatile bool s_finished;   /* the worker is done and suspended, waiting to be reaped */
static volatile bool s_done;       /* the current fetch asked for everything it was going to */
static volatile size_t s_bytes;    /* MP3 bytes handed over, this fetch */

/* ---- The provider ---- */

static const char *tts_lang(const char *text)
{
    return muse_text_has_cjk(text) ? "zh-TW" : "en";
}

/* Writes the request for one segment. False if it wouldn't fit. */
static bool tts_url(const char *text, size_t n, const char *lang, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    int head = snprintf(out, cap, "https://translate.google.com/translate_tts"
                                  "?ie=UTF-8&client=tw-ob&tl=%s&q=", lang);
    if (head < 0 || (size_t)head >= cap) {
        return false;
    }
    size_t at = head;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (at + (plain ? 1 : 3) >= cap) {
            return false;
        }
        if (plain) {
            out[at++] = (char)c;
        } else {
            out[at++] = '%';
            out[at++] = hex[c >> 4];
            out[at++] = hex[c & 15];
        }
    }
    out[at] = '\0';
    return true;
}

/* ---- Splitting the text ---- */

static size_t char_len(const char *p)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80 || c >= 0xF8) return 1;
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;   /* a stray continuation byte: step over it */
}

static bool is_break(const char *p, size_t len)
{
    static const char *const breaks[] = { ".", "!", "?", ";", ":", ",", "\n",
                                          "。", "！", "？", "；", "，", "、", "：" };
    for (size_t k = 0; k < sizeof(breaks) / sizeof(breaks[0]); k++) {
        if (strlen(breaks[k]) == len && !memcmp(p, breaks[k], len)) {
            return true;
        }
    }
    return false;
}

/*
 * How much of `p` goes in the next request: a sentence break if one is within
 * reach, else a word break, else as many whole characters as fit. Splitting
 * mid-character would send the endpoint invalid UTF-8.
 */
static size_t next_seg(const char *p)
{
    size_t n = strlen(p);
    if (n <= SEG_MAX) {
        return n;
    }
    size_t brk = 0, space = 0, i = 0;
    while (i < n) {
        size_t c = char_len(p + i);
        if (i + c > SEG_MAX) {
            break;
        }
        if (is_break(p + i, c)) {
            brk = i + c;
        } else if (p[i] == ' ') {
            space = i + 1;
        }
        i += c;
    }
    if (brk > SEG_MAX / 3) return brk;
    if (space > SEG_MAX / 3) return space;
    return i ? i : n;
}

/* ---- Fetching ---- */

/* An MP3 frame header, or an ID3 tag: anything else is an error page. */
static bool looks_like_mp3(const uint8_t *b, size_t n)
{
    if (n >= 3 && !memcmp(b, "ID3", 3)) return true;
    return n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0;
}

/* Hands `n` bytes to the reader, waiting while it's behind. False if cancelled. */
static bool put(const uint8_t *data, size_t n, uint32_t epoch)
{
    while (n) {
        if (epoch != s_epoch) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_buf, data, n, pdMS_TO_TICKS(SEND_WAIT_MS));
        data += sent;
        n -= sent;
        s_bytes += sent;
    }
    return true;
}

static bool fetch_seg(const char *text, size_t n, const char *lang, uint32_t epoch)
{
    char *url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_config_t cfg = {
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = NULL;
    esp_err_t err = ESP_OK;
    bool ok = false, first = true;
    int status = 0;

    if (!url || !chunk || !tts_url(text, n, lang, url, URL_MAX)) {
        goto out;
    }
    cfg.url = url;
    c = esp_http_client_init(&cfg);
    if (!c) {
        goto out;
    }
    esp_http_client_set_header(c, "User-Agent", UA);
    err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s (internal heap %u free, largest %u)", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        goto out;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "no response headers");
        goto out;
    }
    status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto out;
    }
    for (;;) {
        if (epoch != s_epoch) {
            goto out;   /* cancelled */
        }
        int got = esp_http_client_read(c, (char *)chunk, READ_CHUNK);
        if (got < 0) {
            ESP_LOGW(TAG, "read failed");
            goto out;
        }
        if (!got) {
            ok = esp_http_client_is_complete_data_received(c);
            if (!ok) {
                ESP_LOGW(TAG, "response cut short");
            }
            goto out;
        }
        if (first) {
            if (!looks_like_mp3(chunk, got)) {
                ESP_LOGW(TAG, "response isn't MP3");
                goto out;
            }
            first = false;
        }
        if (!put(chunk, got, epoch)) {
            goto out;
        }
    }
out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(url);
    free(chunk);
    return ok;
}

static void worker(void *arg)
{
    job_t *job = arg;
    const char *lang = tts_lang(job->text);
    const char *p = job->text;
    int segs = 0;
    while (*p && job->epoch == s_epoch) {
        size_t n = next_seg(p);
        if (!n || !fetch_seg(p, n, lang, job->epoch)) {
            break;
        }
        segs++;
        p += n;
        while (*p == ' ') {
            p++;
        }
    }
    if (job->epoch == s_epoch) {
        ESP_LOGI(TAG, "%s: %d segment(s), %u bytes of MP3", *p ? "fetch failed" : "fetched", segs,
                 (unsigned)s_bytes);
        s_done = true;
    }
    free(job->text);
    free(job);
    s_finished = true;
    vTaskSuspend(NULL);   /* the next begin() deletes it, which frees the PSRAM stack */
}

/* ---- The session's side ---- */

/* Deletes a worker that has run its course, so another can start. */
static void reap(void)
{
    if (s_task && s_finished) {
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
        s_finished = false;
    }
}

bool muse_tts_begin(const char *text)
{
    reap();
    if (s_task) {
        ESP_LOGW(TAG, "a fetch is still winding down");
        return false;
    }
    if (!text || !text[0]) {
        return false;
    }
    if (!s_buf) {
        s_buf = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            return false;
        }
    }
    xStreamBufferReset(s_buf);   /* no worker is running: nothing else touches it */
    job_t *job = calloc(1, sizeof(*job));
    char *copy = strdup(text);
    if (!job || !copy) {
        free(job);
        free(copy);
        return false;
    }
    s_bytes = 0;
    s_done = false;
    s_epoch++;
    job->text = copy;
    job->epoch = s_epoch;
    /* Stack in PSRAM, as the session task's is: TLS runs on it. */
    if (xTaskCreatePinnedToCoreWithCaps(worker, "muse_tts", 16 * 1024, job, 5, &s_task, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no room for the fetch task");
        free(copy);
        free(job);
        s_task = NULL;
        s_done = true;
        return false;
    }
    return true;
}

size_t muse_tts_read(uint8_t *buf, size_t max)
{
    if (!s_buf || !max) {
        return 0;
    }
    return xStreamBufferReceive(s_buf, buf, max, 0);
}

bool muse_tts_ended(void)
{
    return s_done && (!s_buf || !xStreamBufferBytesAvailable(s_buf));
}

bool muse_tts_failed(void)
{
    return muse_tts_ended() && !s_bytes;
}

void muse_tts_abort(void)
{
    s_epoch++;   /* the worker sees it and gives up; the next begin() reaps it */
    s_done = true;
    reap();
}
