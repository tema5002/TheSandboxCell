#include "subticks.h"
#include "grid.h"
#include "ticking.h"
#include "../utils.h"
#include "../threads/workers.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "../api/api.h"

tsc_subtick_manager_t subticks = {NULL, 0};

static tsc_updateinfo_t *subticks_updateBuffer = NULL;
static size_t subticks_updateLength = 0;

static tsc_updateinfo_t *subticks_getBuffer(size_t amount) {
    if(subticks_updateLength < amount) {
        subticks_updateBuffer = realloc(subticks_updateBuffer, sizeof(tsc_updateinfo_t) * amount);
        subticks_updateLength = amount;
        return subticks_updateBuffer;
    }

    return subticks_updateBuffer;
}

static void tsc_subtick_swap(tsc_subtick_t *a, tsc_subtick_t *b) {
    tsc_subtick_t c = *a;
    *a = *b;
    *b = c;
}

static int tsc_subtick_partition(int low, int high) {
    double pivot = subticks.subs[low].priority;
    int i = low;
    int j = high;

    while(i < j) {
        while(subticks.subs[i].priority <= pivot && i <= high - 1) {
            i++;
        }

        while(subticks.subs[j].priority > pivot && j >= low + 1) {
            j--;
        }
        if(i < j) {
            tsc_subtick_swap(subticks.subs + i, subticks.subs + j);
        }
    }

    tsc_subtick_swap(subticks.subs + low, subticks.subs + j);
    return j;
}

// Credit to zxspectrum128 for writing a version of the original sort but which uses a stack on heap.
// In theory this should support arbitrarily many subticks without stackoverflows if you have enough RAM.
static void tsc_subtick_sort(int low, int high) {
    int *stack = calloc(2 * (high - low + 1), sizeof(int));
    int top = 0;

    stack[top++] = low;
    stack[top++] = high;

    while (top > 0) {
        high = stack[--top];
        low = stack[--top];

        const int partition = tsc_subtick_partition(low, high);

        if (partition - 1 > low) {
            stack[top++] = low;
            stack[top++] = partition - 1;
        }

        if (partition + 1 < high) {
            stack[top++] = partition + 1;
            stack[top++] = high;
        }
    }
    free(stack);
}

tsc_subtick_t *tsc_subtick_add(tsc_subtick_t subtick) {
    size_t idx = subticks.subc++;
    subticks.subs = realloc(subticks.subs, sizeof(tsc_subtick_t) * subticks.subc);
    subticks.subs[idx] = subtick;

    tsc_subtick_sort(0, subticks.subc - 1);
    return tsc_subtick_find(subtick.name);
}

void tsc_subtick_addCell(tsc_subtick_t *subtick, tsc_id_t cell) {
    size_t idx = subtick->idc++;
    subtick->ids = realloc(subtick->ids, subtick->idc * sizeof(tsc_id_t));
    subtick->ids[idx] = cell;
}

// Names must be interned because I said so
tsc_subtick_t *tsc_subtick_find(const char *name) {
    name = tsc_strintern(name);
    for(size_t i = 0; i < subticks.subc; i++) {
        if(subticks.subs[i].name == name) return subticks.subs + i;
    }
    return NULL;
}

static tsc_subtick_t tsc_subtick_createNew(const char *name, double priority, char spacing, bool parallel) {
    name = tsc_strintern(tsc_padWithModID(name));
    tsc_subtick_t subtick;
    subtick.spacing = 0;
    subtick.parallel = parallel;
    subtick.spacing = spacing;
    subtick.priority = priority;
    subtick.name = name;
    subtick.ids = NULL;
    subtick.idc = 0;
    subtick.mode = TSC_SUBMODE_TICKED;
    return subtick;
}

tsc_subtick_t *tsc_subtick_addTicked(const char *name, double priority, char spacing, bool parallel) {
    tsc_subtick_t subtick = tsc_subtick_createNew(name, priority, spacing, parallel);
    subtick.mode = TSC_SUBMODE_TICKED;
    return tsc_subtick_add(subtick);
}

