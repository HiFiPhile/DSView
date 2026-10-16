/*
 * dsldecode: run DSView's protocol decoders on a capture, without the GUI
 * (GPL-3.0, as DSView).
 *
 *   dsldecode --list
 *   dsldecode [--decoders DIR] [--all] -P ID[:CH=N...][:OPT=VALUE...] [-P ID[:OPT=VALUE...]]... FILE.dsl
 *
 * FILE.dsl is a DSView session file, as written by DSView or by
 * dslcap --dsl. The first -P decoder takes the capture: each of its
 * channels CH is mapped to the capture channel N. Every further -P decoder
 * is stacked on the previous one. Options take the type of their default.
 *
 * Prints one JSON object per line for each annotation of the last decoder
 * (of every decoder with --all), then {"done":true,...}; every failure
 * prints an object with an "error" key instead. --list prints the decoders
 * with their channels and options.
 *
 * Exit status: 0 success, 1 runtime or decoding error, 2 invalid arguments.
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <minizip/unzip.h>
#include "libsigrokdecode.h"

/* Samples per decoder call, as DSView's DecoderStack::MaxChunkSize. */
#define CHUNK_SAMPLES (1024 * 16)
/* Samples per .dsl data block, DSView's LogicSnapshot::LeafBlockSamples. */
#define BLOCK_SAMPLES (1 << 24)
#define MAX_STACK 8

static struct srd_decoder_inst *g_top = NULL;
static int g_all = 0;
static uint64_t g_samplerate = 0;
static uint64_t g_annotations = 0;

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20)
            printf("\\u%04x", (unsigned char)*s);
        else {
            if (*s == '"' || *s == '\\') putchar('\\');
            putchar(*s);
        }
    }
    putchar('"');
}

static int fail(int rc, const char *error, const char *detail)
{
    printf("{\"error\":");
    json_str(error);
    if (detail) {
        printf(",\"detail\":");
        json_str(detail);
    }
    printf("}\n");
    return rc;
}

/* The id of an annotation class: (type, id, desc) or (id, desc). The
 * library lists the classes in reverse order. */
static const char *ann_class_id(const struct srd_decoder *dec, int ann_class)
{
    int n = (int)g_slist_length(dec->annotations);
    char **ann = ann_class >= 0 && ann_class < n ?
                 g_slist_nth_data(dec->annotations, n - 1 - ann_class) : NULL;
    if (!ann) return "";
    return g_strv_length(ann) == 3 ? ann[1] : ann[0];
}

static void on_annotation(struct srd_proto_data *pdata, void *cb_data)
{
    (void)cb_data;
    struct srd_decoder_inst *di = pdata->pdo->di;
    const struct srd_proto_data_annotation *pda = pdata->data;
    if (!g_all && di != g_top)
        return;

    printf("{\"pd\":");
    json_str(di->decoder->id);
    printf(",\"ann\":");
    json_str(ann_class_id(di->decoder, pda->ann_class));
    printf(",\"start\":%llu,\"end\":%llu", (unsigned long long)pdata->start_sample,
           (unsigned long long)pdata->end_sample);
    if (g_samplerate)
        printf(",\"t\":%.9g", (double)pdata->start_sample / (double)g_samplerate);
    /* A numeric annotation ("@41" from the decoder) has its value in hex, and
     * a "\n" text in its place. DSView shows the value where a text has {$},
     * in hex by default, or alone. */
    const char *hex = pda->str_number_hex[0] ? pda->str_number_hex : NULL;
    if (hex) {
        printf(",\"hex\":");
        json_str(hex);
        char *end;
        guint64 v = g_ascii_strtoull(hex, &end, 16);
        if (!*end && strlen(hex) <= 16)
            printf(",\"value\":%llu", (unsigned long long)v);
    }
    printf(",\"texts\":[");
    int n = 0;
    for (char **t = pda->ann_text; t && *t; t++) {
        if (!strcmp(*t, "\n"))
            continue;
        if (n++) putchar(',');
        if (hex) {
            gchar **parts = g_strsplit(*t, "{$}", -1);
            gchar *text = g_strjoinv(hex, parts);
            json_str(text);
            g_free(text);
            g_strfreev(parts);
        } else {
            json_str(*t);
        }
    }
    if (!n && hex) json_str(hex);
    printf("]}\n");
    g_annotations++;
}

