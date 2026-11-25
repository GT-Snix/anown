// Deterministic layouts (docs/graphs.md): pure functions from structure to
// positions, computed at script time. Integer ranks and slots; the layered
// layout runs a fixed number of sweeps in index order; no floats are summed in
// any order but index order.
#include "anown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// children of each node in index order, as a CSR list
typedef struct kids { int *start, *list; } kids;

static bool make_kids(const an_layout_node *t, int n, kids *k, int *root) {
    k->start = calloc((size_t)n + 1, sizeof(int));
    k->list = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!k->start || !k->list) return false;
    *root = -1;
    for (int i = 0; i < n; i++) {
        if (t[i].parent < 0) {
            if (*root >= 0) return false; // two roots
            *root = i;
        } else if (t[i].parent >= n || t[i].parent == i) return false;
        else k->start[t[i].parent + 1]++;
    }
    if (*root < 0) return false;
    for (int i = 0; i < n; i++) k->start[i + 1] += k->start[i];
    int *fill = calloc((size_t)n, sizeof(int));
    if (!fill) return false;
    for (int i = 0; i < n; i++)
        if (t[i].parent >= 0) k->list[k->start[t[i].parent] + fill[t[i].parent]++] = i;
    free(fill);
    return true;
}

static an_vec2 spacing_of(const an_layout_opts *o) {
    return (o && o->spacing.x > 0.0f && o->spacing.y > 0.0f) ? o->spacing : (an_vec2){ 100, 100 };
}

bool an_layout_tree(const an_layout_node *t, int n, const an_layout_opts *opts, an_vec2 *out) {
    if (!t || !out || n <= 0) return false;
    const an_vec2 spacing = spacing_of(opts), origin = opts ? opts->origin : (an_vec2){ 0, 0 };
    kids k = { 0 };
    int root;
    int *stack = malloc(sizeof(int) * (size_t)n * 2), *depth = calloc((size_t)n, sizeof(int));
    double *x = calloc((size_t)n, sizeof(double));
    bool ok = stack && depth && x && make_kids(t, n, &k, &root);
    bool binary = true;
    for (int i = 0; ok && i < n; i++) binary = binary && (t[i].parent < 0 || t[i].side != 0);
    // depths, and a check that every node hangs from the root (no cycles)
    int seen = 0;
    if (ok) {
        int sp = 0;
        stack[sp++] = root;
        while (sp > 0) {
            const int v = stack[--sp];
            seen++;
            for (int c = k.start[v]; c < k.start[v + 1]; c++) {
                depth[k.list[c]] = depth[v] + 1;
                stack[sp++] = k.list[c];
            }
        }
        ok = seen == n;
    }
    if (ok && binary) {
        // left and right child of each node; then in-order ranks
        int *lr = malloc(sizeof(int) * (size_t)n * 2);
        ok = lr != NULL;
        for (int i = 0; ok && i < 2 * n; i++) lr[i] = -1;
        for (int i = 0; ok && i < n; i++) {
            if (t[i].parent < 0) continue;
            int *slot = &lr[t[i].parent * 2 + (t[i].side > 0)];
            if (*slot >= 0) ok = false; // two children on one side
            *slot = i;
        }
        int sp = 0, v = root, rank = 0;
        while (ok && (sp > 0 || v >= 0)) {
            while (v >= 0) {
                stack[sp++] = v;
                v = lr[v * 2];
            }
            v = stack[--sp];
            x[v] = rank++;
            v = lr[v * 2 + 1];
        }
        free(lr);
    } else if (ok) {
        // n-ary: leaves get slots in pre-order; a parent sits halfway between its
        // first and last child (children resolved first: post-order)
        int slot = 0, sp = 0;
        int *post = malloc(sizeof(int) * (size_t)n), np = 0;
        ok = post != NULL;
        if (ok) stack[sp++] = root;
        while (ok && sp > 0) {
            const int v = stack[--sp];
            post[np++] = v;
            if (k.start[v] == k.start[v + 1]) x[v] = slot++;
            for (int c = k.start[v + 1] - 1; c >= k.start[v]; c--) stack[sp++] = k.list[c]; // first child on top
        }
        for (int i = np - 1; ok && i >= 0; i--) { // reverse pre-order: children before parents
            const int v = post[i];
            if (k.start[v] < k.start[v + 1]) x[v] = (x[k.list[k.start[v]]] + x[k.list[k.start[v + 1] - 1]]) * 0.5;
        }
        free(post);
    }
    if (ok) {
        const double x0 = x[root];
        for (int i = 0; i < n; i++)
            out[i] = (an_vec2){ origin.x + (float)((x[i] - x0) * (double)spacing.x), origin.y + (float)depth[i] * spacing.y };
    } else {
        fprintf(stderr, "anown: an_layout_tree: not a tree (one root, parents in range, one child per side)\n");
    }
    free(stack);
    free(depth);
    free(x);
    free(k.start);
    free(k.list);
    return ok;
}

