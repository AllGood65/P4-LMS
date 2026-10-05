/*
 * P4-LMS - SOURCE browser: state, navigation history, artwork, paging
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * LYRION SOURCE / LIBRARY BROWSER
 * ============================================================ */

/* Lists scroll: up to this many items are fetched at once, and PREV / NEXT
 * appear only for lists that are longer than that.  Only the rows on (or
 * near) the screen are actually drawn - see source_vl_update() - so a long
 * list costs no more display memory than a short one. */
#define SOURCE_MAX_ITEMS       200
#define SOURCE_QUERY_ITEMS     201
#define SOURCE_MAX_TRACKS      200   /* fetch all album tracks in one request */
/* A list grows as it is scrolled (200 more each time the end comes near),
 * up to this many; only very long lists ever need PREV / NEXT. */
#define SOURCE_ITEM_CAP        1000
#define SOURCE_BUTTON_W        425
#define SOURCE_GRID_X0          30    /* left edge of the two-column grid */
#define SOURCE_GRID_DX         (SOURCE_BUTTON_W + 20)

/* Entries shown on the SOURCE home screen.  Both are supported by the
 * browser code below; set to 0 to hide one. */
#define SOURCE_ROOT_SHOW_PLAYLISTS 1

/* Close the SOURCE browser after this long without a touch anywhere. */
#define SOURCE_IDLE_CLOSE_MS 60000

/* 1 = log one "SRCDIAG" line each time the source browser draws a new list
 * (object counts + free memory).  Used to tell leftover objects from stale
 * display pixels.  Set to 0 to silence. */
#define SRC_DIAG 1
#define SOURCE_ROOT_SHOW_RADIO     1

/* Big enough for a 200-track album or the radio plugin's menu. */
#define SOURCE_BROWSE_BUF      (160 * 1024)   /* PSRAM; room for 200 albums with their details */
#define SOURCE_APP_BUF         (384 * 1024)   /* app / radio menus (PSRAM) */
#define SOURCE_BUTTON_H        70
#define SOURCE_MENU_BUTTON_H    60
#define SOURCE_MENU_ROW_H       69      /* 5 rows fit above the paging row */
#define SOURCE_TRACK_BUTTON_H   44
#define SOURCE_TRACK_ROW_H      48
#define SOURCE_TRACK_LIST_X     280
#define SOURCE_TRACK_LIST_Y      82
#define SOURCE_TRACK_LIST_W      620
#define SOURCE_TRACK_LIST_H      358

