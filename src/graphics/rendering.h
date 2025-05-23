#ifndef TSC_GRAPHICS_RENDERING
#define TSC_GRAPHICS_RENDERING

#include "../cells/grid.h"
#include "ui.h"

extern const char *currentId;
extern char currentRot;

typedef struct gridclip_t {
    tsc_cell *cells;
    int width;
    int height;
} gridclip_t;

extern gridclip_t tsc_renderingGridClipboard;
extern bool tsc_renderingIsPasting;

typedef union tsc_categorybutton {
    ui_button *cell;
    ui_button *button;
    struct {
        ui_button *category;
        union tsc_categorybutton *items;
    };
} tsc_categorybutton;

void tsc_setupRendering();
void tsc_resetRendering();
int tsc_cellMouseX();
int tsc_cellMouseY();
void tsc_drawGrid();
void tsc_handleRenderInputs();
void tsc_pasteGridClipboard();

#endif