// is `to` reachable from `from` over the kept edges? (iterative DFS)
static bool reaches(int n, const int (*e)[2], const bool *keep, int m, int from, int to, int *stack, bool *mark) {
    memset(mark, 0, (size_t)n);
    int sp = 0;
    stack[sp++] = from;
    mark[from] = true;
    while (sp > 0) {
        const int v = stack[--sp];
        if (v == to) return true;
        for (int j = 0; j < m; j++)
            if (keep[j] && e[j][0] == v && !mark[e[j][1]]) {
                mark[e[j][1]] = true;
                stack[sp++] = e[j][1];
            }
    }
    return false;
}

enum { SWEEPS = 8 };

bool an_layout_layered(int n, const int (*e)[2], int m, const an_layout_opts *opts, an_vec2 *out) {
    if (n <= 0 || !out || (m > 0 && !e)) return false;
    const an_vec2 spacing = spacing_of(opts), origin = opts ? opts->origin : (an_vec2){ 0, 0 };
    for (int j = 0; j < m; j++)
        if (e[j][0] < 0 || e[j][0] >= n || e[j][1] < 0 || e[j][1] >= n) return false;
    bool *keep = calloc((size_t)(m > 0 ? m : 1), sizeof(bool)), *mark = calloc((size_t)n, sizeof(bool));
    int *stack = malloc(sizeof(int) * (size_t)n), *layer = calloc((size_t)n, sizeof(int));
    int *pos = calloc((size_t)n, sizeof(int)), *order = malloc(sizeof(int) * (size_t)n);
    double *bary = calloc((size_t)n, sizeof(double));
    bool ok = keep && mark && stack && layer && pos && order && bary;
    // keep edges in index order unless one closes a cycle (or is a self-loop)
    for (int j = 0; ok && j < m; j++)
        keep[j] = e[j][0] != e[j][1] && !reaches(n, e, keep, m, e[j][1], e[j][0], stack, mark);
    // layers: longest path from a source; n rounds of relaxation in edge order
    for (int r = 0; ok && r < n; r++) {
        bool changed = false;
        for (int j = 0; j < m; j++)
            if (keep[j] && layer[e[j][1]] < layer[e[j][0]] + 1) {
                layer[e[j][1]] = layer[e[j][0]] + 1;
                changed = true;
            }
        if (!changed) break;
    }
    int layers = 0;
    for (int i = 0; ok && i < n; i++) layers = layer[i] + 1 > layers ? layer[i] + 1 : layers;
    if (ok) { // initial order in each layer: index order (pos = rank within the layer)
        for (int L = 0; L < layers; L++) {
            int r = 0;
            for (int i = 0; i < n; i++)
                if (layer[i] == L) pos[i] = r++;
        }
    }
    // barycenter sweeps: even sweeps go down (predecessors), odd go up (successors)
    for (int sweep = 0; ok && sweep < SWEEPS; sweep++) {
        const bool down = (sweep % 2) == 0;
        for (int li = 1; li < layers; li++) {
            const int L = down ? li : layers - 1 - li;
            for (int i = 0; i < n; i++) {
                if (layer[i] != L) continue;
                double sum = 0.0;
                int cnt = 0;
                for (int j = 0; j < m; j++) { // index order
                    if (!keep[j]) continue;
                    const int nb = down ? (e[j][1] == i ? e[j][0] : -1) : (e[j][0] == i ? e[j][1] : -1);
                    if (nb < 0) continue;
                    sum += pos[nb];
                    cnt++;
                }
                bary[i] = cnt ? sum / cnt : (double)pos[i]; // no neighbors: stays put
            }
            // sort the layer by (barycenter, previous position, index): insertion sort
            int cnt = 0;
            for (int i = 0; i < n; i++)
                if (layer[i] == L) order[cnt++] = i;
            for (int a = 1; a < cnt; a++) {
                const int v = order[a];
                int b = a - 1;
                while (b >= 0) {
                    const int u = order[b];
                    const bool after = bary[u] > bary[v] || (bary[u] == bary[v] && (pos[u] > pos[v] || (pos[u] == pos[v] && u > v)));
                    if (!after) break;
                    order[b + 1] = u;
                    b--;
                }
                order[b + 1] = v;
            }
            for (int r = 0; r < cnt; r++) pos[order[r]] = r;
        }
    }
    if (ok) {
        for (int i = 0; i < n; i++) {
            int cnt = 0;
            for (int k = 0; k < n; k++) cnt += layer[k] == layer[i];
            out[i] = (an_vec2){ origin.x + ((float)pos[i] - (float)(cnt - 1) * 0.5f) * spacing.x,
                                origin.y + (float)layer[i] * spacing.y };
        }
    }
    free(keep);
    free(mark);
    free(stack);
    free(layer);
    free(pos);
    free(order);
    free(bary);
    return ok;
}
