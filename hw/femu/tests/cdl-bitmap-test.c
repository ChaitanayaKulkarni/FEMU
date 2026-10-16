/* SPDX-License-Identifier: GPL-2.0-only */
/* Standalone reference-model test; also run with address/undefined sanitizers. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cdl-bitmap.h"

#define MODEL_CLASSES 16
#define MODEL_SQS 2047
#define MODEL_POLLERS 32
#define MODEL_WORDS ((MODEL_SQS + CDL_BITMAP_WORD_BITS) / CDL_BITMAP_WORD_BITS)

typedef struct SchedulerModel {
    uint8_t cls[MODEL_SQS + 1];
    bool active[MODEL_SQS + 1];
    bool pending[MODEL_SQS + 1];
    uint8_t poller[MODEL_SQS + 1];
    uint32_t cost[MODEL_SQS + 1];
    bool blocked[MODEL_POLLERS + 1];
    uint16_t rr[MODEL_CLASSES];
    uint16_t wait[MODEL_CLASSES];
    uint32_t duration[MODEL_CLASSES];
    uint64_t last[MODEL_CLASSES];
    uint16_t cached_qid[MODEL_CLASSES];
    uint64_t cached_finish[MODEL_CLASSES];
    CdlBitmap member[MODEL_CLASSES];
    CdlBitmap ready[MODEL_CLASSES];
    uint64_t member_words[MODEL_CLASSES][MODEL_WORDS];
    uint64_t ready_words[MODEL_CLASSES][MODEL_WORDS];
    unsigned int classes;
    unsigned int sqs;
} SchedulerModel;

static uint32_t random_state = 0x4221c0de;

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static uint16_t reference_candidate(const SchedulerModel *model,
                                    unsigned int cls)
{
    unsigned int start = model->rr[cls] ? model->rr[cls] : 1;

    for (unsigned int offset = 0; offset < model->sqs; offset++) {
        unsigned int qid = 1 + (start - 1 + offset) % model->sqs;

        if (model->cls[qid] == cls && model->active[qid] &&
            model->pending[qid] && !model->blocked[model->poller[qid]]) {
            return qid;
        }
    }
    return 0;
}

static uint16_t bitmap_candidate(const SchedulerModel *model,
                                 unsigned int cls)
{
    const CdlBitmap *ready = &model->ready[cls];
    unsigned int first = cdl_bitmap_next(ready, model->rr[cls]);
    unsigned int qid = first;

    if (!qid) {
        return 0;
    }
    do {
        assert(model->cls[qid] == cls && model->pending[qid]);
        if (model->active[qid] &&
            !model->blocked[model->poller[qid]]) {
            return qid;
        }
        qid = cdl_bitmap_next(ready, qid + 1);
    } while (qid != first);
    return 0;
}

static uint16_t reference_next_member(const SchedulerModel *model,
                                      unsigned int cls, unsigned int after)
{
    for (unsigned int offset = 1; offset <= model->sqs; offset++) {
        unsigned int qid = 1 + (after + offset - 1) % model->sqs;

        if (model->cls[qid] == cls && model->active[qid]) {
            return qid;
        }
    }
    return 0;
}

static uint16_t bitmap_next_member(const SchedulerModel *model,
                                   unsigned int cls, unsigned int after)
{
    const CdlBitmap *member = &model->member[cls];
    unsigned int first = cdl_bitmap_next(member, after + 1);
    unsigned int qid = first;

    if (!qid) {
        return 0;
    }
    do {
        if (model->active[qid]) {
            return qid;
        }
        qid = cdl_bitmap_next(member, qid + 1);
    } while (qid != first);
    return 0;
}

static uint64_t model_finish(const SchedulerModel *model, unsigned int cls,
                             unsigned int qid)
{
    return model->last[cls] +
           (uint64_t)model->cost[qid] * model->duration[cls];
}

static void refresh_one(SchedulerModel *model, unsigned int cls)
{
    uint16_t qid = bitmap_candidate(model, cls);

    model->cached_qid[cls] = qid;
    model->cached_finish[cls] = qid ? model_finish(model, cls, qid) : 0;
}

static void refresh_all(SchedulerModel *model)
{
    for (unsigned int cls = 1; cls < model->classes; cls++) {
        refresh_one(model, cls);
    }
}

static uint16_t choose_reference(const SchedulerModel *model,
                                 unsigned int *chosen_cls)
{
    uint16_t chosen_qid = 0, chosen_wait = 0;
    uint64_t chosen_finish = UINT64_MAX;
    bool force = false;

    *chosen_cls = 0;
    for (unsigned int cls = 1; cls < model->classes; cls++) {
        uint16_t qid = reference_candidate(model, cls);
        uint64_t finish;

        if (!qid) {
            continue;
        }
        finish = model_finish(model, cls, qid);
        if (model->wait[cls] >= 999) {
            if (!force || model->wait[cls] > chosen_wait ||
                (model->wait[cls] == chosen_wait && cls < *chosen_cls)) {
                force = true;
                chosen_wait = model->wait[cls];
                chosen_qid = qid;
                chosen_finish = finish;
                *chosen_cls = cls;
            }
        } else if (!force && (!chosen_qid || finish < chosen_finish ||
                   (finish == chosen_finish && cls < *chosen_cls))) {
            chosen_qid = qid;
            chosen_finish = finish;
            *chosen_cls = cls;
        }
    }
    return chosen_qid;
}

static uint16_t choose_cached(const SchedulerModel *model,
                              unsigned int *chosen_cls)
{
    uint16_t chosen_qid = 0, chosen_wait = 0;
    uint64_t chosen_finish = UINT64_MAX;
    bool force = false;

    *chosen_cls = 0;
    for (unsigned int cls = 1; cls < model->classes; cls++) {
        uint16_t qid = model->cached_qid[cls];
        uint64_t finish = model->cached_finish[cls];

        if (!qid) {
            continue;
        }
        if (model->wait[cls] >= 999) {
            if (!force || model->wait[cls] > chosen_wait ||
                (model->wait[cls] == chosen_wait && cls < *chosen_cls)) {
                force = true;
                chosen_wait = model->wait[cls];
                chosen_qid = qid;
                chosen_finish = finish;
                *chosen_cls = cls;
            }
        } else if (!force && (!chosen_qid || finish < chosen_finish ||
                   (finish == chosen_finish && cls < *chosen_cls))) {
            chosen_qid = qid;
            chosen_finish = finish;
            *chosen_cls = cls;
        }
    }
    return chosen_qid;
}

static void test_scheduler_model(void)
{
    SchedulerModel *model = calloc(1, sizeof(*model));

    assert(model);
    for (unsigned int trial = 0; trial < 5000; trial++) {
        memset(model, 0, sizeof(*model));
        model->classes = 2 + next_random() % (MODEL_CLASSES - 2);
        model->sqs = 1 + next_random() % MODEL_SQS;
        for (unsigned int cls = 1; cls < model->classes; cls++) {
            model->member[cls].words = model->member_words[cls];
            model->member[cls].nbits = model->sqs + 1;
            model->ready[cls].words = model->ready_words[cls];
            model->ready[cls].nbits = model->sqs + 1;
            model->rr[cls] = 1 + next_random() % model->sqs;
            model->wait[cls] = next_random() % 1010;
            model->duration[cls] = 1 + next_random() % 10000;
            model->last[cls] = next_random() % 1000000;
        }
        for (unsigned int poller = 1; poller <= MODEL_POLLERS; poller++) {
            model->blocked[poller] = next_random() % 11 == 0;
        }
        for (unsigned int qid = 1; qid <= model->sqs; qid++) {
            if (next_random() % 5 == 0) {
                continue;
            }
            unsigned int cls = 1 + next_random() % (model->classes - 1);

            model->cls[qid] = cls;
            model->active[qid] = next_random() % 10 != 0;
            model->pending[qid] = next_random() % 4 != 0;
            model->poller[qid] = 1 + next_random() % MODEL_POLLERS;
            model->cost[qid] = 1 + next_random() % 100000;
            cdl_bitmap_set(&model->member[cls], qid);
            if (model->pending[qid]) {
                cdl_bitmap_set(&model->ready[cls], qid);
            }
        }
        for (unsigned int cls = 1; cls < model->classes; cls++) {
            assert(reference_candidate(model, cls) ==
                   bitmap_candidate(model, cls));
            for (unsigned int i = 0; i < 3; i++) {
                unsigned int after = next_random() % (model->sqs + 1);

                assert(reference_next_member(model, cls, after) ==
                       bitmap_next_member(model, cls, after));
            }
        }
        refresh_all(model);
        for (unsigned int step = 0; step < 64; step++) {
            unsigned int reference_cls, cached_cls;
            uint16_t reference_qid = choose_reference(model, &reference_cls);
            uint16_t cached_qid = choose_cached(model, &cached_cls);

            assert(reference_qid == cached_qid);
            assert(reference_cls == cached_cls);
            if (!reference_qid) {
                break;
            }
            if (next_random() % 5 == 0) {
                /* A failed destination invalidates every cached class. */
                model->blocked[model->poller[reference_qid]] = true;
                refresh_all(model);
                continue;
            }

            model->pending[reference_qid] = false;
            cdl_bitmap_clear(&model->ready[reference_cls], reference_qid);
            model->last[reference_cls] =
                model_finish(model, reference_cls, reference_qid);
            model->rr[reference_cls] = bitmap_next_member(
                model, reference_cls, reference_qid);
            refresh_one(model, reference_cls);
            for (unsigned int cls = 1; cls < model->classes; cls++) {
                if (model->cached_qid[cls]) {
                    if (cls == reference_cls) {
                        model->wait[cls] = 0;
                    } else if (model->wait[cls] != UINT16_MAX) {
                        model->wait[cls]++;
                    }
                }
            }
        }
    }
    free(model);
}