static void print_variant(GVariant *v)
{
    if (!v) printf("null");
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) json_str(g_variant_get_string(v, NULL));
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) printf("%lld", (long long)g_variant_get_int64(v));
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) printf("%.17g", g_variant_get_double(v));
    else printf("null");
}

static void print_channels(const char *key, const GSList *channels)
{
    printf(",\"%s\":[", key);
    for (const GSList *l = channels; l; l = l->next) {
        const struct srd_channel *ch = l->data;
        printf("%s{\"id\":", l == channels ? "" : ",");
        json_str(ch->id);
        printf(",\"name\":");
        json_str(ch->name);
        printf("}");
    }
    printf("]");
}

static void list_decoders(void)
{
    printf("{\"decoders\":[");
    for (const GSList *l = srd_decoder_list(); l; l = l->next) {
        const struct srd_decoder *dec = l->data;
        printf("%s{\"id\":", l == srd_decoder_list() ? "" : ",");
        json_str(dec->id);
        printf(",\"name\":");
        json_str(dec->name);
        printf(",\"longname\":");
        json_str(dec->longname);
        printf(",\"inputs\":[");
        for (const GSList *i = dec->inputs; i; i = i->next) {
            if (i != dec->inputs) putchar(',');
            json_str(i->data);
        }
        printf("],\"outputs\":[");
        for (const GSList *o = dec->outputs; o; o = o->next) {
            if (o != dec->outputs) putchar(',');
            json_str(o->data);
        }
        printf("]");
        print_channels("channels", dec->channels);
        print_channels("optional_channels", dec->opt_channels);
        printf(",\"options\":[");
        for (const GSList *o = dec->options; o; o = o->next) {
            const struct srd_decoder_option *opt = o->data;
            printf("%s{\"id\":", o == dec->options ? "" : ",");
            json_str(opt->id);
            printf(",\"desc\":");
            json_str(opt->desc);
            printf(",\"default\":");
            print_variant(opt->def);
            printf(",\"values\":[");
            for (const GSList *v = opt->values; v; v = v->next) {
                if (v != opt->values) putchar(',');
                print_variant(v->data);
            }
            printf("]}");
        }
        printf("],\"annotations\":[");
        for (int a = 0; a < (int)g_slist_length(dec->annotations); a++) {
            if (a) putchar(',');
            json_str(ann_class_id(dec, a));
        }
        printf("]}");
    }
    printf("]}\n");
}

/* A decoder of a -P argument, with its channel mapping. */
struct stage {
    struct srd_decoder_inst *di;
    const struct srd_decoder *dec;
    int map[64];                   /* capture channel of each decoder channel, or -1 */
    int nmap;
};

/* Parse "ID[:KEY=VALUE]...": an ID may itself contain ':' (as 1:uart), its
 * parts are the ones without '='. KEY is a channel of the decoder, mapped
 * to capture channel VALUE, or an option, of the type of its default. */
static int add_stage(struct srd_session *sess, const char *spec, struct stage *st,
                     struct stage *below)
{
    gchar **parts = g_strsplit(spec, ":", -1);
    int first_kv = 0;
    while (parts[first_kv] && !strchr(parts[first_kv], '=')) first_kv++;
    gchar *saved = parts[first_kv];
    parts[first_kv] = NULL;
    gchar *id = g_strjoinv(":", parts);
    parts[first_kv] = saved;

