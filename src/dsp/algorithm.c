#include "dsp.h"

static const uint8_t POW3[NUM_NODES] = {1, 3, 9, 27};

char combine_glyph(Combine c) {
    switch (c) {
    case COMBINE_SERIES:
        return 'S';
    case COMBINE_PARALLEL:
        return 'P';
    default:
        return 'F';
    }
}

Combine combine_next(Combine c) {
    switch (c) {
    case COMBINE_SERIES:
        return COMBINE_PARALLEL;
    case COMBINE_PARALLEL:
        return COMBINE_FEEDBACK;
    default:
        return COMBINE_SERIES;
    }
}

static Combine combine_from_trit(uint8_t t) {
    switch (t) {
    case 1:
        return COMBINE_PARALLEL;
    case 2:
        return COMBINE_FEEDBACK;
    default:
        return COMBINE_SERIES;
    }
}

Combine algorithm_node(AlgorithmId id, int k) {
    return combine_from_trit(id / POW3[k] % 3);
}

AlgorithmId algorithm_with_node(AlgorithmId id, int k, Combine choice) {
    uint16_t place = POW3[k];
    uint16_t now = ((uint16_t)id / place) % 3;
    return (AlgorithmId)((uint16_t)id - now * place + (uint16_t)choice * place);
}

int algorithm_distance(AlgorithmId a, AlgorithmId b) {
    int count = 0;
    for (int k = 0; k < NUM_NODES; k++) {
        if (algorithm_node(a, k) != algorithm_node(b, k)) {
            count++;
        }
    }
    return count;
}

void algorithm_to_glyphs(AlgorithmId id, char out[NUM_NODES + 1]) {
    for (int k = 0; k < NUM_NODES; k++) {
        out[k] = combine_glyph(algorithm_node(id, k));
    }
    out[NUM_NODES] = '\0';
}

bool algorithm_from_glyphs(const char *text, AlgorithmId *out) {
    size_t len = 0;
    while (text[len] != '\0') {
        len++;
    }
    if (len != NUM_NODES) {
        return false;
    }
    AlgorithmId id = 0;
    for (int k = 0; k < NUM_NODES; k++) {
        char b = text[k];
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - ('a' - 'A'));
        }
        Combine choice;
        switch (b) {
        case 'S':
            choice = COMBINE_SERIES;
            break;
        case 'P':
            choice = COMBINE_PARALLEL;
            break;
        case 'F':
            choice = COMBINE_FEEDBACK;
            break;
        default:
            return false;
        }
        id = algorithm_with_node(id, k, choice);
    }
    *out = id;
    return true;
}

AlgorithmId algorithm_from_legacy_bits(uint8_t bits) {
    AlgorithmId id = 0;
    for (int k = 0; k < NUM_NODES; k++) {
        if ((bits >> k & 1) == 1) {
            id = algorithm_with_node(id, k, COMBINE_PARALLEL);
        }
    }
    return id;
}

#define ALGO(n0, n1, n2, n3) ((AlgorithmId)((n0) + 3 * (n1) + 9 * (n2) + 27 * (n3)))

const AlgorithmId ALGORITHMS[8] = {
    ALGO(0, 0, 0, 0),
    ALGO(0, 0, 0, 1),
    ALGO(1, 0, 0, 1),
    ALGO(1, 1, 0, 1),
    ALGO(1, 1, 1, 1),
    ALGO(1, 1, 1, 2),
    ALGO(1, 1, 2, 2),
    ALGO(1, 2, 2, 2),
};

typedef struct {
    uint8_t carriers;
    int primary;
} Cluster;

static Cluster expand(uint32_t n, AlgorithmId id, int *next_op, int *next_node,
                      uint8_t modulators[NUM_OPS], uint8_t *fb_sites) {
    if (n <= 2) {
        int op = *next_op;
        *next_op += 1;
        Cluster c = {(uint8_t)(1u << op), op};
        return c;
    }
    int node = *next_node;
    *next_node += 1;
    Cluster larger = expand(n - 1, id, next_op, next_node, modulators, fb_sites);
    Cluster smaller = expand(n - 2, id, next_op, next_node, modulators, fb_sites);
    Combine choice = algorithm_node(id, node);
    if (choice == COMBINE_SERIES || choice == COMBINE_FEEDBACK) {
        modulators[smaller.primary] |= larger.carriers;
        if (choice == COMBINE_FEEDBACK) {
            *fb_sites |= (uint8_t)(1u << smaller.primary);
        }
        Cluster c = {smaller.carriers, smaller.primary};
        return c;
    }
    Cluster c = {(uint8_t)(larger.carriers | smaller.carriers), larger.primary};
    return c;
}

Compiled compile(AlgorithmId id) {
    Compiled out;
    uint8_t fb_sites = 0;
    int next_op = 0;
    int next_node = 0;
    for (int i = 0; i < NUM_OPS; i++) {
        out.modulators[i] = 0;
    }
    Cluster root = expand(5, id, &next_op, &next_node, out.modulators, &fb_sites);

    uint8_t depth[NUM_OPS] = {0};
    int stack[NUM_OPS];
    int top = 0;
    for (int op = 0; op < NUM_OPS; op++) {
        if ((root.carriers >> op & 1) == 1) {
            depth[op] = 1;
            stack[top] = op;
            top++;
        }
    }
    while (top > 0) {
        top--;
        int target = stack[top];
        for (int m = 0; m < NUM_OPS; m++) {
            if ((out.modulators[target] >> m & 1) == 1) {
                depth[m] = (uint8_t)(depth[target] + 1);
                stack[top] = m;
                top++;
            }
        }
    }

    int eval_order[NUM_OPS];
    for (int op = 0; op < NUM_OPS; op++) {
        eval_order[op] = op;
    }
    for (int i = 1; i < NUM_OPS; i++) {
        int op = eval_order[i];
        int j = i;
        while (j > 0 &&
               (depth[eval_order[j - 1]] < depth[op] ||
                (depth[eval_order[j - 1]] == depth[op] && eval_order[j - 1] > op))) {
            eval_order[j] = eval_order[j - 1];
            j--;
        }
        eval_order[j] = op;
    }

    int feedback_op = eval_order[0];
    if (fb_sites != 0) {
        int best = -1;
        for (int op = 0; op < NUM_OPS; op++) {
            if ((fb_sites >> op & 1) == 1 &&
                (best < 0 || depth[op] < depth[best] ||
                 (depth[op] == depth[best] && op < best))) {
                best = op;
            }
        }
        if (best >= 0) {
            feedback_op = best;
        }
    }

    out.carriers = root.carriers;
    for (int i = 0; i < NUM_OPS; i++) {
        out.eval_order[i] = eval_order[i];
        out.depth[i] = depth[i];
    }
    out.feedback_op = feedback_op;
    uint8_t count = 0;
    for (int op = 0; op < NUM_OPS; op++) {
        count = (uint8_t)(count + (root.carriers >> op & 1));
    }
    out.carrier_count = count;
    return out;
}