static unsigned int reference_find(const bool *present, unsigned int max,
                                    unsigned int start)
{
    for (unsigned int bit = start; bit <= max; bit++) {
        if (present[bit]) {
            return bit;
        }
    }
    return 0;
}

static void check(const CdlBitmap *map, const bool *present, unsigned int max,
                  unsigned int start)
{
    unsigned int expected = reference_find(present, max, start);

    assert(cdl_bitmap_find(map, start) == expected);
    assert(cdl_bitmap_next(map, start) ==
           (expected ? expected : reference_find(present, max, 1)));
}

static void test_size(unsigned int max)
{
    bool present[CDL_BITMAP_CAPACITY] = { false };
    CdlBitmap map = {
        .words = calloc(max / CDL_BITMAP_WORD_BITS + 1, sizeof(uint64_t)),
        .nbits = max + 1,
    };
    uint32_t random = 0x6d2b79f5;

    assert(map.words);
    /* Every possible singleton checks both summary and leaf boundaries,
     * wrapping, empty lookup, and exact-sized leaf allocation.
     */
    for (unsigned int bit = 1; bit <= max; bit++) {
        cdl_bitmap_set(&map, bit);
        present[bit] = true;
        check(&map, present, max, 1);
        check(&map, present, max, bit);
        check(&map, present, max, bit + 1);
        check(&map, present, max, map.nbits);
        cdl_bitmap_clear(&map, bit);
        present[bit] = false;
        assert(!map.summary);
    }
    /* Compare independent sorted-set behavior across churn and sparse/dense
     * membership. The production scheduler uses the same maps for RR and
     * ready-class enumeration, so exercise both wrapped and nonwrapped lookup.
     */
    for (unsigned int step = 0; step < 10000; step++) {
        unsigned int bit;

        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        bit = 1 + random % max;
        present[bit] = !present[bit];
        if (present[bit]) {
            cdl_bitmap_set(&map, bit);
        } else {
            cdl_bitmap_clear(&map, bit);
        }
        check(&map, present, max, random % (map.nbits + 1));
        check(&map, present, max, bit);
        check(&map, present, max, bit + 1);
        if (!(step % 100)) {
            for (unsigned int start = 1; start <= map.nbits; start++) {
                check(&map, present, max, start);
            }
        }
    }
    for (unsigned int bit = 1; bit <= max; bit++) {
        cdl_bitmap_clear(&map, bit);
    }
    assert(!map.summary);
    free(map.words);
}

int main(void)
{
    static const unsigned int maxima[] = { 1, 9, 63, 64, 65, 127, 128,
                                          255, 1024, 2047, 4095 };

    for (unsigned int i = 0; i < sizeof(maxima) / sizeof(maxima[0]); i++) {
        test_size(maxima[i]);
    }
    test_scheduler_model();
    puts("CDL bitmap and randomized scheduler-model checks passed");
    return 0;
}
