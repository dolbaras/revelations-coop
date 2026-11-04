/*
 * lobby1_input.c - raw keyboard / gamepad state -> lobby events (pure C, tested offline). Auto repeat for the direction inputs, edge only for the rest.
 * Gamepad: both physical pads can drive the screen (D-pad or left stick, A confirms, B goes back); BACK (XInput 0x20) opens it from the title menu, so does the key F6.
 */
#include <string.h>

#include "lobby1.h"

enum { I_UP, I_DOWN, I_LEFT, I_RIGHT, I_ENTER, I_SPACE, I_ESC, I_F6, I_PUP, I_PDN, I_PLF, I_PRT, I_PA, I_PB, I_PBACK, I_COUNT };

void l1_input_reset(l1_input *in) { memset(in, 0, sizeof *in); }

static int edge_or_repeat(l1_input *in, int idx, int down, unsigned now, int repeat)
{
    int fire = 0;
    if (down && !in->prev[idx]) {
        fire = 1;
        in->next[idx] = now + 420;
    } else if (down && repeat && (int)(now - in->next[idx]) >= 0) {
        fire = 1;
        in->next[idx] = now + 90;
    }
    in->prev[idx] = (unsigned char)down;
    return fire;
}

int l1_input_poll(l1_input *in, const l1_raw *raw, unsigned now, int open, l1_event *ev, int max, int *want_open)
{
    int n = 0;
    if (want_open) *want_open = 0;
    int down[I_COUNT];
    unsigned short b0 = raw->pad_buttons[0], b1 = raw->pad_buttons[1];
    unsigned short b = (unsigned short)(b0 | b1);
    int ly = (raw->ly[0] > raw->ly[1] ? raw->ly[0] : raw->ly[1]), lyn = (raw->ly[0] < raw->ly[1] ? raw->ly[0] : raw->ly[1]);
    int lxp = (raw->lx[0] > raw->lx[1] ? raw->lx[0] : raw->lx[1]), lxn = (raw->lx[0] < raw->lx[1] ? raw->lx[0] : raw->lx[1]);
    down[I_UP] = raw->up, down[I_DOWN] = raw->down, down[I_LEFT] = raw->left, down[I_RIGHT] = raw->right;
    down[I_ENTER] = raw->enter, down[I_SPACE] = raw->space, down[I_ESC] = raw->esc, down[I_F6] = raw->f6;
    down[I_PUP] = (b & 0x0001) || ly > 22000;
    down[I_PDN] = (b & 0x0002) || lyn < -22000;
    down[I_PLF] = (b & 0x0004) || lxn < -22000;
    down[I_PRT] = (b & 0x0008) || lxp > 22000;
    down[I_PA] = (b & 0x1000) != 0;
    down[I_PB] = (b & 0x2000) != 0;
    down[I_PBACK] = (b & 0x0020) != 0;
    if (!in->init) {   /* the first poll after a reset: whatever is held now (the opening key) does not count until it was released once */
        in->init = 1;
        in->armed = 0;
    }
    if (!in->armed) {
        int any = 0;
        for (int i = 0; i < I_COUNT; i++) any |= down[i];
        if (!any) in->armed = 1;
        for (int i = 0; i < I_COUNT; i++) in->prev[i] = (unsigned char)down[i];
        return 0;
    }
#define PUSH(k) do { if (n < max) ev[n].kind = (k), ev[n].row = 0, n++; } while (0)
    int f6 = edge_or_repeat(in, I_F6, down[I_F6], now, 0);
    int pback = edge_or_repeat(in, I_PBACK, down[I_PBACK], now, 0);
    if (!open) {
        /* the other inputs only keep their previous state (so a press that starts while closed is not a new press when the screen opens) */
        for (int i = 0; i < I_COUNT; i++)
            if (i != I_F6 && i != I_PBACK) in->prev[i] = (unsigned char)down[i];
        if ((f6 || pback) && want_open) *want_open = 1;
        return 0;
    }
    if (edge_or_repeat(in, I_UP, down[I_UP], now, 1)) PUSH(L1E_UP);
    if (edge_or_repeat(in, I_PUP, down[I_PUP], now, 1)) PUSH(L1E_UP);
    if (edge_or_repeat(in, I_DOWN, down[I_DOWN], now, 1)) PUSH(L1E_DOWN);
    if (edge_or_repeat(in, I_PDN, down[I_PDN], now, 1)) PUSH(L1E_DOWN);
    if (edge_or_repeat(in, I_LEFT, down[I_LEFT], now, 1)) PUSH(L1E_LEFT);
    if (edge_or_repeat(in, I_PLF, down[I_PLF], now, 1)) PUSH(L1E_LEFT);
    if (edge_or_repeat(in, I_RIGHT, down[I_RIGHT], now, 1)) PUSH(L1E_RIGHT);
    if (edge_or_repeat(in, I_PRT, down[I_PRT], now, 1)) PUSH(L1E_RIGHT);
    if (edge_or_repeat(in, I_ENTER, down[I_ENTER], now, 0)) PUSH(L1E_OK);
    if (edge_or_repeat(in, I_SPACE, down[I_SPACE], now, 0)) PUSH(L1E_OK);
    if (edge_or_repeat(in, I_PA, down[I_PA], now, 0)) PUSH(L1E_OK);
    if (edge_or_repeat(in, I_ESC, down[I_ESC], now, 0)) PUSH(L1E_BACK);
    if (edge_or_repeat(in, I_PB, down[I_PB], now, 0)) PUSH(L1E_BACK);
    if (f6) PUSH(L1E_BACK);
    if (pback) PUSH(L1E_BACK);
#undef PUSH
    return n;
}