tsc_subtick_t *tsc_subtick_addTracked(const char *name, double priority, char spacing, bool parallel) {
    tsc_subtick_t subtick = tsc_subtick_createNew(name, priority, spacing, parallel);
    subtick.mode = TSC_SUBMODE_TRACKED;
    return tsc_subtick_add(subtick);
}

tsc_subtick_t *tsc_subtick_addNeighbour(const char *name, double priority, char spacing, bool parallel) {
    tsc_subtick_t subtick = tsc_subtick_createNew(name, priority, spacing, parallel);
    subtick.mode = TSC_SUBMODE_NEIGHBOUR;
    return tsc_subtick_add(subtick);
}

tsc_subtick_t *tsc_subtick_addCustom(const char *name, double priority, char spacing, bool parallel, tsc_subtick_custom_order *orders, size_t orderc) {
    tsc_subtick_t subtick = tsc_subtick_createNew(name, priority, spacing, parallel);
    subtick.mode = TSC_SUBMODE_CUSTOM;
    subtick.customOrder = malloc(sizeof(tsc_subtick_custom_order *) * (orderc + 1));
    subtick.customOrder[orderc] = NULL;
    for(size_t i = 0; i < orderc; i++) {
        tsc_subtick_custom_order *orderp = malloc(sizeof(tsc_subtick_custom_order));
        orderp->order = orders[i].order;
        orderp->rotc = orders[i].rotc;
        orderp->rots = malloc(sizeof(int) * orderp->rotc);
        memcpy(orderp->rots, orders[i].rots, sizeof(int) * orderp->rotc);
        subtick.customOrder[i] = orderp;
    }
    return tsc_subtick_add(subtick);
}

