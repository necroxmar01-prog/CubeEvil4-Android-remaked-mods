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

// 1 = something moved
int re4_case_arrange(pzlPlayer* pl)
{
    if (pl == 0 || pl->pieceInHand()) {
        return 0;
    }
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
        return 0;
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
