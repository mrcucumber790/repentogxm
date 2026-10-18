/* Negative cache for the game's mod resource probes (64-bit hosts).
 *
 * For every asset it loads, the game asks each enabled mod whether it
 * overrides it: _access("<mods>/<mod>/resources/<asset>") and the same under
 * resources-dlc3.  Almost every answer is "no", and on the Switch each one
 * is a synchronous SD card service call, so a room transition with a few
 * mods spends most of its time asking.  Here each directory along such a
 * path is listed once; a path whose component is missing from its parent's
 * listing is answered as missing without touching the card.  Paths that do
 * exist still go to the real stat.
 *
 * Only paths of the form .../mods/<mod>/resources*<...> are cached: the game
 * writes into a mod folder only at its top (disable.it from the Mods menu),
 * never under its resources.  The Switch SD card is case-insensitive, so
 * names compare without case there. */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define PROBE_DIRS_MAX 4096u
#define PROBE_SLOTS 8192u        /* power of two, > 2 * PROBE_DIRS_MAX */
#define PROBE_PATH_MAX 512u

typedef struct {
    char *path;          /* folded directory path, no trailing slash */
    char **names;        /* folded entry names, sorted; NULL when missing */
    uint32_t count;
    int missing;
} probe_dir;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static probe_dir *s_dirs;
static uint32_t *s_slots;          /* index + 1 into s_dirs, 0 = empty */
static uint32_t s_dir_count;
static int s_full;
static uint32_t s_hits, s_listed;

static char fold(char ch)
{
#if defined(__SWITCH__)
    if (ch >= 'A' && ch <= 'Z')
        return (char)(ch - 'A' + 'a');
#endif
    return ch;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static uint32_t probe_hash(const char *text)
{
    uint32_t hash = 2166136261u;

    while (*text)
        hash = (hash ^ (uint8_t)*text++) * 16777619u;
    return hash;
}

/* The slot holding folded, or the empty slot where it would go. */
static uint32_t *probe_slot(const char *folded)
{
    uint32_t i = probe_hash(folded) & (PROBE_SLOTS - 1u);

    while (s_slots[i] && strcmp(s_dirs[s_slots[i] - 1u].path, folded))
        i = (i + 1u) & (PROBE_SLOTS - 1u);
    return &s_slots[i];
}

static const probe_dir *probe_find(const char *folded)
{
    uint32_t index = *probe_slot(folded);

    return index ? &s_dirs[index - 1u] : NULL;
}

/* real_dir is the on-disk spelling, folded its cache key. */
static const probe_dir *probe_list(const char *real_dir, const char *folded)
{
    const probe_dir *found;
    probe_dir dir;
    DIR *handle;
    struct dirent *entry;
    uint32_t capacity = 0;

    if (s_full)
        return NULL;
    if (!s_dirs) {
        s_dirs = calloc(PROBE_DIRS_MAX, sizeof *s_dirs);
        s_slots = calloc(PROBE_SLOTS, sizeof *s_slots);
        if (!s_dirs || !s_slots) {
            s_full = 1;
            return NULL;
        }
    }
    found = probe_find(folded);
    if (found)
        return found;
    if (s_dir_count == PROBE_DIRS_MAX) {
        s_full = 1;
        return NULL;
    }
    memset(&dir, 0, sizeof dir);
    dir.path = strdup(folded);
    if (!dir.path)
        return NULL;
    handle = opendir(real_dir);
    if (!handle) {
        dir.missing = 1;
    } else {
        while ((entry = readdir(handle)) != NULL) {
            size_t length = strlen(entry->d_name);
            char *name;
            size_t i;

            if (dir.count == capacity) {
                uint32_t grown = capacity ? capacity * 2u : 32u;
                char **names = realloc(dir.names, grown * sizeof *names);
                if (!names)
                    break;
                dir.names = names;
                capacity = grown;
            }
            name = malloc(length + 1u);
            if (!name)
                break;
            for (i = 0; i <= length; ++i)
                name[i] = fold(entry->d_name[i]);
            dir.names[dir.count++] = name;
        }
        closedir(handle);
        if (dir.count)
            qsort(dir.names, dir.count, sizeof *dir.names, cmp_names);
    }
    ++s_listed;
    s_dirs[s_dir_count] = dir;
    *probe_slot(folded) = ++s_dir_count;
    return &s_dirs[s_dir_count - 1u];
}

static int probe_has(const probe_dir *dir, const char *folded_name)
{
    const char *key = folded_name;

    return dir->count &&
           bsearch(&key, dir->names, dir->count, sizeof *dir->names,
                   cmp_names) != NULL;
}

/* 0: unknown, ask the card; 1: the file is missing from an existing
 * directory; 2: a directory on the way is missing. */
int isaac_mod_probe_known_missing(const char *path)
{
    char real[PROBE_PATH_MAX];
    char folded[PROBE_PATH_MAX];
    const char *mods = strstr(path, "/mods/");
    size_t length = strlen(path);
    size_t start, i, component = 0;
    int missing = 0;

    if (!mods || length >= PROBE_PATH_MAX)
        return 0;
    start = (size_t)(mods - path) + 6u;
    /* .../mods/<mod>/resources... with at least one component below */
    {
        const char *slash = strchr(path + start, '/');
        if (!slash || slash == path + start ||
            strncmp(slash + 1, "resources", 9) != 0 ||
            !strchr(slash + 1, '/'))
            return 0;
    }
    for (i = 0; i <= length; ++i) {
        real[i] = path[i];
        folded[i] = fold(path[i]);
    }
    pthread_mutex_lock(&s_lock);
    /* check each component from <mod> down against its parent listing */
    i = start - 1u;      /* the slash after "mods" */
    while (!missing && i < length) {
        size_t end = i + 1u;
        const probe_dir *dir;

        while (end < length && path[end] != '/')
            ++end;
        if (end == i + 1u ||
            (end - i == 2u && path[i + 1] == '.') ||
            (end - i == 3u && path[i + 1] == '.' && path[i + 2] == '.'))
            break;       /* empty, "." or "..": leave it to the real stat */
        real[i] = folded[i] = '\0';
        dir = probe_list(real, folded);
        real[i] = folded[i] = '/';
        if (!dir)
            break;
        if (dir->missing) {
            missing = 2;
            break;
        }
        {
            char saved = folded[end];
            folded[end] = '\0';
            if (!probe_has(dir, folded + i + 1u))
                missing = end == length ? 1 : 2;
            folded[end] = saved;
        }
        ++component;
        i = end;
    }
    if (missing)
        ++s_hits;
    pthread_mutex_unlock(&s_lock);
    (void)component;
    return missing;
}

void isaac_mod_probe_report(void)
{
    pthread_mutex_lock(&s_lock);
    isaac_vita_log("mod probe cache: dirs=%u answered_missing=%u full=%d",
                   (unsigned)s_listed, (unsigned)s_hits, s_full);
    pthread_mutex_unlock(&s_lock);
}