static void tsc_subtick_worker(void *data) {
    tsc_updateinfo_t *info = data;

    char mode = info->subtick->mode;

    if(mode == TSC_SUBMODE_TRACKED) {
        char rot = info->rot;
        if(rot == 0) {
            for(int x = currentGrid->width-1; x >= 0; x--) {
                int y = info->x;
                if(!tsc_grid_checkChunk(currentGrid, x, y)) {
                    x = tsc_grid_chunkOff(x, 0);
                    continue;
                }
                tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                if(cell == NULL) continue;
                if(tsc_cell_getRotation(cell) != 0) continue;
                #ifndef TSC_TURBO
                if(cell->updated) continue;
                #endif
                for(size_t i = 0; i < info->subtick->idc; i++) {
                    if(info->subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break; // breaks out of id loop.
                        if(table->update == NULL) break;
                        #ifndef TSC_TURBO
                        cell->updated = true;
                        #endif
                        table->update(cell, x, y, x, y, table->payload);
                        break;
                    }
                }
            }
            for(int x = 0; x < currentGrid->width; x++) {
                int y = info->x;
                if(!tsc_grid_checkChunk(currentGrid, x, y)) {
                    x = tsc_grid_chunkOff(x, +1) - 1;
                    continue;
                }
                tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                if(cell == NULL) continue;
                if(tsc_cell_getRotation(cell) != 2) continue;
                #ifndef TSC_TURBO
                if(cell->updated) continue;
                #endif
                for(size_t i = 0; i < info->subtick->idc; i++) {
                    if(info->subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break; // breaks out of id loop.
                        if(table->update == NULL) break;
                        #ifndef TSC_TURBO
                        cell->updated = true;
                        #endif
                        table->update(cell, x, y, x, y, table->payload);
                        break;
                    }
                }
            }
        }
        if(rot == 1) {
            for(int y = 0; y < currentGrid->height; y++) {
                int x = info->x;
                if(!tsc_grid_checkChunk(currentGrid, x, y)) {
                    y = tsc_grid_chunkOff(y, +1) - 1;
                    continue;
                }
                tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                if(tsc_cell_getRotation(cell) != 3) continue;
                #ifndef TSC_TURBO
                if(cell->updated) continue;
                #endif
                for(size_t i = 0; i < info->subtick->idc; i++) {
                    if(info->subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break; // breaks out of id loop.
                        if(table->update == NULL) break;
                        #ifndef TSC_TURBO
                        cell->updated = true;
                        #endif
                        table->update(cell, x, y, x, y, table->payload);
                        break;
                    }
                }
            }
            for(int y = currentGrid->height-1; y >= 0; y--) {
                int x = info->x;
                if(!tsc_grid_checkChunk(currentGrid, x, y)) {
                    y = tsc_grid_chunkOff(y, 0);
                    continue;
                }
                tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                if(tsc_cell_getRotation(cell) != 1) continue;
                #ifndef TSC_TURBO
                if(cell->updated) continue;
                #endif
                for(size_t i = 0; i < info->subtick->idc; i++) {
                    if(info->subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break; // breaks out of id loop.
                        if(table->update == NULL) break;
                        #ifndef TSC_TURBO
                        cell->updated = true;
                        #endif
                        table->update(cell, x, y, x, y, table->payload);
                        break;
                    }
                }
            }
        }
        return;
    }

    if(mode == TSC_SUBMODE_TICKED) {
        for(int y = 0; y < currentGrid->height; y++) {
            if(!tsc_grid_checkChunk(currentGrid, info->x, y)) {
                y = tsc_grid_chunkOff(y, +1) - 1;
                continue;
            }
            tsc_cell *cell = tsc_grid_get(currentGrid, info->x, y);
            for(size_t i = 0; i < info->subtick->idc; i++) {
                if(info->subtick->ids[i] == cell->id) {
                    tsc_celltable *table = tsc_cell_getTable(cell);
                    if(table == NULL) break; // breaks out of id loop.
                    if(table->update == NULL) break;
                    #ifndef TSC_TURBO
                    cell->updated = true;
                    #endif
                    table->update(cell, info->x, y, info->x, y, table->payload);
                    break;
                }
            }
        }
        return;
    }
    
    if(mode == TSC_SUBMODE_NEIGHBOUR) {
        int off[] = {
            -1, 0,
            1, 0,
            0, -1,
            0, 1
        };
        int offc = 4;
        for(int i = 0; i < offc; i++) {
            for(int x = 0; x < currentGrid->width; x++) {
                int y = info->x;
                if(!tsc_grid_checkChunk(currentGrid, x, y)) {
                    x = tsc_grid_chunkOff(x, +1) - 1;
                    continue;
                }
                int cx = x + off[i*2];
                int cy = y + off[i*2+1];
                tsc_cell *cell = tsc_grid_get(currentGrid, cx, cy);
                if(cell == NULL) continue;
                for(size_t i = 0; i < info->subtick->idc; i++) {
                    if(info->subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break; // breaks out of id loop.
                        if(table->update == NULL) break;
                        table->update(cell, cx, cy, x, y, table->payload);
                        break;
                    }
                }
            }
        }
        return;
    }
}

static void tsc_subtick_doMover(struct tsc_cell *cell, int x, int y, int _ux, int _uy, void *_) {
    tsc_grid_push(currentGrid, x, y, tsc_cell_getRotation(cell), 0, NULL);
}

static void tsc_subtick_doGen(struct tsc_cell *cell, int x, int y, int _ux, int _uy, void *_) {
    char rot = tsc_cell_getRotation(cell);
    int fx = tsc_grid_frontX(x, rot);
    int fy = tsc_grid_frontY(y, rot);
#ifndef TSC_TURBO
    tsc_cell *front = tsc_grid_get(currentGrid, fx, fy);
    if(front == NULL) return;
    if(front->id != builtin.empty && tsc_grid_checkOptimization(currentGrid, fx, fy, builtin.optimizations.gens[(size_t)rot])) {
        tsc_grid_setOptimization(currentGrid, x, y, builtin.optimizations.gens[(size_t)rot], true);
        return;
    }
#endif
    int bx = tsc_grid_shiftX(x, rot, -1);
    int by = tsc_grid_shiftY(y, rot, -1);
    tsc_cell *back = tsc_grid_get(currentGrid, bx, by);
    if(back == NULL) return;
    if(!tsc_cell_canGenerate(currentGrid, back, bx, by, cell, x, y, rot)) return;
    if(tsc_grid_push(currentGrid, fx, fy, rot, 1, back) == 0) {
        tsc_grid_setOptimization(currentGrid, x, y, builtin.optimizations.gens[(size_t)rot], true);
    }
}

