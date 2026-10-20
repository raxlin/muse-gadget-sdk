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

#pragma once

/*
 * Speech for a reply, fetched from a TTS API as MP3.
 *
 * Muse sends gadgets no audio, so the session speaks replies itself: it asks
 * for a message's speech here and feeds what comes back to the MP3 decoder it
 * already has (muse_chat_session.cpp, start_tts). The fetch is a blocking
 * HTTPS GET on a worker task; the session's single network task only ever
 * drains the buffer between, so nothing on it blocks.
 *
 * One fetch at a time. Long text goes out as several requests, split on
 * sentence boundaries, whose MP3s arrive back to back.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts fetching speech for `text`. False if nothing will come (no text, a
 * fetch still winding down, out of memory), and the caller shows the message
 * at reading pace instead.
 */
bool muse_tts_begin(const char *text);

/* Moves up to `max` bytes of MP3 out; 0 when none is waiting. Never blocks. */
size_t muse_tts_read(uint8_t *buf, size_t max);

/* True once every request has finished and everything fetched has been read. */
bool muse_tts_ended(void);

/* True if it ended having produced no audio at all: speak nothing, show instead. */
bool muse_tts_failed(void);

/* Drops an in-flight fetch (a cancelled turn). Returns at once; the worker
 * notices and exits on its own. */
void muse_tts_abort(void);

#ifdef __cplusplus
}
#endif