typedef enum {
    SOURCE_ROOT = 0,
    SOURCE_PLAYLISTS,
    SOURCE_ARTISTS,
    SOURCE_ALBUMS,
    SOURCE_GENRES,
    SOURCE_FOLDERS,
    SOURCE_FAVORITES,
    SOURCE_FAVORITE_CONTENT,
    SOURCE_ARTIST_ALBUMS,
    SOURCE_GENRE_ALBUMS,
    SOURCE_FOLDER_CONTENT,
    SOURCE_ALBUM_TRACKS,
    SOURCE_LOAD_PLAYLIST,
    SOURCE_LOAD_ALBUM,
    SOURCE_LOAD_ARTIST,
    SOURCE_LOAD_GENRE,
    SOURCE_LOAD_FOLDER,
    SOURCE_LOAD_TRACK,
    SOURCE_LOAD_FAVORITE,
    SOURCE_LOAD_FAVORITE_URL,
    SOURCE_RADIO_MENU,
    SOURCE_RADIO_PLAY,
    SOURCE_QUEUE,           /* view: the player's current playlist        */
    SOURCE_QUEUE_JUMP,      /* id = playlist index to jump to             */
    SOURCE_QUEUE_REMOVE,    /* id = playlist index to remove              */
    SOURCE_QUEUE_CLEAR,     /* empty the whole queue                      */
    SOURCE_QUEUE_SAVE,      /* title = playlist name, id 1 = replace ok   */
    SOURCE_PLAYLIST_DELETE, /* id = saved playlist id                     */
    SOURCE_WIFI_SCAN,       /* Wi-Fi picker: list nearby networks          */
    SOURCE_WIFI_CONNECT,    /* title = SSID, item_id = password           */
    SOURCE_WIFI_FORGET,     /* back to the network built into the firmware */
    SOURCE_LMS_FIND,        /* look for Lyrion servers on this network     */
    SOURCE_LMS_USE,         /* title = "http://host:port" to use here      */
    SOURCE_SEARCH_PROMPT,   /* Library home tile: opens the search keyboard */
    SOURCE_SEARCH,          /* title = what to search for                  */
    SOURCE_QUEUE_MOVE,      /* id = from index, queue_op = to index        */
    SOURCE_ALARMS_LOAD,     /* Alarms screen (18c_alarms.c): list them     */
    SOURCE_ALARM_SAVE,      /* add / update alarm_pending                  */
    SOURCE_ALARM_DELETE,    /* item_id = alarm id                          */
    SOURCE_ALARM_TOGGLE,    /* item_id = alarm id, id = 1 on / 0 off       */
    SOURCE_ALARM_PLAYLISTS, /* saved playlists, for the alarm editor       */
    SOURCE_FAV_TOGGLE,      /* add / remove what's playing (np_fav_*)      */
    SOURCE_FAV_DELETE,      /* item_id = favorites id; queue_op = view to redraw, title = its parent */
    SOURCE_FAV_CHECK,       /* is what's playing a favorite? (sets np_fav)  */
    SOURCE_FAV_ADD_TRACK,   /* Library: id = track id                       */
    SOURCE_FAV_ADD_ALBUM,   /* Library: id = album id                       */
    SOURCE_SELECT_QUEUE,    /* SELECT: radio_action = "id,id,...", queue_op */
    SOURCE_SELECT_FAV,      /* SELECT: radio_action = "id,id,..."           */
    SOURCE_WALL_FETCH,      /* screensaver cover wall: id = covers wanted   */
    /* Saved playlists (18f_playlists.c) - keep these together, in this order. */
    SOURCE_PLAYLIST_TRACKS, /* view: id = playlist id, title = its name     */
    SOURCE_PL_PLAY_FROM,    /* id = playlist id, queue_op = song index      */
    SOURCE_PL_EDIT,         /* id = playlist id, queue_op = PL_EDIT_*, item_id = "i" or "from,to" */
    SOURCE_PL_RENAME,       /* id = playlist id, title = new name           */
    SOURCE_PL_LIST,         /* the "add to playlist" chooser's list         */
    SOURCE_PL_ADD,          /* id = playlist id, item_id = its name, queue_op = PL_KIND_*,
                               radio_action = url or "id,id,...", title = what, offset = count */
    SOURCE_PL_NEW_ADD       /* as SOURCE_PL_ADD, but item_id = a new playlist's name */
} source_mode_t;

#define PL_KIND_URL    0     /* one address (a stream, a YouTube video, a file) */
#define PL_KIND_TRACKS 1     /* library song ids, "12,13,14"                    */
#define PL_EDIT_DELETE 0
#define PL_EDIT_MOVE   1

/* What a SOURCE_LOAD_* request does with the player's queue. */
#define QUEUE_OP_PLAY    0   /* replace the queue and play (the old behaviour) */
#define QUEUE_OP_ADD     1   /* add to the end of the queue                    */
#define QUEUE_OP_INSERT  2   /* play next (insert after the current song)      */

typedef struct {
    source_mode_t mode;
    int id;
    int offset;
    char item_id[96];
    char radio_action[768];
    char title[SOURCE_TEXT_MAX];   /* name of the item being opened (folder name) */
    bool push_history;    /* remember the current view so BACK can return to it */
    bool append;          /* more of the list on screen (scrolled near its end) */
    int queue_op;         /* QUEUE_OP_* for SOURCE_LOAD_* requests */
} source_request_t;

/*
 * Navigation history.  Every time the user drills down (Artists -> an artist
 * -> an album, or folder -> subfolder -> subfolder) the view being left is
 * pushed here, so BACK returns to exactly where the user came from - same
 * list, same page - instead of jumping to a fixed parent.
 */
#define SOURCE_NAV_STACK_MAX 16

typedef struct {
    source_mode_t mode;
    int id;
    int offset;
    char item_id[96];
    char title[SOURCE_TEXT_MAX];
} source_nav_frame_t;

static source_nav_frame_t source_nav_stack[SOURCE_NAV_STACK_MAX];
static int source_nav_depth = 0;

static void source_nav_push(source_mode_t mode, int id, int offset,
                            const char *item_id, const char *title)
{
    /* When the stack is full, drop the oldest frame. */
    if (source_nav_depth >= SOURCE_NAV_STACK_MAX) {
        memmove(&source_nav_stack[0], &source_nav_stack[1],
                sizeof(source_nav_stack[0]) * (SOURCE_NAV_STACK_MAX - 1));
        source_nav_depth = SOURCE_NAV_STACK_MAX - 1;
    }

    source_nav_frame_t *frame = &source_nav_stack[source_nav_depth++];
    frame->mode = mode;
    frame->id = id;
    frame->offset = offset;
    snprintf(frame->item_id, sizeof(frame->item_id), "%s", item_id != NULL ? item_id : "");
    snprintf(frame->title, sizeof(frame->title), "%s", title != NULL ? title : "");
}