static void tsc_subtick_doClockwiseRotator(struct tsc_cell *cell, int x, int y, int ux, int uy, void *_) {
    tsc_cell *toRot = tsc_grid_get(currentGrid, ux, uy);
    if(toRot == NULL) return;
    if(toRot->id == builtin.empty) return;
    tsc_cell_rotate(toRot, 1);
}

static void tsc_subtick_doCounterClockwiseRotator(struct tsc_cell *cell, int x, int y, int ux, int uy, void *_) {
    tsc_cell *toRot = tsc_grid_get(currentGrid, ux, uy);
    if(toRot == NULL) return;
    if(toRot->id == builtin.empty) return;
    tsc_cell_rotate(toRot, -1);
}

static void tsc_subtick_do(tsc_subtick_t *subtick) {
    char mode = subtick->mode;
    char parallel = subtick->parallel;
    #ifdef TSC_SINGLE_THREAD
    parallel = 0;
    #endif
    // Not worth the overhead
    if(currentGrid->width * currentGrid->height < 10000) parallel = 0;
    if(workers_isDisabled()) parallel = 0;
    char spacing = subtick->spacing;

    // If bad, blame Blendy
    char rots[] = {0, 2, 3, 1};
    char rotc = 4;

    if(mode == TSC_SUBMODE_TRACKED) {
        if(parallel) {
            tsc_updateinfo_t *buffer = subticks_getBuffer(currentGrid->width < currentGrid->height ? currentGrid->height : currentGrid->width);

            for(char space = 0; space <= spacing; space++) {
                for(char i = 0; i < 2; i++) {
                    if(i == 1) {
                        size_t j = 0;
                        for(size_t x = space; x < currentGrid->width; x += 1 + spacing) {
                            if(!tsc_grid_checkColumn(currentGrid, x)) {
                                continue;
                            }
                            buffer[j].x = x;
                            buffer[j].rot = 1;
                            buffer[j].subtick = subtick;
                            j++;
                        }

                        workers_waitForTasksFlat(&tsc_subtick_worker, buffer, sizeof(tsc_updateinfo_t), j);
                    } else {
                        size_t j = 0;
                        for(size_t y = space; y < currentGrid->height; y += 1 + spacing) {
                            if(!tsc_grid_checkRow(currentGrid, y)) {
                                continue;
                            }
                            buffer[j].x = y;
                            buffer[j].rot = 0;
                            buffer[j].subtick = subtick;
                            j++;
                        }

                        workers_waitForTasksFlat(&tsc_subtick_worker, buffer, sizeof(tsc_updateinfo_t), j);
                    }
                }
            }
            return;
        }
        for(int i = 0; i < rotc; i++) {
            char rot = rots[i];
            if(rot == 0) {
                for(int y = 0; y < currentGrid->height; y++) {
                    for(int x = currentGrid->width - 1; x >= 0; x--) {
                        tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                        #ifndef TSC_TURBO
                        if(cell->updated) continue;
                        #endif
                        if(tsc_cell_getRotation(cell) != rot) continue;
                        for(size_t i = 0; i < subtick->idc; i++) {
                            if(subtick->ids[i] == cell->id) {
                                #ifndef TSC_TURBO
                                cell->updated = true;
                                #endif
                                tsc_celltable *table = tsc_cell_getTable(cell);
                                if(table == NULL) break;
                                if(table->update == NULL) break;
                                table->update(cell, x, y, x, y, table->payload);
                            }
                        }
                    }
                }
            }
            if(rot == 1) {
                for(int x = 0; x < currentGrid->width; x++) {
                    for(int y = currentGrid->height-1; y >= 0; y--) {
                        tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                        #ifndef TSC_TURBO
                        if(cell->updated) continue;
                        #endif
                        if(tsc_cell_getRotation(cell) != rot) continue;
                        for(size_t i = 0; i < subtick->idc; i++) {
                            if(subtick->ids[i] == cell->id) {
                                #ifndef TSC_TURBO
                                cell->updated = true;
                                #endif
                                tsc_celltable *table = tsc_cell_getTable(cell);
                                if(table == NULL) break;
                                if(table->update == NULL) break;
                                table->update(cell, x, y, x, y, table->payload);
                            }
                        }
                    }
                }
            }
            if(rot == 2) {
                for(int y = 0; y < currentGrid->height; y++) {
                    for(int x = 0; x < currentGrid->width; x++) {
                        tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                        #ifndef TSC_TURBO
                        if(cell->updated) continue;
                        #endif
                        if(tsc_cell_getRotation(cell) != rot) continue;
                        for(size_t i = 0; i < subtick->idc; i++) {
                            if(subtick->ids[i] == cell->id) {
                                #ifndef TSC_TURBO
                                cell->updated = true;
                                #endif
                                tsc_celltable *table = tsc_cell_getTable(cell);
                                if(table == NULL) break;
                                if(table->update == NULL) break;
                                table->update(cell, x, y, x, y, table->payload);
                            }
                        }
                    }
                }
            }
            if(rot == 3) {
                for(int x = 0; x < currentGrid->width; x++) {
                    for(int y = 0; y < currentGrid->height; y++) {
                        tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                        #ifndef TSC_TURBO
                        if(cell->updated) continue;
                        #endif
                        if(tsc_cell_getRotation(cell) != rot) continue;
                        for(size_t i = 0; i < subtick->idc; i++) {
                            if(subtick->ids[i] == cell->id) {
                                #ifndef TSC_TURBO
                                cell->updated = true;
                                #endif
                                tsc_celltable *table = tsc_cell_getTable(cell);
                                if(table == NULL) break;
                                if(table->update == NULL) break;
                                table->update(cell, x, y, x, y, table->payload);
                            }
                        }
                    }
                }
            }
        }
    }
    
    if(mode == TSC_SUBMODE_NEIGHBOUR) {
        if(parallel) {
            tsc_updateinfo_t *buffer = subticks_getBuffer(currentGrid->width);
            for(char space = 0; space <= spacing; space++) {
                // per-row for cache friendliness
                int j = 0;
                for(size_t y = space; y < currentGrid->height; y += 1 + spacing) {
                    if(!tsc_grid_checkRow(currentGrid, y)) continue;
                    buffer[j].x = y;
                    buffer[j].subtick = subtick;
                    j++;
                }

                workers_waitForTasksFlat(&tsc_subtick_worker, buffer, sizeof(tsc_updateinfo_t), j);
            }
            return;
        }
        // Single-threaded
        for(int y = 0; y < currentGrid->height; y++) {
            for(int x = 0; x < currentGrid->width; x++) {
                int off[] = {
                    -1, 0,
                    1, 0,
                    0, -1,
                    0, 1
                };
                int offc = 4;
                for(int i = 0; i < offc; i++) {
                    int cx = x + off[i*2];
                    int cy = y + off[i*2+1];
                    tsc_cell *cell = tsc_grid_get(currentGrid, cx, cy);
                    if(cell == NULL) continue;
                    for(size_t i = 0; i < subtick->idc; i++) {
                        if(subtick->ids[i] == cell->id) {
                            tsc_celltable *table = tsc_cell_getTable(cell);
                            if(table == NULL) break;
                            if(table->update == NULL) break;
                            table->update(cell, cx, cy, x, y, table->payload);
                            break;
                        }
                    }
                }
            }
        }
    }

    if(mode == TSC_SUBMODE_TICKED) {
        if(parallel) {
            tsc_updateinfo_t *buffer = subticks_getBuffer(currentGrid->width);
            for(char space = 0; space <= spacing; space++) {
                int j = 0;
                for(size_t x = space; x < currentGrid->width; x += 1 + spacing) {
                    if(!tsc_grid_checkColumn(currentGrid, x)) continue;
                    buffer[j].x = x;
                    buffer[j].subtick = subtick;
                    j++;
                }

                workers_waitForTasksFlat(&tsc_subtick_worker, buffer, sizeof(tsc_updateinfo_t), j);
            }
            return;
        }
        // Single-threaded
        for(int x = 0; x < currentGrid->width; x++) {
            for(int y = 0; y < currentGrid->height; y++) {
                tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
                #ifndef TSC_TURBO
                if(cell->updated) continue;
                #endif
                for(size_t i = 0; i < subtick->idc; i++) {
                    if(subtick->ids[i] == cell->id) {
                        tsc_celltable *table = tsc_cell_getTable(cell);
                        if(table == NULL) break;
                        if(table->update == NULL) break;
                        #ifndef TSC_TURBO
                        cell->updated = true;
                        #endif
                        table->update(cell, x, y, x, y, table->payload);
                        break;
                    }
                }
            }
        }
    }
}

