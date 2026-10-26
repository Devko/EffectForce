// A qemu-user TCG plugin for make bench-count: the exact number of ARM instructions each measured
// processReplacing call executes (docs/PERFORMANCE.md "Instruction counts"). Every translated block
// adds its instruction count to one counter as it runs (inline, no callback). The bench (efbench -m)
// marks events with syscall(4000, op, arg), which no kernel has; the plugin sees it on its way in:
//   op 0: a block starts   op 1: it ends (the instructions since op 0 are its count)
//   op 2: the case ends (arg: its number): prints its blocks' average, p99 and max, and starts over
//   op 3, 4: a profile window opens, closes
// -plugin libefcount.so,prof=<file>: at exit, "vaddr insns execs" (hex, decimal, decimal) for every
// translated block that ran inside the profile windows; the bench's log maps the addresses.
// The qemu 8.2 plugin API (QEMU_PLUGIN_VERSION 1): one counter for every vCPU, right for the bench,
// which runs on one thread. qemu 9 counts inline per vCPU instead.
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

// One translated block, for the profile. A block translated again at the same address with another
// length is another record: the key is both (a guest address has 32 bits, a block at most 512 insns).
typedef struct {
    uint64_t key;
    uint64_t vaddr;
    uint64_t n;
    uint64_t execs;   // the inline counter: every run of the block
    uint64_t base;    // execs when the window opened
    uint64_t acc;     // runs inside the windows
} Rec;

static uint64_t total;   // instructions so far
static uint64_t start;   // total at op 0
static GArray* deltas;   // this case's blocks
static GHashTable* recs;
static GMutex lock;
static const char* prof_path;

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
    (void)id;
    const uint64_t n = qemu_plugin_tb_n_insns(tb);
    const uint64_t va = qemu_plugin_tb_vaddr(tb);
    qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &total, n);
    if (!prof_path) return;
    const uint64_t key = va << 16 | n;
    g_mutex_lock(&lock);
    Rec* r = g_hash_table_lookup(recs, &key);
    if (!r) {
        r = g_new0(Rec, 1);
        r->key = key;
        r->vaddr = va;
        r->n = n;
        g_hash_table_insert(recs, &r->key, r);
    }
    g_mutex_unlock(&lock);
    qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &r->execs, 1);
}

static int cmp_u64(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

// ud: NULL when a window opens, non-NULL when it closes.
static void snap(gpointer k, gpointer v, gpointer ud) {
    (void)k;
    Rec* r = v;
    if (ud) r->acc += r->execs - r->base;
    else r->base = r->execs;
}

static void syscall_cb(qemu_plugin_id_t id, unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    (void)id, (void)vcpu, (void)a3, (void)a4, (void)a5, (void)a6, (void)a7, (void)a8;
    if (num != 4000) return;
    if (a1 == 0) {
        start = total;
    } else if (a1 == 1) {
        const uint64_t d = total - start;
        g_array_append_val(deltas, d);
    } else if (a1 == 2) {
        const guint n = deltas->len;
        if (n) {   // p99 as the bench's: the sorted blocks' value at 99% of their number
            uint64_t* v = (uint64_t*)deltas->data;
            double sum = 0;
            for (guint i = 0; i < n; ++i) sum += (double)v[i];
            qsort(v, n, sizeof *v, cmp_u64);
            char buf[160];
            snprintf(buf, sizeof buf, "COUNT case=%" PRIu64 " blocks=%u avg=%.1f p99=%.1f max=%.1f (k insns)\n",
                     a2, n, sum / n / 1000.0, (double)v[(guint)(n * 0.99)] / 1000.0, (double)v[n - 1] / 1000.0);
            qemu_plugin_outs(buf);
        }
        g_array_set_size(deltas, 0);
    } else if (a1 == 3 || a1 == 4) {
        if (!prof_path) return;
        g_mutex_lock(&lock);
        g_hash_table_foreach(recs, snap, a1 == 4 ? (gpointer)1 : NULL);
        g_mutex_unlock(&lock);
    }
}

static void at_exit(qemu_plugin_id_t id, void* p) {
    (void)id, (void)p;
    if (!prof_path) return;
    FILE* f = fopen(prof_path, "w");
    if (!f) {
        qemu_plugin_outs("efcount: can't write the profile\n");
        return;
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, recs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const Rec* r = v;
        if (r->acc) fprintf(f, "%" PRIx64 " %" PRIu64 " %" PRIu64 "\n", r->vaddr, r->n, r->acc);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
    (void)info;
    for (int i = 0; i < argc; ++i)
        if (!strncmp(argv[i], "prof=", 5)) prof_path = strdup(argv[i] + 5);
    deltas = g_array_new(FALSE, FALSE, sizeof(uint64_t));
    recs = g_hash_table_new(g_int64_hash, g_int64_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_vcpu_syscall_cb(id, syscall_cb);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