/* Name of the folder currently shown in SOURCE_FOLDER_CONTENT. */
static char source_folder_title[SOURCE_TEXT_MAX] = "";

typedef struct {
    int id;
    source_mode_t action;
    char title[SOURCE_TEXT_MAX];
    int artwork_track_id;
    char item_id[96];
    char radio_go_json[768];
    char radio_play_json[768];
    bool queued;          /* this song is already in the player's queue */
    char subtitle[64];    /* album tiles: the artist */
    char cover_key[24];   /* album tiles: the cover's id - a number or a code like "43f4745a" */
    char fav_id[24];      /* Favorites rows: the favorite's id, for deleting it */
    bool needs_input;     /* app / radio menus: a search that asks for text first */
} source_item_t;

static QueueHandle_t source_queue = NULL;
static lv_obj_t *source_overlay = NULL;
static lv_obj_t *source_track_list = NULL;
/* Scroll area for the two-column menus and the album-cover grid. */
static lv_obj_t *source_menu_list = NULL;
#define SOURCE_MENU_LIST_Y      80
#define SOURCE_MENU_LIST_H      (PANEL_H - 82 - SOURCE_MENU_LIST_Y)
static lv_obj_t *source_title_label = NULL;
static lv_obj_t *source_detail_album_label = NULL;
static lv_obj_t *source_detail_album_caption = NULL;
static lv_obj_t *source_detail_track_count = NULL;
static lv_obj_t *source_detail_fav_button = NULL;    /* album view: add the album to Favorites */

/* SELECT mode: tick several songs, then one action for all of them. */
#define SOURCE_SEL_MAX 100
static bool source_select_mode = false;
static int  source_sel_ids[SOURCE_SEL_MAX];
static int  source_sel_count = 0;
static int  source_sel_view_key = -1;             /* which list the ticks belong to */
static lv_obj_t *source_select_button = NULL;     /* "SELECT" in the bottom bar     */
static lv_obj_t *source_select_bar = NULL;        /* the bar shown while selecting  */
static lv_obj_t *source_select_count_label = NULL;
static lv_obj_t *source_back_button = NULL;
static lv_obj_t *source_close_button = NULL;
static lv_obj_t *source_empty_label = NULL;
static lv_obj_t **source_item_buttons = NULL;   /* SOURCE_ITEM_CAP, in PSRAM */
static volatile uint8_t *source_grid_want = NULL;   /* SOURCE_ITEM_CAP, in PSRAM */
static volatile bool     source_loading_more = false; /* an append request is on its way */
static int               source_vl_count = 0;          /* items the virtual list may draw */
static source_item_t *source_items = NULL;
static int source_item_count = 0;
static source_mode_t source_current_mode = SOURCE_ROOT;

/* ---- the Queue view: one song per row, 7 per page ---- */
#define SOURCE_QUEUE_PAGE      7
#define SOURCE_QUEUE_ROW_H     50
static int  source_queue_total   = 0;     /* songs in the queue                    */
static int  source_queue_current = -1;    /* index of the playing song             */
static int  source_queue_sel     = -1;    /* index whose move controls are open    */
static bool source_queue_radio = false;   /* the queue is just one live radio station */
static bool source_queue_keep_sel = false;/* a move: keep that song selected       */
static volatile bool source_queue_busy = false;  /* a move is on its way           */
/* Queue / playlist rows: each row's "..." button and its control strip
 * (SOURCE_ITEM_CAP each, in PSRAM; only rows on screen have them). */
static lv_obj_t **queue_row_more = NULL;
static lv_obj_t **queue_row_strip = NULL;
/* Redrawing the queue / a playlist that is on screen keeps its scroll
 * position (-1: a fresh view, which shows the playing song). */