    int rc = 0;
    const struct srd_decoder *dec = srd_decoder_get_by_id(id);
    GHashTable *opts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)g_variant_unref);
    GHashTable *chans = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              (GDestroyNotify)g_variant_unref);
    if (!dec) {
        rc = fail(2, "unknown decoder", id);
        goto done;
    }
    st->dec = dec;
    st->nmap = g_slist_length(dec->channels) + g_slist_length(dec->opt_channels);
    if (st->nmap > (int)G_N_ELEMENTS(st->map)) {
        rc = fail(2, "decoder has too many channels", id);
        goto done;
    }
    for (int i = 0; i < st->nmap; i++) st->map[i] = -1;

    for (int k = first_kv; parts[k]; k++) {
        gchar *eq = strchr(parts[k], '=');
        if (!eq || eq == parts[k]) {
            rc = fail(2, "invalid decoder argument", parts[k]);
            goto done;
        }
        gchar *key = g_strndup(parts[k], eq - parts[k]);
        const char *val = eq + 1;
        const struct srd_channel *ch = NULL;
        for (const GSList *l = dec->channels; l && !ch; l = l->next)
            if (!strcmp(((struct srd_channel *)l->data)->id, key)) ch = l->data;
        for (const GSList *l = dec->opt_channels; l && !ch; l = l->next)
            if (!strcmp(((struct srd_channel *)l->data)->id, key)) ch = l->data;
        if (ch) {
            char *end;
            long n = strtol(val, &end, 10);
            if (below || *end || end == val || n < 0 || n > 63 || ch->order >= 64) {
                rc = fail(2, below ? "a stacked decoder takes no channels" : "invalid channel number", parts[k]);
                g_free(key);
                goto done;
            }
            st->map[ch->order] = (int)n;
            g_hash_table_insert(chans, key, g_variant_ref_sink(g_variant_new_int32((gint32)n)));
            continue;
        }
        const struct srd_decoder_option *opt = NULL;
        for (const GSList *l = dec->options; l && !opt; l = l->next)
            if (!strcmp(((struct srd_decoder_option *)l->data)->id, key)) opt = l->data;
        GVariant *v = NULL;
        char *end = NULL;
        if (!opt) {
            rc = fail(2, "unknown channel or option", parts[k]);
        } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_INT64)) {
            long long n = strtoll(val, &end, 0);
            if (*end || end == val) rc = fail(2, "invalid integer option", parts[k]);
            else v = g_variant_new_int64(n);
        } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE_DOUBLE)) {
            double d = g_ascii_strtod(val, &end);
            if (*end || end == val) rc = fail(2, "invalid number option", parts[k]);
            else v = g_variant_new_double(d);
        } else {
            v = g_variant_new_string(val);
        }
        if (rc) {
            g_free(key);
            goto done;
        }
        g_hash_table_insert(opts, key, g_variant_ref_sink(v));
    }

    /* The library only checks the required channels when some are given. */
    for (const GSList *l = dec->channels; l && !below; l = l->next) {
        const struct srd_channel *ch = l->data;
        if (st->map[ch->order] < 0) {
            rc = fail(2, "missing decoder channel", ch->id);
            goto done;
        }
    }
    if (!(st->di = srd_inst_new(sess, dec->id, opts))) {
        rc = fail(2, "cannot create the decoder", spec);
        goto done;
    }
    if (!below && srd_inst_channel_set_all(st->di, chans) != SRD_OK) {
        rc = fail(2, "missing or invalid channels", spec);
        goto done;
    }
    if (below && srd_inst_stack(sess, below->di, st->di) != SRD_OK)
        rc = fail(2, "cannot stack the decoder", spec);
done:
    g_hash_table_destroy(opts);
    g_hash_table_destroy(chans);
    g_free(id);
    g_strfreev(parts);
    return rc;
}

/* Read a whole file of the archive into buf, which grows as needed. */
static int read_entry(unzFile z, const char *name, uint8_t **buf, size_t *cap, size_t *len)
{
    unz_file_info64 info;
    if (unzLocateFile(z, name, 0) != UNZ_OK ||
            unzGetCurrentFileInfo64(z, &info, NULL, 0, NULL, 0, NULL, 0) != UNZ_OK)
        return -1;
    if (info.uncompressed_size + 1 > *cap) {
        g_free(*buf);
        *cap = (size_t)info.uncompressed_size + 1;
        if (!(*buf = g_try_malloc(*cap))) { *cap = 0; return -1; }
    }
    if (unzOpenCurrentFile(z) != UNZ_OK) return -1;
    int n = unzReadCurrentFile(z, *buf, (unsigned)info.uncompressed_size);
    unzCloseCurrentFile(z);
    if (n < 0 || (uint64_t)n != info.uncompressed_size) return -1;
    (*buf)[n] = 0;
    *len = (size_t)n;
    return 0;
}

/* The directory of this executable, or NULL. */
static char *exe_dir(void)
{
    char *path = NULL;
#if defined(_WIN32)
    char *pgm = NULL;
    if (_get_pgmptr(&pgm) == 0 && pgm) path = _fullpath(NULL, pgm, 0);
#elif defined(__APPLE__)
    char exe[4096];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) == 0) path = realpath(exe, NULL);
