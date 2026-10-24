#pragma once

/*
 * Pure macro-engine core (Task 4a): recording buffer, NVS blob
 * (de)serializer, interruptible replay sequencer, and the macro index
 * lifecycle. No IDF / FreeRTOS dependency — everything here is
 * `static inline` and host-compilable with plain `cc`, exactly like
 * app/web_logic.h. Only host-safe headers are pulled in.
 *
 * Caveat: the `static` state (record buffer + macro index) is per-TU.
 * Include this header from exactly ONE firmware TU (macro_engine.cc, wired
 * in a later subtask); the host self-test test_macro_engine.c gets its own
 * independent copy of the same pure logic.
 */

#include <stdint.h>
#include <stddef.h>

#include "app/macro_types.h"   /* macro_step_t, macro_info_t, limits */

/* ---- NVS blob (de)serialization (explicit little-endian layout) ----
 *
 * Layout: for each of the n steps, 5 bytes:
 *   [0..3] wait_ms as uint32 little-endian
 *   [4]    key     as uint8
 * Total blob size = n * 5. Explicit endianness/layout: the in-memory struct
 * padding never reaches the wire.
 */

static inline void macro_core_write_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t macro_core_read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Returns bytes written (n * 5), or 0 on any capacity error. */
static inline size_t macro_core_serialize(const macro_step_t *steps, uint16_t n,
                                          uint8_t *out, size_t cap)
{
    const size_t need = (size_t)n * MACRO_STEP_ENCODED_BYTES;
    if (steps == NULL || out == NULL || cap < need) {
        return 0;
    }
    size_t off = 0;
    for (uint16_t i = 0; i < n; i++) {
        macro_core_write_u32le(out + off, steps[i].wait_ms);
        off += 4;
        out[off++] = (uint8_t)steps[i].key;
    }
    return off;
}

/* Returns the number of steps decoded, or 0 on capacity error. Partial
 * trailing bytes (len % 5) are ignored. */
static inline uint16_t macro_core_deserialize(const uint8_t *data, size_t len,
                                              macro_step_t *out, uint16_t max)
{
    if (data == NULL || out == NULL) {
        return 0;
    }
    uint16_t count = (uint16_t)(len / MACRO_STEP_ENCODED_BYTES);
    if (count > max) {
        return 0;
    }
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = data + (size_t)i * MACRO_STEP_ENCODED_BYTES;
        out[i].wait_ms = macro_core_read_u32le(p);
        out[i].key = (ir_key_t)p[4];
    }
    return count;
}

/* ---- recording state machine (brief Step 2: record flag + temp buffer) ---- */

static uint8_t           macro_core_recording;
static macro_step_t      macro_core_steps[MACRO_MAX_STEPS];
static uint16_t          macro_core_step_count;
static ir_key_t          macro_core_first_key;   /* IR_KEY_COUNT == "none yet" */
static uint8_t           macro_core_dirty;       /* unsaved-until-snapshot flag */

/* Begin recording: clears the temp buffer, arms the flag. Returns 0. */
static inline int macro_core_record_start(void)
{
    macro_core_recording   = 1;
    macro_core_step_count  = 0;
    macro_core_first_key   = IR_KEY_COUNT;
    macro_core_dirty       = 0;
    return 0;
}

/* Stop recording and clear the temp buffer + dirty flag. */
static inline void macro_core_record_reset(void)
{
    macro_core_recording  = 0;
    macro_core_step_count = 0;
    macro_core_first_key  = IR_KEY_COUNT;
    macro_core_dirty      = 0;
}

/* Append one step. Returns 0 on success, -1 when not recording or the
 * 100-step cap is reached. Tracks first_key on the first step. */
static inline int macro_core_record_add(ir_key_t key, uint32_t wait_ms)
{
    if (!macro_core_recording) {
        return -1;
    }
    if (macro_core_step_count >= MACRO_MAX_STEPS) {
        return -1;
    }
    if (macro_core_step_count == 0) {
        macro_core_first_key = key;
    }
    macro_core_steps[macro_core_step_count].key     = key;
    macro_core_steps[macro_core_step_count].wait_ms = wait_ms;
    macro_core_step_count++;
    macro_core_dirty = 1;
    return 0;
}

/* ---- recording accessors ---- */

static inline uint16_t macro_core_record_step_count(void)  { return macro_core_step_count; }
static inline int      macro_core_record_active(void)     { return macro_core_recording ? 1 : 0; }
static inline int      macro_core_record_dirty(void)      { return macro_core_dirty ? 1 : 0; }
static inline void     macro_core_record_clear_dirty(void){ macro_core_dirty = 0; }
static inline ir_key_t macro_core_record_first_key(void)  { return macro_core_first_key; }
static inline const macro_step_t *macro_core_record_steps(void) { return macro_core_steps; }

/* ---- replay sequencer (brief Step 3; host-testable via injected callbacks) ---- */

