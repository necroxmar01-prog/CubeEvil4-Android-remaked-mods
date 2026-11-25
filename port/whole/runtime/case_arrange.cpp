// Mods: "Arrange the case" (wish of DevJunior, 25.11.2026: "an option to arrange the case automatically, so that the
// things fall into place"; the app's Mods page, off by default, bit 22 of the third aim word, runtime/aim_options.cpp).
//
// Patch 0953 (Sscrn/ss_pzzl.cpp SsPzzlMain::init) calls re4_case_arrange each time the case screen opens, before it
// lays the items out from their places: the case's items (those inside the case, not the ones on the side space) are
// taken out and put back the largest first, each at the first place from the top left that takes it, upright or
// turned a quarter (as PutInCase puts a picked-up item, puzzle.cpp), so that the free room ends up together at the
// bottom right. Nothing changes when they do not all fit back (they did before: then the old places stay).
#include "re4_runtime.h"

#include <atomic>
#include <string.h>

#include "puzzle.h"

namespace {

std::atomic<bool> gOn(false);

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

extern "C" {

void re4_case_arrange_set(int on)
{
    if (gOn.exchange(on != 0) != (on != 0)) {
        re4_log("case: arranged on opening %s", on ? "on" : "off");
    }
}

void re4_case_arrange(int size)
{
    if (!gOn.load()) {
        return;
    }
    pzlPlayer* pl = new pzlPlayer;
    if (!pl->init(size)) {
        delete pl;
        return;
    }
    pzlBoard* b = pl->boardPtr();
    Was was[64];
    int n = 0;
    for (int i = 0; i < pl->pieceMax() && n < 64; i++) {
        pzlPiece* p = pl->pieceAt(i);
        if (p->isAlive() && b->search(p)) {
            was[n].p = p;
            was[n].x = p->m_pos_x;
            was[n].y = p->m_pos_y;
            was[n].orient = p->m_orientation;
            was[n].area = areaOf(p);
            n++;
        }
    }
    // the largest first (ties: the taller, then as they were)
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
    } else {
        pl->save();
        re4_log("case: %d items put in order", n);
    }
    pl->quit();
    delete pl;
}

}  // extern "C"