#ifndef TSC_TURBO
static void tsc_subtick_reset(void *data) {
    // Super smart optimization
    // If you don't understand, me neither
    size_t x = (size_t)data;
    size_t optSize = tsc_optSize();

    for(size_t y = 0; y < currentGrid->height; y++) {
        if(!tsc_grid_checkChunk(currentGrid, x, y)) {
            size_t i = x + y * currentGrid->width;
            y += tsc_gridChunkSize - 1;
            continue;
        }
        tsc_cell *cell = tsc_grid_get(currentGrid, x, y);
        cell->updated = false;
        cell->lx = x;
        cell->ly = y;
        char rot = tsc_cell_getRotation(cell);
        cell->rotData = rot;
        size_t i = x + y * currentGrid->width;
        memset(currentGrid->optData + i * optSize, 0, optSize);
        // TODO: cell reset method
    }
}
#endif

void tsc_subtick_run() {
#ifndef TSC_TURBO
    if(storeExtraGraphicInfo) {
        tsc_trashedCellCount = 0; // yup, yup, yup
    }
    char shouldBeParallel = 1;
    #ifdef TSC_SINGLE_THREAD
        shouldBeParallel = 0;
    #endif
    if(workers_isDisabled()) shouldBeParallel = 0;
    if(currentGrid->width * currentGrid->height < 10000) shouldBeParallel = 0;
    if(shouldBeParallel) {
        // This absolutely evil hack will call the tsc_subtick_reset with a data pointer who's address is the x.
        // I don't care about how horribly unsafe this is or how much better this could be in Rust,
        // this is the fastest way to reset the grid in parallel.
        workers_waitForTasksFlat(&tsc_subtick_reset, 0, 1, currentGrid->width);
    } else {
        for(size_t i = 0; i < currentGrid->width; i++) {
            if(!tsc_grid_checkColumn(currentGrid, i)) {
                i += tsc_gridChunkSize - 1;
                continue;
            }
            // Cast it to bullshit
            void *bullshit = (void *)i;
            tsc_subtick_reset(bullshit);
        }
    }
#endif

    for(size_t i = 0; i < subticks.subc; i++) {
        tsc_subtick_do(subticks.subs + i);
    }
}