#else
    char *link = g_file_read_link("/proc/self/exe", NULL);
    path = link ? strdup(link) : NULL;
    g_free(link);
#endif
    char *dir = path ? g_path_get_dirname(path) : NULL;
    free(path);
    return dir;
}

/* The decoders directory: --decoders, DSLDECODE_DECODERS, then next to this
 * executable as DSView looks for it. */
static char *find_decoders(const char *given)
{
    if (given) return g_strdup(given);
    const char *env = getenv("DSLDECODE_DECODERS");
    if (env) return g_strdup(env);
    char *dir = exe_dir(), *found = NULL;
    const char *candidates[] = { "decoders", "../share/libsigrokdecode4DSL/decoders",
                                 "../libsigrokdecode4DSL/decoders" };
    for (size_t i = 0; dir && i < G_N_ELEMENTS(candidates) && !found; i++) {
        char *p = g_build_filename(dir, candidates[i], NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) found = p;
        else g_free(p);
    }
    g_free(dir);
    return found;
}

#ifdef _WIN32
/* As DSView: the Windows package carries the Python standard library in
 * "pylib" next to the executable, flat rather than in lib/pythonX.Y. */
static void use_bundled_python(void)
{
    char *dir = exe_dir();
    char *home = dir ? g_build_filename(dir, "pylib", NULL) : NULL;
    if (home && g_file_test(home, G_FILE_TEST_IS_DIR)) {
        static gunichar2 *whome;
        whome = g_utf8_to_utf16(home, -1, NULL, NULL, NULL);
        if (whome) srd_set_python_home((const wchar_t *)whome);
        char *path = g_strdup_printf("%s;%s/lib-dynload", home, home);
        g_setenv("PYTHONPATH", path, TRUE);
        g_free(path);
    }
    g_free(home);
    g_free(dir);
}
#endif

int main(int argc, char **argv)
{
    const char *decoders_arg = NULL, *file = NULL;
    const char *specs[MAX_STACK];
    int nspecs = 0, list = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "--all")) g_all = 1;
        else if (!strcmp(a, "--decoders") || !strcmp(a, "-P")) {
            if (i + 1 >= argc) return fail(2, "missing option value", a);
            if (!strcmp(a, "--decoders")) decoders_arg = argv[++i];
            else if (nspecs == MAX_STACK) return fail(2, "too many stacked decoders", NULL);
            else specs[nspecs++] = argv[++i];
        }
        else if (a[0] == '-') return fail(2, "unknown argument", a);
        else if (file) return fail(2, "only one capture file", a);
        else file = a;
    }
    if (!list && (!file || !nspecs))
        return fail(2, "usage: dsldecode -P ID[:CH=N...][:OPT=VALUE...] [-P ...] FILE.dsl", NULL);

    char *decoders = find_decoders(decoders_arg);
    if (!decoders) return fail(2, "decoders directory not found; set --decoders or DSLDECODE_DECODERS", NULL);
#ifdef _WIN32
    use_bundled_python();
