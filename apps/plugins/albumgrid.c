#include "plugin.h"

#define TILE 70
#define COLS 4
#define GAP 6
#define ROWS 2
#define REC 10216
#define HDR 16
#define PIX_OFF 416
#define TILE_BYTES (70*70*2)
#define NSLOT 24
#define MAXTRACKS 100
#define MAXALBUMS 1024 /* recent mode ignores albums past this */
#define MAXRECENT 24

#define C_BG   LCD_RGBPACK(0x0F, 0x10, 0x13)
#define C_FG   LCD_RGBPACK(0xF1, 0xF2, 0xF4)
#define C_SUB  LCD_RGBPACK(0xA2, 0xA8, 0xB4)
#define C_RULE LCD_RGBPACK(0x26, 0x28, 0x2E)
#define C_ACC  LCD_RGBPACK(0x25, 0x63, 0xEB)

static int fd, count, nslots, sel, top_row;
static unsigned char *cache;
static int slot_album[NSLOT];
static char dir[MAX_PATH];
static char tracks[MAXTRACKS][MAX_PATH];
static int ntracks;

#define POS_FILE ROCKBOX_DIR "/albumgrid.pos"

static bool recent;           /* "Recently Played": records shown in order[] */
static int order[MAXALBUMS];
static long last[MAXALBUMS];

/* file offset of the record shown at grid position i */
static off_t rec_at(int i)
{
    return HDR + (off_t)(recent ? order[i] : i) * REC;
}

static uint32_t dir_hash(const char *path, int len)
{
    return rb->crc_32(path, len, 0xffffffff);
}

static int cmp_last(const void *a, const void *b)
{
    long la = last[*(const int *)a], lb = last[*(const int *)b];
    return la < lb ? 1 : la > lb ? -1 : 0;
}

/* order albums by their most recently played track (the database's
   lastplayed serial, set when a track ends or is skipped); the album
   playing now counts as newest */
static void sort_recent(void)
{
    static uint32_t hash[MAXALBUMS];
    struct tagcache_search tcs;
    struct mp3entry *id3 = rb->audio_status() ? rb->audio_current_track() : NULL;
    char buf[MAX_PATH], *slash;
    int n = MIN(count, MAXALBUMS), i;

    for (i = 0; i < n; i++)
    {
        rb->lseek(fd, HDR + (off_t)i * REC + 160, SEEK_SET);
        rb->memset(buf, 0, sizeof(buf));
        rb->read(fd, buf, 255);
        hash[i] = dir_hash(buf, rb->strlen(buf));
        last[i] = 0;
    }
    if (rb->tagcache_search(&tcs, tag_lastplayed))
    {
        while (rb->tagcache_get_next(&tcs, buf, sizeof(buf)))
        {
            long lp = rb->tagcache_get_numeric(&tcs, tag_lastplayed);
            uint32_t h;
            if (lp <= 0 || !rb->tagcache_retrieve(&tcs, tcs.idx_id, tag_filename,
                                                  buf, sizeof(buf)))
                continue;
            slash = rb->strrchr(buf, '/');
            if (!slash)
                continue;
            h = dir_hash(buf, slash - buf);
            for (i = 0; i < n; i++)
                if (hash[i] == h && last[i] < lp)
                    last[i] = lp;
        }
        rb->tagcache_search_finish(&tcs);
    }
    if (id3 && id3->path[0] && (slash = rb->strrchr(id3->path, '/')))
    {
        uint32_t h = dir_hash(id3->path, slash - id3->path);
        for (i = 0; i < n; i++)
            if (hash[i] == h)
                last[i] = LONG_MAX;
    }
    count = 0;
    for (i = 0; i < n; i++)
        if (last[i] > 0)
            order[count++] = i;
    rb->qsort(order, count, sizeof(int), cmp_last);
    if (count > MAXRECENT)
        count = MAXRECENT;
}

