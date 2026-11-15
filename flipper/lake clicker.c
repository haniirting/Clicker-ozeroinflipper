#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <bt/bt_service/bt.h>
#include <profiles/serial_profile.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HZ   22
#define MAXS 3
#define MAXR 10

typedef struct { bool on; float x, y, h, vh, sp; } Stone;
typedef struct { bool on; float x, y, t, s; } Rip;

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* q;
    FuriMutex* mu;
    Bt* bt;
    FuriHalBleProfileBase* ble;
    volatile bool conn;
    volatile uint32_t world;
    uint32_t clicks, pending;
    Stone st[MAXS];
    Rip rp[MAXR];
    char rx[24];
    uint8_t rxn;
} App;

static float sc(float y) { return 0.3f + 0.7f * (y - HZ) / (64 - HZ); }

static void add_rip(App* a, float x, float y, float s) {
    for(int i = 0; i < MAXR; i++) {
        if(!a->rp[i].on) { a->rp[i] = (Rip){true, x, y, 0, s}; return; }
    }
}

static void launch(App* a) {
    for(int i = 0; i < MAXS; i++) {
        if(!a->st[i].on) {
            Stone* s = &a->st[i];
            s->on = true; s->x = 20 + furi_hal_random_get() % 88; s->y = 60;
            s->h = 0; s->vh = 70; s->sp = 45;
            add_rip(a, s->x, s->y, sc(s->y));
            return;
        }
    }
}

static void step(App* a, float dt) {
    for(int i = 0; i < MAXS; i++) {
        Stone* s = &a->st[i];
        if(!s->on) continue;
        s->y -= s->sp * sc(s->y) * dt;
        s->vh -= 200 * dt;
        s->h += s->vh * dt;
        if(s->h <= 0) {
            s->h = 0;
            add_rip(a, s->x, s->y, sc(s->y));
            float nv = -s->vh * 0.7f;
            s->sp *= 0.82f;
            if(nv < 22 || s->y < HZ + 3) s->on = false; else s->vh = nv;
        }
    }
    for(int i = 0; i < MAXR; i++) {
        if(!a->rp[i].on) continue;
        a->rp[i].t += dt;
        if(a->rp[i].t > 1.0f) a->rp[i].on = false;
    }
}

static void draw_cb(Canvas* c, void* ctx) {
    App* a = ctx;
    furi_mutex_acquire(a->mu, FuriWaitForever);
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    char b[40];
    snprintf(b, sizeof(b), "World %lu", (unsigned long)a->world);
    canvas_draw_str(c, 0, 8, b);
    snprintf(b, sizeof(b), "You %lu  BT %s", (unsigned long)a->clicks, a->conn ? "ok" : "--");
    canvas_draw_str(c, 0, 18, b);
    canvas_draw_line(c, 0, HZ, 127, HZ);
    for(int i = 0; i < MAXR; i++) {
        Rip* r = &a->rp[i];
        if(!r->on) continue;
        float rad = (2 + r->t * 14) * r->s;
        for(int k = 0; k < 12; k++) {
            float ang = k * 0.5236f;
            canvas_draw_dot(c, (int)(r->x + cosf(ang) * rad), (int)(r->y + sinf(ang) * rad * 0.35f));
        }
    }
    for(int i = 0; i < MAXS; i++) {
        Stone* s = &a->st[i];
        if(!s->on) continue;
        canvas_draw_box(c, (int)s->x - 1, (int)(s->y - s->h * sc(s->y)) - 1, 3, 2);
    }
    canvas_draw_str(c, 86, 62, "OK = throw");
    furi_mutex_release(a->mu);
}

static void input_cb(InputEvent* e, void* ctx) {
    App* a = ctx;
    furi_message_queue_put(a->q, e, 0);
}

static void bt_status_cb(BtStatus s, void* ctx) {
    App* a = ctx;
    a->conn = (s == BtStatusConnected);
}

static uint16_t serial_cb(SerialServiceEvent ev, void* ctx) {
    App* a = ctx;
    if(ev.event == SerialServiceEventTypeDataReceived) {
        for(uint16_t i = 0; i < ev.data.size; i++) {
            char ch = (char)ev.data.buffer[i];
            if(ch == '\n') {
                a->rx[a->rxn] = 0;
                if(a->rx[0] == 'S') a->world = strtoul(a->rx + 1, NULL, 10);
                a->rxn = 0;
            } else if(a->rxn < sizeof(a->rx) - 1) {
                a->rx[a->rxn++] = ch;
            }
        }
    }
    return 128;
}

int32_t lake_clicker_app(void* p) {
    UNUSED(p);
    App* a = malloc(sizeof(App));
    memset(a, 0, sizeof(App));
    a->q = furi_message_queue_alloc(8, sizeof(InputEvent));
    a->mu = furi_mutex_alloc(FuriMutexTypeNormal);

    a->bt = furi_record_open(RECORD_BT);
    bt_disconnect(a->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_storage_path(a->bt, APP_DATA_PATH(".bt_serial.keys"));
    a->ble = bt_profile_start(a->bt, ble_profile_serial, NULL);
    furi_check(a->ble);
    ble_profile_serial_set_event_callback(a->ble, 128, serial_cb, a);
    bt_set_status_changed_callback(a->bt, bt_status_cb, a);
    furi_hal_bt_start_advertising();

    a->vp = view_port_alloc();
    view_port_draw_callback_set(a->vp, draw_cb, a);
    view_port_input_callback_set(a->vp, input_cb, a);
    a->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(a->gui, a->vp, GuiLayerFullscreen);

    uint32_t last = furi_get_tick(), last_tx = last;
    bool run = true;
    while(run) {
        InputEvent ev;
        if(furi_message_queue_get(a->q, &ev, 30) == FuriStatusOk) {
            if(ev.type == InputTypeShort || ev.type == InputTypeRepeat) {
                if(ev.key == InputKeyBack) run = false;
                else if(ev.key == InputKeyOk) {
                    furi_mutex_acquire(a->mu, FuriWaitForever);
                    launch(a);
                    a->clicks++; a->pending++;
                    furi_mutex_release(a->mu);
                }
            }
        }
        uint32_t now = furi_get_tick();
        furi_mutex_acquire(a->mu, FuriWaitForever);
        step(a, (now - last) / 1000.0f);
        furi_mutex_release(a->mu);
        last = now;
        if(a->conn && a->pending > 0 && now - last_tx > 200) {
            char b[16];
            int n = snprintf(b, sizeof(b), "+%lu\n", (unsigned long)a->pending);
            ble_profile_serial_tx(a->ble, (uint8_t*)b, n);
            a->pending = 0; last_tx = now;
        }
        view_port_update(a->vp);
    }

    gui_remove_view_port(a->gui, a->vp);
    furi_record_close(RECORD_GUI);
    view_port_free(a->vp);
    ble_profile_serial_set_event_callback(a->ble, 0, NULL, NULL);
    bt_set_status_changed_callback(a->bt, NULL, NULL);
    bt_disconnect(a->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(a->bt);
    bt_profile_restore_default(a->bt);
    furi_record_close(RECORD_BT);
    furi_mutex_free(a->mu);
    furi_message_queue_free(a->q);
    free(a);
    return 0;
}