static int source_list_keep_y = -1;
static int source_parent_id = 0;
static char source_parent_item_id[96] = "";
static char source_current_title[SOURCE_TEXT_MAX] = "SOURCE";
static int source_artwork_track_id = 0;
static int source_page_offset = 0;
static bool source_has_next = false;
static lv_obj_t *source_prev_button = NULL;
static lv_obj_t *source_next_button = NULL;
static lv_obj_t *source_page_label = NULL;
static lv_obj_t *source_nav_bar = NULL;
static lv_obj_t *source_play_context_button = NULL;
static lv_obj_t *source_save_button = NULL;          /* "SAVE" in the queue view   */
static lv_obj_t *source_queue_play_button = NULL;    /* "PLAY" in the queue view   */
static lv_obj_t *qs_backdrop = NULL;                 /* save-as-playlist panel      */
static lv_obj_t *qs_textarea = NULL;
static lv_obj_t *qs_status = NULL;
static lv_obj_t *qs_keyboard = NULL;
static char      qs_replace_name[SOURCE_TEXT_MAX] = "";   /* name confirmed for replace */
static lv_obj_t *qs_title_label = NULL;
static int       qs_mode = 0;              /* 0 save queue, 1 new playlist (then add), 2 rename */
/* What "add to playlist" is adding (set on the LVGL task before the chooser opens). */
typedef struct { int kind; int count; char payload[768]; char what[SOURCE_TEXT_MAX]; } pl_pending_t;
static pl_pending_t pl_pending;
static void source_render_playlist(void);             /* 18f_playlists.c */
static void qs_hide(void);
static char source_radio_parent_commands[6][768];
static int source_radio_depth = 0;
static char source_radio_current_action[768] = "";
/* The radio-style menu shows Lyrion's Apps (YouTube, ...) instead of Internet Radio. */
static bool source_menu_is_apps = false;
/* The search item waiting for the text typed on the keyboard. */
static char source_app_input_action[768] = "";


static lv_obj_t *source_art_image = NULL;
static uint8_t *source_art_pixels = NULL;
static lv_image_dsc_t source_art_dsc;

/* Which album / folder the picture on screen belongs to (0 = none).  Albums use
 * their id, folders the negative of theirs, so the two can never be confused. */
static int source_art_key_shown = 0;
static int source_art_key_wanted = 0;

/* Cover pictures beside the track list.  Set to 0 to turn off for albums or for
 * music folders (useful when tracking down a problem). */
#define SOURCE_ART_ALBUMS    1
#define SOURCE_ART_FOLDERS   1

#define SOURCE_ART_SIZE      200
#define SOURCE_ART_MAX_BYTES (1024 * 1024)

static void source_clear_art(void)
{
    /* Detach the image descriptor BEFORE freeing its pixel buffer.
     * Otherwise LVGL can retain a pointer to freed RGB565 memory and
     * render garbage later (including after returning to the screensaver). */
    if (source_art_image != NULL) {
        lv_image_set_src(source_art_image, NULL);
        lv_obj_add_flag(source_art_image, LV_OBJ_FLAG_HIDDEN);
    }

    if (source_art_pixels != NULL) {
        heap_caps_free(source_art_pixels);
        source_art_pixels = NULL;
    }

    memset(&source_art_dsc, 0, sizeof(source_art_dsc));
    source_art_key_shown = 0;
}




/* Download the cover for one track (Lyrion accepts a track id in the cover URL)
 * and show it beside the track list.  Runs in the browser task, after the list
 * itself is already on screen. */
typedef struct {
    uint8_t *pixels;
    uint32_t w, h, padded;
    size_t size;
    int key;
    bool shown;
} source_art_ctx_t;
static void source_art_show_job(void *arg);

/* Download the cover of `track_id` at px x px and decode it to RGB565.  On
 * success the caller owns *out_pixels. */
static bool source_download_cover(const char *cover_key, int px, uint8_t **out_pixels, uint32_t *out_w,
                                  uint32_t *out_h, uint32_t *out_padded, size_t *out_size)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/music/%s/cover_%dx%d.png", LYRION_SERVER_BASE, cover_key, px, px);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(SOURCE_ART_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) return false;

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 8000,
        .buffer_size = 4096, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        heap_caps_free(buf);
        return false;
    }
    size_t len = 0;
    bool ok = (esp_http_client_open(client, 0) == ESP_OK);
    if (ok) {
        if (esp_http_client_fetch_headers(client) > SOURCE_ART_MAX_BYTES) ok = false;
        while (ok) {
            const int n = esp_http_client_read(client, (char *)buf + len, SOURCE_ART_MAX_BYTES - len);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            len += (size_t)n;
            if (len >= SOURCE_ART_MAX_BYTES) { ok = false; break; }
        }
        if (ok && esp_http_client_get_status_code(client) != 200) ok = false;
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);

    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    ok = ok && len >= 8 && memcmp(buf, png_sig, 8) == 0 &&
         png_decode_rgb565_buffer(buf, len, out_pixels, out_w, out_h, out_padded, out_size);
    heap_caps_free(buf);
    return ok;
}