/* remember the selected album so re-entering the grid lands on it */
static int leave(int status)
{
    int pf = recent ? -1 : rb->open(POS_FILE, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (pf >= 0)
    {
        rb->write(pf, &sel, sizeof(sel));
        rb->close(pf);
    }
    rb->close(fd);
    return status;
}

static int cmp_track(const void *a, const void *b)
{
    return rb->strcasecmp((const char *)a, (const char *)b);
}

/* audio files of the selected album, sorted by name ("01 - ..." order) */
static void load_tracks(void)
{
    struct dirent *e;
    DIR *d;

    rb->lseek(fd, rec_at(sel) + 160, SEEK_SET);
    rb->memset(dir, 0, sizeof(dir));
    rb->read(fd, dir, 255);
    ntracks = 0;
    d = rb->opendir(dir);
    if (!d)
        return;
    while ((e = rb->readdir(d)) && ntracks < MAXTRACKS)
    {
        if (e->d_name[0] == '.' ||
            (rb->filetype_get_attr(e->d_name) & FILE_ATTR_MASK) != FILE_ATTR_AUDIO)
            continue;
        rb->strlcpy(tracks[ntracks++], e->d_name, MAX_PATH);
    }
    rb->closedir(d);
    rb->qsort(tracks, ntracks, MAX_PATH, cmp_track);
}

static const char *track_name(int i, void *data, char *buf, size_t len)
{
    char *dot;
    (void)data;
    rb->strlcpy(buf, tracks[i], len);
    dot = rb->strrchr(buf, '.');
    if (dot)
        *dot = '\0';
    return buf;
}

static int list_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    /* Menu backs out of the track list like Left does */
    return action == ACTION_STD_MENU ? ACTION_STD_CANCEL : action;
}

/* returns a plugin status to exit with, or -1 to go back to the grid */
static int show_tracks(void)
{
    struct simplelist_info info;
    char path[MAX_PATH], title[96];
    int i;

    load_tracks();
    if (ntracks == 0)
    {
        rb->splash(HZ*2, "No tracks found");
        return -1;
    }
    rb->lseek(fd, rec_at(sel), SEEK_SET);
    rb->read(fd, title, sizeof(title));
    title[95] = 0;
    rb->simplelist_info_init(&info, title, ntracks, NULL);
    info.get_name = track_name;
    info.action_callback = list_action;
    if (rb->simplelist_show_list(&info))
        return PLUGIN_USB_CONNECTED;
    if (info.selection == -2)
        return PLUGIN_GOTO_WPS;
    if (info.selection < 0)
        return -1;
    if (rb->global_settings->party_mode && rb->audio_status())
    {   /* warn_on_pl_erase() doesn't guard party mode; never replace its queue */
        rb->splash(HZ*2, ID2P(LANG_PARTY_MODE));
        return -1;
    }
    if (!rb->warn_on_pl_erase())
        return -1;
    rb->playlist_create(dir, NULL);
    for (i = 0; i < ntracks; i++)
    {
        rb->snprintf(path, sizeof(path), "%s/%s", dir, tracks[i]);
        rb->playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, false);
    }
    rb->playlist_sync(NULL);
    rb->playlist_start(info.selection, 0, 0);
    return PLUGIN_GOTO_WPS;
}

static fb_data *get_tile(int i)
{
    int slot = i % nslots;
    fb_data *p = (fb_data *)(cache + slot * TILE_BYTES);
    if (slot_album[slot] != i)
    {
        rb->lseek(fd, rec_at(i) + PIX_OFF, SEEK_SET);
        rb->read(fd, p, TILE_BYTES);
        slot_album[slot] = i;
    }
    return p;
}