void tsc_subtick_addCore() {
    tsc_celltable *mover = tsc_cell_newTable(builtin.mover);
    mover->update = &tsc_subtick_doMover;

    tsc_subtick_t *moverSubtick = tsc_subtick_addTracked("movers", 3.0, 0, true);
    tsc_subtick_addCell(moverSubtick, builtin.mover);

    tsc_celltable *generator = tsc_cell_newTable(builtin.generator);
    generator->update = &tsc_subtick_doGen;

    tsc_subtick_t *generatorSubtick = tsc_subtick_addTracked("generators", 1.0, 0, true);
    tsc_subtick_addCell(generatorSubtick, builtin.generator);

    tsc_celltable *rotcw = tsc_cell_newTable(builtin.rotator_cw);
    rotcw->update = &tsc_subtick_doClockwiseRotator;
    
    tsc_celltable *rotccw = tsc_cell_newTable(builtin.rotator_ccw);
    rotccw->update = &tsc_subtick_doCounterClockwiseRotator;

    tsc_subtick_t *rotatorSubtick = tsc_subtick_addNeighbour("rotators", 2.0, 0, true);
    tsc_subtick_addCell(rotatorSubtick, builtin.rotator_cw);
    tsc_subtick_addCell(rotatorSubtick, builtin.rotator_ccw);
}
