// Mods: "Arrange the case" (wish of DevJunior, 25.11.2026: "an option to arrange the case automatically, so that the
// things fall into place"; "when the user wants, with an option"; "it has to be an option inside the inventory").
//
// Patch 0953 (Sscrn/ss_pzzl.cpp PieceSelect::move): on the case screen, with the cursor free (no item in hand), Z
// (R1 on the Retroid) calls re4_case_arrange on the live pzlPlayer: the case's items are taken out and put back the
// largest first, each at the first place from the top left that takes it, upright or turned a quarter (as PutInCase
// puts a picked-up item, puzzle.cpp), so that the free room ends up together at the bottom right; then the items left
// on the side space go into the case where they fit. Nothing changes when the case's own items do not all fit back
// (then the old places stay). The models follow by themselves: pieceModelDisp places every piece each frame.
#include "re4_runtime.h"

#include <string.h>

#include "puzzle.h"
#include "item.h"
#include "mes.h"
#include "trans_ot.h"

#include <atomic>
#include <stdio.h>

extern MessageFont MesFont[4];  // src/game/mes.cpp
extern MessageControl cMes;
void messageTrans(MSG_QUE* q);  // src/game/mes.cpp: one glyph (the OT's callback)

namespace {

struct Was {
    pzlPiece* p;
    f32 x, y;
    s8 orient;
    int area;
};

int areaOf(pzlPiece* p)
{
    const int sx = p->size_x(), sy = p->size_y();
    int n = 0;
    for (int i = 0; i != sx; sx > 0 ? i++ : i--) {
        for (int j = 0; j != sy; sy > 0 ? j++ : j--) {
            n += p->shape((s8) i, (s8) j) ? 1 : 0;
        }
    }
    return n;
}

// the first place from the top left (row by row) that takes p, in orientation 0 or 1 (the pieces are bars and
// blocks: 2 and 3 give the same places); 1 = put
int putFirst(pzlBoard* b, pzlPiece* p)
{
    const int h = b->size_y(), w = b->size_x();
    for (int o = 0; o < 2; o++) {
        p->orientation(o);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                p->set_ver0_x((f32) x);
                p->set_ver0_y((f32) y);
                if (b->putPiece(p)) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

}  // namespace

extern "C" int re4_stack_items(void);  // runtime/cheats.cpp (patch 0254)

// (DevJunior, 26.11.2026: "if the user has the stack option on, the stacks combine by themselves with that move") with
// the cheat "Stack items" on, the ammo of one kind (item type 2, as the cheat stacks only ammo) is put together first,
// in the case and on the side space: the game's own combine (cItemMgr::combine, as dropping one box on another does),
// up to the slot's most (itemMaxNum: the cheat's value); emptied boxes leave the case (rehash). The item being picked up
// (the extra piece) stays apart. Returns how many boxes were emptied.
static int mergeAmmo(pzlPlayer* pl)
{
    if (re4_stack_items() <= 0) {
        return 0;
    }
    int emptied = 0;
    for (int i = 0; i < pl->pieceMax(); i++) {
        pzlPiece* p = pl->pieceAt(i);
        if (p->isEmpty() || !p->isAlive() || p == pl->pieceExtra() || !p->item || p->item->isEmpty() ||
            itemType(p->item->id) != 2) {
            continue;
        }
        for (int j = i + 1; j < pl->pieceMax(); j++) {
            pzlPiece* q = pl->pieceAt(j);
            if (q->isEmpty() || !q->isAlive() || q == pl->pieceExtra() || !q->item || q->item->isEmpty() ||
                q->item->id != p->item->id) {
                continue;
            }
            if (!ItemMgr.combine(p->item, q->item, 0)) {
                break;  // p is full
            }
            if (q->item->isEmpty()) {
                emptied++;
            }
        }
    }
    if (emptied) {
        pl->rehash();
        re4_log("case: stacks put together, %d boxes emptied", emptied);
    }
    return emptied;
}

// 1 = something moved
int re4_case_arrange(pzlPlayer* pl)
{
    if (pl == 0 || pl->pieceInHand()) {
        return 0;
    }
    const int merged = mergeAmmo(pl);
    pzlBoard* b = pl->boardPtr();
    pzlBoard* space = pl->spacePtr();
    Was was[64];
    Was side[64];
    int n = 0, ns = 0;
    for (int i = 0; i < pl->pieceMax(); i++) {
        pzlPiece* p = pl->pieceAt(i);
        if (p->isEmpty() || !p->isAlive()) {
            continue;
        }
        Was* w = 0;
        if (b->search(p) && n < 64) {
            w = &was[n++];
        } else if (space->search(p) && ns < 64) {
            w = &side[ns++];
        } else {
            continue;
        }
        w->p = p;
        w->x = p->m_pos_x;
        w->y = p->m_pos_y;
        w->orient = p->m_orientation;
        w->area = areaOf(p);
    }
    // the largest first (ties: as they were)
    for (int i = 1; i < n; i++) {
        Was w = was[i];
        int j = i - 1;
        while (j >= 0 && was[j].area < w.area) {
            was[j + 1] = was[j];
            j--;
        }
        was[j + 1] = w;
    }
    for (int i = 0; i < n; i++) {
        b->rmPiece(was[i].p);
    }
    int put = 0;
    while (put < n && putFirst(b, was[put].p)) {
        put++;
    }
    if (put < n) {
        // not all back: the old places
        for (int i = 0; i < put; i++) {
            b->rmPiece(was[i].p);
        }
        for (int i = 0; i < n; i++) {
            was[i].p->orientation(was[i].orient);
            was[i].p->m_pos_x = was[i].x;
            was[i].p->m_pos_y = was[i].y;
            b->putPiece(was[i].p);
        }
        re4_log("case: %d items, %d did not fit back in order: left as they were", n, n - put);
        return merged ? 1 : 0;
    }
    // the side space's items into the free room, where they fit
    int in = 0;
    for (int i = 0; i < ns; i++) {
        space->rmPiece(side[i].p);
        if (putFirst(b, side[i].p)) {
            in++;
        } else {
            side[i].p->orientation(side[i].orient);
            side[i].p->m_pos_x = side[i].x;
            side[i].p->m_pos_y = side[i].y;
            space->putPiece(side[i].p);
        }
    }
    pl->rehash();
    re4_log("case: %d items put in order, %d of %d from the side space put in", n, in, ns);
    return 1;
}

// The hint on the case screen (DevJunior, 26.11.2026: "the auto-arrange option is missing from the inventory's HUD, so
// that the user knows which button to press"): "<button>: Auto-arrange" at the bottom right while the cursor is free
// (patch 0953, SsPzzlMain::move with cur the PieceSelect), in the game's message font (MesFont[0], Font/common_p.fnt:
// glyph 3 + d the digit d, 13 ':', 17 '-', 39 + k 'A' + k, 65 + k 'a' + k), each glyph one MSG_QUE drawn through the
// OT as the game's messages are (Message::trans). The button: what the app's controller layout puts on the map action
// (the GameCube Z), sent in bits 22..29 of the third aim word (re4_case_key_set; 0 unknown: "Z").
namespace {
std::atomic<int> gCaseKey(0);
const char* const kKeyNames[] = {"Z", "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "L3", "R3", "START", "SELECT"};
MSG_QUE gHint[2][48];  // two frames' glyphs (the OT draws the last frame's while this one is filled)
int gHintBuf;

int glyphOf(char c)
{
    if (c >= 'A' && c <= 'Z') return 39 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 65 + (c - 'a');
    if (c >= '0' && c <= '9') return 3 + (c - '0');
    if (c == ':') return 13;
    if (c == '-') return 17;
    return -1;  // a space
}
}  // namespace

extern "C" void re4_case_key_set(int code)
{
    const int k = code >= 0 && code < (int) (sizeof kKeyNames / sizeof kKeyNames[0]) ? code : 0;
    if (gCaseKey.exchange(k) != k) {
        re4_log("case: the arrange button shown as %s", kKeyNames[k]);
    }
}

void re4_case_arrange_hint()
{
    MessageFont& f = MesFont[0];
    if (!(f.be_flag & 1) || f.m_char_w != 32 || f.m_char_h != 32 || !f.pWidth) {
        return;  // not the Latin font (Japanese: common_j)
    }
    char text[40];
    snprintf(text, sizeof text, "%s: Auto-arrange", kKeyNames[gCaseKey.load()]);
    // the size of the case's item names (ss_pzzl.cpp pzzl_font_w / _h: 18 x 24 for a 32-texel cell)
    const f32 sx = 18.0f / 32.0f;
    const int h = 24, space = 8;
    int width = 0;
    for (const char* c = text; *c; c++) {
        const int g = glyphOf(*c);
        width += g < 0 ? space : (int) ((f.pWidth[g * 2 + 1] - (s8) f.pWidth[g * 2]) * sx + 0.5f);
    }
    int x = 500 - width;
    const int y = 384 - 20 - h;
    MSG_QUE* q = gHint[gHintBuf];
    gHintBuf ^= 1;
    const Message& slot = *(const Message*) cMes.getWork();
    int n = 0;
    for (const char* c = text; *c && n < 48; c++) {
        const int g = glyphOf(*c);
        if (g < 0) {
            x += space;
            continue;
        }
        const int w = (int) (((s8) f.pWidth[g * 2 + 1] - (s8) f.pWidth[g * 2]) * sx + 0.5f);
        MSG_QUE& m = q[n++];
        m.px = (s16) x;
        m.py = (s16) y;
        m.mes = (u16) g;
        m.width = (u8) w;
        m.height = (u8) h;
        m.col = 0xFFFFFFFF;
        m.m_p_font = &f;
        AddOtDirect(slot.m_ot_type, &m, (void (*)()) messageTrans, slot.m_ot_no, 0x1000, NULL, 0.0f);
        x += w;
    }
}