static void draw(void)
{
    char cap[160];
    int x0 = (LCD_WIDTH - (COLS*TILE + (COLS-1)*GAP)) / 2;
    int r, c, i;

    rb->lcd_set_foreground(C_FG);
    rb->lcd_set_background(C_BG);
    rb->lcd_clear_display();
    rb->lcd_putsxy(8, 2, recent ? "Recently Played" : "Albums");
    rb->lcd_set_foreground(C_RULE);
    rb->lcd_hline(0, LCD_WIDTH - 1, 22);
    for (r = 0; r < ROWS; r++)
        for (c = 0; c < COLS; c++)
        {
            int x = x0 + c * (TILE + GAP), y = 30 + r * (TILE + GAP);
            i = (top_row + r) * COLS + c;
            if (i >= count)
                continue;
            rb->lcd_bitmap(get_tile(i), x, y, TILE, TILE);
            if (i == sel)
            {
                rb->lcd_set_foreground(C_ACC);
                rb->lcd_fillrect(x - 7, y - 7, TILE + 14, 3);
                rb->lcd_fillrect(x - 7, y + TILE + 4, TILE + 14, 3);
                rb->lcd_fillrect(x - 7, y - 7, 3, TILE + 14);
                rb->lcd_fillrect(x + TILE + 4, y - 7, 3, TILE + 14);
            }
        }
    rb->lcd_set_foreground(C_RULE);
    rb->lcd_hline(0, LCD_WIDTH - 1, 184);
    rb->memset(cap, 0, sizeof(cap));
    rb->lseek(fd, rec_at(sel), SEEK_SET);
    rb->read(fd, cap, sizeof(cap));
    cap[95] = 0;
    cap[159] = 0;
    rb->lcd_set_foreground(C_FG);
    rb->lcd_putsxy(8, 190, cap);
    rb->lcd_set_foreground(C_SUB);
    rb->lcd_putsxy(8, 212, cap + 96);
    rb->lcd_update();
}

enum plugin_status plugin_start(const void* parameter)
{
    char hdr[HDR];
    uint32_t tile;
    size_t size;
    int i, button, old;
    recent = parameter && !rb->strcmp(parameter, "recent");

    fd = rb->open("/.rockbox/albumgrid.dat", O_RDONLY);
    if (fd < 0 || rb->read(fd, hdr, HDR) != HDR || rb->memcmp(hdr, "AGR1", 4) != 0)
        goto nodata;
    rb->memcpy(&count, hdr + 4, 4);
    rb->memcpy(&tile, hdr + 8, 4);
    if (tile != TILE || count <= 0 ||
        count > (rb->ffilesize(fd) - HDR) / REC)  /* truncated or corrupt index */
        goto nodata;
    if (recent)
    {
        sort_recent();
        if (count == 0)
        {
            rb->close(fd);
            rb->splash(HZ*2, "Nothing played yet");
            return PLUGIN_GOTO_ROOT;
        }
    }

    cache = rb->plugin_get_buffer(&size);
    nslots = size / TILE_BYTES;
    if (nslots > NSLOT)
        nslots = NSLOT;
    if (nslots < 8)
    {
        rb->splash(HZ*3, "Not enough memory");
        rb->close(fd);
        return PLUGIN_GOTO_ROOT;
    }
    for (i = 0; i < NSLOT; i++)
        slot_album[i] = -1;
    sel = top_row = 0;
    i = recent ? -1 : rb->open(POS_FILE, O_RDONLY);
    if (i >= 0)
    {
        if (rb->read(i, &sel, sizeof(sel)) != sizeof(sel) || sel < 0 || sel >= count)
            sel = 0;
        rb->close(i);
    }
    top_row = sel / COLS > 0 ? sel / COLS - 1 : 0;
    draw();

    while (1)
    {
        button = rb->get_action(CONTEXT_TREE, TIMEOUT_BLOCK);
        old = sel;
        switch (button)
        {
        case ACTION_STD_NEXT:
        case ACTION_STD_NEXTREPEAT:
            sel++;
            break;
        case ACTION_STD_PREV:
        case ACTION_STD_PREVREPEAT:
            sel--;
            break;
        case ACTION_STD_OK:
            i = show_tracks();
            if (i >= 0)
                return leave(i);
            draw();
            continue;
        case ACTION_TREE_WPS:
            if (rb->audio_status())
                return leave(PLUGIN_GOTO_WPS);
            break;
        case ACTION_STD_CANCEL:
        case ACTION_STD_MENU:
            return leave(PLUGIN_GOTO_ROOT);
        default:
            if (rb->default_event_handler(button) == SYS_USB_CONNECTED)
                return leave(PLUGIN_USB_CONNECTED);
            break;
        }
        if (sel < 0)
            sel = 0;
        if (sel > count - 1)
            sel = count - 1;
        if (sel != old)
        {
            int row = sel / COLS;
            if (row < top_row)
                top_row = row;
            if (row > top_row + 1)
                top_row = row - 1;
            draw();
        }
    }

nodata:
    if (fd >= 0)
        rb->close(fd);
    rb->splash(HZ*3, "No album grid yet: run build_albumgrid.py");
    return PLUGIN_GOTO_ROOT;
}