typedef void (*macro_send_fn)(ir_key_t key);
typedef void (*macro_sleep_fn)(uint32_t ms);

/* Replay n steps in order. For each step i:
 *   1. if *abort_flag != 0 -> return i (keys 0..i-1 sent, step i NOT sent);
 *   2. sleep_ms(wait_ms), chopped into <= 50 ms pieces, re-checking the
 *      abort flag after every piece so a long wait is promptly interruptible;
 *   3. re-check the flag, then send(key).
 * After all steps return n. Any of steps/send/sleep_ms/abort_flag NULL -> -1.
 * Non-zero *abort_flag means stop. */
static inline int macro_core_replay(const macro_step_t *steps, uint16_t n,
                                    macro_send_fn send, macro_sleep_fn sleep_ms,
                                    volatile uint8_t *abort_flag)
{
    if (steps == NULL || send == NULL || sleep_ms == NULL || abort_flag == NULL) {
        return -1;
    }
    for (uint16_t i = 0; i < n; i++) {
        if (*abort_flag != 0) {
            return (int)i;
        }
        uint32_t remaining = steps[i].wait_ms;
        while (remaining > 0) {
            uint32_t piece = (remaining > MACRO_REPLAY_SLEEP_CHUNK_MS)
                           ? MACRO_REPLAY_SLEEP_CHUNK_MS : remaining;
            sleep_ms(piece);
            remaining -= piece;
            if (*abort_flag != 0) {
                return (int)i;   /* aborted mid-wait: step i not sent */
            }
        }
        if (*abort_flag != 0) {
            return (int)i;
        }
        send(steps[i].key);
    }
    return (int)n;
}

/* ---- macro index lifecycle (brief Step 1: id auto-increment, no recycle) ----
 *
 * Small in-memory index of macro_info_t. Ids auto-increment from 0 and are
 * never recycled after a delete; when the counter reaches MACRO_MAX_ID (255)
 * the whole index is full-wiped and restarts at 0. Array slots are compacted
 * on remove. This is exactly what the NVS persistence layer needs.
 */

static macro_info_t  macro_core_index[MACRO_INDEX_MAX];
static uint16_t      macro_core_index_count;
static uint8_t       macro_core_next_id;

static inline void macro_core_index_reset(void)
{
    macro_core_index_count = 0;
    macro_core_next_id     = 0;
}

static inline void macro_core_index_set_next_id(uint8_t v)
{
    macro_core_next_id = v;
}

static inline uint8_t macro_core_index_get_next_id(void)
{
    return macro_core_next_id;
}

static inline uint16_t macro_core_index_get_count(void)
{
    return macro_core_index_count;
}

static inline const macro_info_t *macro_core_index_get_all(void)
{
    return macro_core_index;
}

/* Array position of `id`, or -1 when absent. */
static inline int macro_core_index_find(uint8_t id)
{
    for (uint16_t i = 0; i < macro_core_index_count; i++) {
        if (macro_core_index[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* Pointer to the entry with `id`, or NULL (the "not found" signal). */
static inline const macro_info_t *macro_core_index_get_by_id(uint8_t id)
{
    int pos = macro_core_index_find(id);
    return (pos < 0) ? NULL : &macro_core_index[pos];
}

/* Allocate the next id, insert a new entry. Returns 0 and writes the id to
 * *out_id on success; -1 on NULL args, index full, or (trivially) empty name. */
static inline int macro_core_index_add(const char *name, uint16_t step_count,
                                       ir_key_t first_key, uint8_t *out_id)
{
    if (name == NULL || name[0] == '\0' || out_id == NULL) {
        return -1;
    }
    if (macro_core_index_count >= MACRO_INDEX_MAX) {
        return -1;
    }
    if (macro_core_next_id >= MACRO_MAX_ID) {   /* exhausted 0..254: full-wipe */
        macro_core_index_count = 0;
        macro_core_next_id     = 0;
    }
    uint8_t id = macro_core_next_id++;
    macro_info_t *slot = &macro_core_index[macro_core_index_count++];
    slot->id         = id;
    slot->step_count = step_count;
    slot->first_key  = first_key;
    size_t i = 0;
    while (name[i] != '\0' && i + 1 < sizeof(slot->name)) {
        slot->name[i] = name[i];
        i++;
    }
    slot->name[i] = '\0';
    *out_id = id;
    return 0;
}

/* Remove the entry with `id` (array compacted; the id itself is NOT
 * recycled). Returns 0 on success, -1 when the id is absent. */
static inline int macro_core_index_remove(uint8_t id)
{
    int pos = macro_core_index_find(id);
    if (pos < 0) {
        return -1;
    }
    for (int i = pos; i + 1 < (int)macro_core_index_count; i++) {
        macro_core_index[i] = macro_core_index[i + 1];
    }
    macro_core_index_count--;
    return 0;
}