static bool source_fetch_art(int track_id, int key)
{
    if (track_id <= 0 || key == 0 || source_art_image == NULL) {
        return false;
    }

    char url[160];
    snprintf(url, sizeof(url), "%s/music/%d/cover_%dx%d.png",
             LYRION_SERVER_BASE, track_id, SOURCE_ART_SIZE, SOURCE_ART_SIZE);

    uint8_t *buf = (uint8_t *)heap_caps_malloc(SOURCE_ART_MAX_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        heap_caps_free(buf);
        return false;
    }

    size_t len = 0;
    bool ok = (esp_http_client_open(client, 0) == ESP_OK);
    if (ok) {
        int content_length = esp_http_client_fetch_headers(client);
        if (content_length > SOURCE_ART_MAX_BYTES) {
            ok = false;
        }
        while (ok) {
            int n = esp_http_client_read(client, (char *)buf + len, SOURCE_ART_MAX_BYTES - len);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            len += (size_t)n;
            if (len >= SOURCE_ART_MAX_BYTES) { ok = false; break; }
        }
        if (ok && esp_http_client_get_status_code(client) != 200) {
            ok = false;
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);

    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (!ok || len < 8 || memcmp(buf, png_sig, 8) != 0) {
        ESP_LOGW(TAG, "Source artwork: download failed or not a PNG (%u bytes)", (unsigned)len);
        heap_caps_free(buf);
        return false;
    }

    uint8_t *pixels = NULL;
    uint32_t w = 0, h = 0, padded = 0;
    size_t size = 0;
    ok = png_decode_rgb565_buffer(buf, len, &pixels, &w, &h, &padded, &size);
    heap_caps_free(buf);
    if (!ok) {
        return false;
    }

    source_art_ctx_t ctx = { .pixels = pixels, .w = w, .h = h, .padded = padded,
                             .size = size, .key = key, .shown = false };
    ui_call(source_art_show_job, &ctx);
    if (!ctx.shown) {
        heap_caps_free(pixels);                /* the user moved on meanwhile */
        return false;
    }

    ESP_LOGI(TAG, "Source artwork shown: %ux%u (track %d)", (unsigned)w, (unsigned)h, track_id);
    return true;
}

/* Runs on the LVGL task: show the cover beside the track list, unless the
 * user has moved on while it was downloading. */
static void source_art_show_job(void *arg)
{
    source_art_ctx_t *c = (source_art_ctx_t *)arg;
    uint8_t *pixels = c->pixels;
    const uint32_t w = c->w, h = c->h, padded = c->padded;
    const size_t size = c->size;
    const int key = c->key;

    if (source_art_key_wanted != key || source_overlay == NULL ||
        lv_obj_has_flag(source_overlay, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    source_clear_art();
    source_art_pixels = pixels;
    memset(&source_art_dsc, 0, sizeof(source_art_dsc));
    source_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    source_art_dsc.header.w = w;
    source_art_dsc.header.h = h;
    source_art_dsc.header.stride = padded * 2U;
    source_art_dsc.data_size = size;
    source_art_dsc.data = source_art_pixels;
    lv_image_set_src(source_art_image, &source_art_dsc);
    lv_obj_clear_flag(source_art_image, LV_OBJ_FLAG_HIDDEN);
    source_art_key_shown = key;
    c->shown = true;
}

static bool source_init_items(void)
{
    if (source_items != NULL) return true;

    source_items = (source_item_t *)heap_caps_calloc(
        SOURCE_ITEM_CAP,
        sizeof(source_item_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    source_item_buttons = (lv_obj_t **)heap_caps_calloc(SOURCE_ITEM_CAP, sizeof(lv_obj_t *),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    source_grid_want = (volatile uint8_t *)heap_caps_calloc(SOURCE_ITEM_CAP, 1,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    queue_row_more = (lv_obj_t **)heap_caps_calloc(SOURCE_ITEM_CAP, sizeof(lv_obj_t *),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    queue_row_strip = (lv_obj_t **)heap_caps_calloc(SOURCE_ITEM_CAP, sizeof(lv_obj_t *),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (source_items == NULL || source_item_buttons == NULL || source_grid_want == NULL ||
        queue_row_more == NULL || queue_row_strip == NULL) {
        ESP_LOGE(TAG,
                 "Could not allocate source item buffer in PSRAM (%u bytes)",
                 (unsigned)(SOURCE_ITEM_CAP * sizeof(source_item_t)));
        return false;
    }

    ESP_LOGI(TAG,
             "Source item buffer allocated in PSRAM: %u bytes",
             (unsigned)(SOURCE_ITEM_CAP * sizeof(source_item_t)));
    return true;
}


static const char *source_mode_title(source_mode_t mode)
{
    switch (mode) {
        case SOURCE_PLAYLISTS: return "PLAYLISTS";
        case SOURCE_ARTISTS: return "ARTISTS";
        case SOURCE_ALBUMS: return "ALBUMS";
        case SOURCE_GENRES: return "GENRES";
        case SOURCE_FOLDERS: return "MUSIC FOLDERS";
        case SOURCE_FAVORITES: return "FAVORITES";
        case SOURCE_FAVORITE_CONTENT: return "FAVORITES";
        case SOURCE_RADIO_MENU: return source_menu_is_apps ? "APPS" : "INTERNET RADIO";
        case SOURCE_RADIO_PLAY: return "INTERNET RADIO";
        case SOURCE_ARTIST_ALBUMS: return "ARTIST ALBUMS";
        case SOURCE_GENRE_ALBUMS: return "GENRE ALBUMS";
        case SOURCE_FOLDER_CONTENT: return "MUSIC FOLDER";
        case SOURCE_ALBUM_TRACKS: return "ALBUM TRACKS";
        case SOURCE_QUEUE: return "QUEUE";
        case SOURCE_PLAYLIST_TRACKS: return "PLAYLIST";
        case SOURCE_SEARCH: return "SEARCH";
        default: return "SOURCE";
    }
}

static void source_set_title(const char *title)
{
    if (source_title_label == NULL) return;
    lv_label_set_text(source_title_label, title != NULL ? title : "SOURCE");
}

/* ---- album cover grid (Albums, an artist's / a genre's albums) ---- */
#define SOURCE_GRID_COLS   5
#define SOURCE_GRID_COVER  136
/* Covers belong to their tile (freed when the tile is deleted).  want[i]:
 * 1 = tile i is on screen and needs its cover, 2 = no cover / failed. */
static volatile int      source_vl_first_index = 0;  /* first item drawn, for the cover fetcher */
static volatile uint32_t source_grid_gen = 0;      /* bumped on every redraw */

static bool source_grid_mode(void)
{
    return source_current_mode == SOURCE_ALBUMS ||
           source_current_mode == SOURCE_ARTIST_ALBUMS ||
           source_current_mode == SOURCE_GENRE_ALBUMS;
}

/* Virtual list state (15_queue_save_and_browser_query.c, source_vl_*). */
static struct {
    lv_obj_t *list;       /* the scroll area in use, NULL = not active */
    lv_obj_t *spacer;     /* sets the scrollable height                */
    int cols, row_h, y0, x0, dx, view_h;
    bool grid, detail, album_detail;
    int first, last;      /* items currently built: [first, last)      */
} source_vl;

static void source_clear_items(void)
{
    /* No virtual list until the next view starts one. */
    source_vl.list = NULL;
    source_vl.first = source_vl.last = 0;
    source_vl_count = 0;
    source_loading_more = false;
    if (source_vl.spacer != NULL) lv_obj_set_pos(source_vl.spacer, 0, 0);

    /* Only the rows in view exist, but any of the list's items may be one. */
    for (int i = 0; source_item_buttons != NULL && i < SOURCE_ITEM_CAP; i++) {
        if (source_item_buttons[i] != NULL) {
            lv_obj_del(source_item_buttons[i]);
            source_item_buttons[i] = NULL;
        }
    }
    /* (Deleting a tile frees its cover - see source_tile_delete_cb.) */
    if (source_grid_want != NULL) memset((void *)source_grid_want, 0, SOURCE_ITEM_CAP);
    source_grid_gen++;
    if (queue_row_more != NULL && queue_row_strip != NULL) {   /* children of the rows just deleted */
        memset(queue_row_more, 0, SOURCE_ITEM_CAP * sizeof(lv_obj_t *));
        memset(queue_row_strip, 0, SOURCE_ITEM_CAP * sizeof(lv_obj_t *));
    }
    source_queue_busy = false;                          /* the view is being redrawn */
    source_item_count = 0;
    source_clear_art();
}

/* Lists that grow as they are scrolled (fetched in pieces from Lyrion). */
static bool source_list_grows(void)
{
    switch (source_current_mode) {
        case SOURCE_ARTISTS: case SOURCE_ALBUMS: case SOURCE_GENRES: case SOURCE_FOLDERS:
        case SOURCE_FAVORITES: case SOURCE_ARTIST_ALBUMS: case SOURCE_GENRE_ALBUMS:
        case SOURCE_FOLDER_CONTENT: case SOURCE_FAVORITE_CONTENT: case SOURCE_PLAYLISTS:
        case SOURCE_RADIO_MENU:
            return true;
        default:
            return false;
    }
}

static void source_update_page_controls(void)
{
    bool is_track_view = (source_current_mode == SOURCE_ALBUM_TRACKS);

    /* SELECT: any list (not the queue) with at least one song in it. */
    if (source_select_button != NULL) {
        bool songs = false;
        for (int i = 0; i < source_item_count && !songs; i++) {
            songs = (source_items[i].action == SOURCE_LOAD_TRACK && source_items[i].id > 0);
        }
        if (songs && source_current_mode != SOURCE_QUEUE && !source_select_mode) {
            lv_obj_clear_flag(source_select_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(source_select_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (source_select_bar != NULL && source_select_mode) lv_obj_move_foreground(source_select_bar);

    /* In track view we scroll instead of page - hide the nav buttons.
     * Other lists scroll too; PREV / NEXT only show for very long lists. */
    /* A list that grows as it scrolls needs PREV / NEXT only past
     * SOURCE_ITEM_CAP items. */
    const bool grows_more = source_has_next && source_list_grows() &&
                            source_item_count < SOURCE_ITEM_CAP;
    const bool one_page = (source_page_offset <= 0 && (!source_has_next || grows_more));
    /* The queue and a playlist's songs are fetched whole and scroll. */
    const bool hide_paging = is_track_view || one_page ||
                             source_current_mode == SOURCE_QUEUE ||
                             source_current_mode == SOURCE_PLAYLIST_TRACKS;
    if (source_prev_button != NULL) {
        if (hide_paging) lv_obj_add_flag(source_prev_button, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_clear_flag(source_prev_button, LV_OBJ_FLAG_HIDDEN);
            if (source_page_offset > 0) lv_obj_clear_state(source_prev_button, LV_STATE_DISABLED);
            else lv_obj_add_state(source_prev_button, LV_STATE_DISABLED);
        }
    }
    if (source_next_button != NULL) {
        if (hide_paging) lv_obj_add_flag(source_next_button, LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_clear_flag(source_next_button, LV_OBJ_FLAG_HIDDEN);
            if (source_has_next) lv_obj_clear_state(source_next_button, LV_STATE_DISABLED);
            else lv_obj_add_state(source_next_button, LV_STATE_DISABLED);
        }
    }
    if (source_play_context_button != NULL) {
        const char *label = NULL;
        if (source_current_mode == SOURCE_FOLDER_CONTENT) {
            label = "PLAY FOLDER";
        } else if (source_current_mode == SOURCE_ARTIST_ALBUMS) {
            label = "PLAY ARTIST";
        } else if (source_current_mode == SOURCE_GENRE_ALBUMS) {
            label = "PLAY GENRE";
        } else if (source_current_mode == SOURCE_ALBUM_TRACKS) {
            label = "PLAY ALBUM";
        } else if (source_current_mode == SOURCE_QUEUE) {
            label = "CLEAR QUEUE";
        } else if (source_current_mode == SOURCE_PLAYLIST_TRACKS) {
            label = LV_SYMBOL_PLAY "  PLAY ALL";
        }

        {
            const bool queue_buttons = (source_current_mode == SOURCE_QUEUE && source_item_count > 0);
            const bool can_save = queue_buttons && !source_queue_radio;   /* no playlist of one station */
            const bool pl_view = (source_current_mode == SOURCE_PLAYLIST_TRACKS && source_parent_id > 0);
            if (source_queue_play_button != NULL) {
                if (queue_buttons) lv_obj_clear_flag(source_queue_play_button, LV_OBJ_FLAG_HIDDEN);
                else               lv_obj_add_flag(source_queue_play_button, LV_OBJ_FLAG_HIDDEN);
            }
            /* SAVE in the queue, RENAME in a playlist. */
            if (source_save_button != NULL) {
                lv_label_set_text(lv_obj_get_child(source_save_button, 0), pl_view ? "RENAME" : "SAVE");
                if (can_save || pl_view) lv_obj_clear_flag(source_save_button, LV_OBJ_FLAG_HIDDEN);
                else                          lv_obj_add_flag(source_save_button, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (label != NULL && (source_parent_id > 0 || source_current_mode == SOURCE_QUEUE) &&
            !(source_current_mode == SOURCE_PLAYLIST_TRACKS && source_item_count == 0)) {
            lv_label_set_text(lv_obj_get_child(source_play_context_button, 0), label);
            lv_obj_clear_flag(source_play_context_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(source_play_context_button, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (source_page_label != NULL) {
        if (is_track_view) {
            char text[48];
            snprintf(text, sizeof(text), "%d tracks", source_item_count);
            lv_label_set_text(source_page_label, text);
            lv_obj_clear_flag(source_page_label, LV_OBJ_FLAG_HIDDEN);
        } else if (source_current_mode == SOURCE_FOLDER_CONTENT) {
            int song_count = 0;
            for (int i = 0; i < source_item_count; i++) {
                if (source_items[i].action == SOURCE_LOAD_TRACK) song_count++;
            }
            char text[48];
            if (song_count > 0) {
                snprintf(text, sizeof(text), "%d songs", song_count);
            } else if (source_item_count > 0) {
                snprintf(text, sizeof(text), "Subfolders");
            } else {
                snprintf(text, sizeof(text), "Empty folder");
            }
            lv_label_set_text(source_page_label, text);
            lv_obj_clear_flag(source_page_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(source_page_label, LV_OBJ_FLAG_HIDDEN);
            char text[48];
            if (source_item_count > 0 && one_page) {
                snprintf(text, sizeof(text), "%d%s item%s", source_item_count,
                         grows_more ? "+" : "", source_item_count == 1 ? "" : "s");
            } else if (source_item_count > 0) {
                snprintf(text, sizeof(text), "Items %d-%d",
                         source_page_offset + 1,
                         source_page_offset + source_item_count);
            } else {
                snprintf(text, sizeof(text), "No items");
            }
            lv_label_set_text(source_page_label, text);
        }
    }
}

/* How far PREV / NEXT move: the Queue shows 7 per page, the rest 10. */
static int source_page_step(void)
{
    if (source_current_mode == SOURCE_QUEUE || source_current_mode == SOURCE_PLAYLIST_TRACKS) {
        return SOURCE_QUEUE_PAGE;
    }
    /* Grown lists: past everything that is loaded (or back as far). */
    return source_item_count > SOURCE_MAX_ITEMS ? source_item_count : SOURCE_MAX_ITEMS;
}

static void source_prev_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (source_queue == NULL || source_page_offset <= 0) return;
    source_request_t request = {
        .mode = source_current_mode,
        .id = 0,
        .offset = source_page_offset - source_page_step() > 0
                  ? source_page_offset - source_page_step() : 0,
        .push_history = false
    };
    if (source_current_mode == SOURCE_ARTIST_ALBUMS ||
        source_current_mode == SOURCE_GENRE_ALBUMS ||
        source_current_mode == SOURCE_FOLDER_CONTENT ||
        source_current_mode == SOURCE_FAVORITE_CONTENT ||
        source_current_mode == SOURCE_ALBUM_TRACKS ||
        source_current_mode == SOURCE_PLAYLIST_TRACKS) {
        request.id = source_parent_id;
    }
    snprintf(request.item_id, sizeof(request.item_id), "%s", source_parent_item_id);
    snprintf(request.title, sizeof(request.title), "%s", source_folder_title);
    if (source_current_mode == SOURCE_RADIO_MENU) {
        /* Radio submenus are paged locally; keep the action that produced them. */
        snprintf(request.radio_action, sizeof(request.radio_action), "%s",
                 source_radio_current_action);
    }
    xQueueSend(source_queue, &request, 0);
}

static void source_next_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (source_queue == NULL || !source_has_next) return;
    source_request_t request = {
        .mode = source_current_mode,
        .id = 0,
        .offset = source_page_offset + source_page_step(),
        .push_history = false
    };
    if (source_current_mode == SOURCE_ARTIST_ALBUMS ||
        source_current_mode == SOURCE_GENRE_ALBUMS ||
        source_current_mode == SOURCE_FOLDER_CONTENT ||
        source_current_mode == SOURCE_FAVORITE_CONTENT ||
        source_current_mode == SOURCE_ALBUM_TRACKS ||
        source_current_mode == SOURCE_PLAYLIST_TRACKS) {
        request.id = source_parent_id;
    }
    snprintf(request.item_id, sizeof(request.item_id), "%s", source_parent_item_id);
    snprintf(request.title, sizeof(request.title), "%s", source_folder_title);
    if (source_current_mode == SOURCE_RADIO_MENU) {
        /* Radio submenus are paged locally; keep the action that produced them. */
        snprintf(request.radio_action, sizeof(request.radio_action), "%s",
                 source_radio_current_action);
    }
    xQueueSend(source_queue, &request, 0);
}

static void source_hide(void)
{
    if (source_overlay != NULL) {
        qs_hide();
        source_grid_gen++;                         /* the cover fetcher can stop */
        lv_obj_add_flag(source_overlay, LV_OBJ_FLAG_HIDDEN);
        ui_schedule_repaint(lv_screen_active());
        lvgl_mem_log("SOURCE closed");
    }
}