#endif
    srd_log_level(1); /* errors only, on stderr */
    if (srd_init(decoders) != SRD_OK) {
        g_free(decoders);
        return fail(1, "cannot initialize the decoder library", NULL);
    }
    g_free(decoders);
    srd_decoder_load_all();
    if (list) {
        list_decoders();
        srd_exit();
        return 0;
    }

    /* The capture: its header, then its data blocks. */
    unzFile z = unzOpen64(file);
    uint8_t *hdr = NULL;
    size_t hdr_cap = 0, hdr_len = 0;
    if (!z || read_entry(z, "header", &hdr, &hdr_cap, &hdr_len)) {
        if (z) unzClose(z);
        srd_exit();
        return fail(1, "cannot read the capture header", file);
    }
    GKeyFile *kf = g_key_file_new();
    gboolean ok = g_key_file_load_from_data(kf, (const char *)hdr, hdr_len, G_KEY_FILE_NONE, NULL);
    g_free(hdr);
    gchar *rate_str = ok ? g_key_file_get_string(kf, "header", "samplerate", NULL) : NULL;
    gchar *total_str = ok ? g_key_file_get_string(kf, "header", "total samples", NULL) : NULL;
    gint mode = ok ? g_key_file_get_integer(kf, "header", "device mode", NULL) : -1;
    g_key_file_free(kf);
    uint64_t total = total_str ? g_ascii_strtoull(total_str, NULL, 10) : 0;
    /* As libsigrok4DSL reads it: an integer, then an optional k/M/G unit. */
    if (rate_str) {
        char *end;
        g_samplerate = g_ascii_strtoull(rate_str, &end, 10);
        while (*end == ' ') end++;
        if (*end == 'k' || *end == 'K') g_samplerate *= 1000ULL;
        else if (*end == 'M' || *end == 'm') g_samplerate *= 1000000ULL;
        else if (*end == 'G' || *end == 'g') g_samplerate *= 1000000000ULL;
    }
    g_free(rate_str);
    g_free(total_str);
    if (mode != 0 || !total || !g_samplerate) {
        unzClose(z);
        srd_exit();
        return fail(1, "not a logic analyzer capture", file);
    }

    struct srd_session *sess = NULL;
    struct stage stages[MAX_STACK];
    memset(stages, 0, sizeof stages);
    int rc = srd_session_new(&sess) == SRD_OK ? 0 : fail(1, "cannot create a decoder session", NULL);
    for (int i = 0; i < nspecs && !rc; i++)
        rc = add_stage(sess, specs[i], &stages[i], i ? &stages[i - 1] : NULL);
    if (!rc) {
        g_top = stages[nspecs - 1].di;
        srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE, g_variant_new_uint64(g_samplerate));
        srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, on_annotation, NULL);
        char *error = NULL;
        if (srd_session_start(sess, &error) != SRD_OK) {
            rc = fail(1, "cannot start the decoders", error);
            g_free(error);
        }
    }

    /* Feed the bottom decoder block by block, in DSView's chunk size. */
    struct stage *bottom = &stages[0];
    uint8_t *bufs[64] = { NULL };
    size_t caps[64] = { 0 }, lens[64];
    const uint8_t *inbuf[64];
    uint8_t inbuf_const[64];
    uint64_t blocks = (total + BLOCK_SAMPLES - 1) / BLOCK_SAMPLES;
    for (uint64_t b = 0; b < blocks && !rc; b++) {
        uint64_t block_start = b * BLOCK_SAMPLES;
        uint64_t block_len = MIN((uint64_t)BLOCK_SAMPLES, total - block_start);
        for (int j = 0; j < bottom->nmap && !rc; j++) {
            if (bottom->map[j] < 0) continue;
            char name[32];
            snprintf(name, sizeof name, "L-%d/%llu", bottom->map[j], (unsigned long long)b);
            if (read_entry(z, name, &bufs[j], &caps[j], &lens[j]) || lens[j] * 8 < block_len)
                rc = fail(1, "capture channel missing or short", name);
        }
        for (uint64_t s = 0; s < block_len && !rc; s += CHUNK_SAMPLES) {
            uint64_t n = MIN((uint64_t)CHUNK_SAMPLES, block_len - s);
            for (int j = 0; j < bottom->nmap; j++) {
                if (bottom->map[j] < 0) {
                    inbuf[j] = NULL;
                    inbuf_const[j] = 0;
                } else {
                    inbuf[j] = bufs[j] + s / 8;
                    inbuf_const[j] = inbuf[j][0] & 1;
                }
            }
            char *error = NULL;
            if (srd_session_send(sess, block_start + s, block_start + s + n, inbuf, inbuf_const,
                                 n, &error) != SRD_OK) {
                rc = fail(1, "decoding failed", error);
                g_free(error);
            }
        }
    }
    if (!rc) {
        char *error = NULL;
        if (srd_session_end(sess, &error) != SRD_OK) {
            rc = fail(1, "decoding failed at the end of the capture", error);
            g_free(error);
        }
    }
    if (!rc)
        printf("{\"done\":true,\"samples\":%llu,\"samplerate\":%llu,\"annotations\":%llu}\n",
               (unsigned long long)total, (unsigned long long)g_samplerate,
               (unsigned long long)g_annotations);

    for (int j = 0; j < 64; j++) g_free(bufs[j]);
    if (sess) srd_session_destroy(sess);
    unzClose(z);
    srd_exit();
    if (fflush(stdout) || ferror(stdout)) return rc ? rc : 1;
    return rc;
}
