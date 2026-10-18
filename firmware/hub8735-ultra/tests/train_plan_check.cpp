// Host check for train_plan.h:
//   g++ -std=c++11 -I../application/embedded_loudspeaker train_plan_check.cpp -o train_plan_check && ./train_plan_check
#include <assert.h>
#include <stdio.h>

#include "train_plan.h"

int main()
{
    // HRmax 190: slow < fast < slow < faster < slow, and higher zones always give faster songs.
    assert(train_stage_bpm(190, 1) == 105);
    assert(train_stage_bpm(190, 2) == 124);
    assert(train_stage_bpm(190, 4) == 162);
    assert(train_stage_bpm(190, 5) == 181);
    assert(train_stage_bpm(0, 3) == 0 && train_stage_bpm(190, 0) == 0 && train_stage_bpm(190, 6) == 0);

    const TrainPlan& a = TRAIN_PLANS[0];
    uint16_t left = 0;
    assert(train_stage_at(a, 0, &left) == 0 && left == 180);
    assert(train_stage_at(a, 179999, &left) == 0 && left == 1);
    assert(train_stage_at(a, 180000, &left) == 1 && left == 120);
    assert(train_stage_at(a, 420000, &left) == 3 && left == 60);
    assert(train_stage_at(a, 659999, &left) == 4 && left == 1);
    assert(train_stage_at(a, 660000, &left) == TRAIN_STAGE_DONE && left == 0);
    assert(train_stage_at(TRAIN_PLANS[1], 299999, nullptr) == 4);

    for (uint8_t p = 0; p < TRAIN_PLAN_COUNT; ++p)
        for (uint8_t i = 0; i < TRAIN_PLANS[p].stageCount; ++i)
            assert(TRAIN_PLANS[p].stages[i].zone >= 1 && TRAIN_PLANS[p].stages[i].zone <= 5 &&
                   TRAIN_PLANS[p].stages[i].seconds > 0);
    puts("train_plan_check: OK");
    return 0;
}
