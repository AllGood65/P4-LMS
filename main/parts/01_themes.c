/*
 * P4-LMS - Colour themes: the theme table and UI_* colour names
 *
 * Part of main.c.  This file is #included by main.c (in order) and is not
 * compiled on its own, so it shares main.c's includes and static names.
 */

/* ============================================================
 * P4-LMS COLOUR THEMES
 * The whole interface reads its colours from the active theme.  The theme is
 * chosen in SETTINGS (gear button in the header), saved to flash and applied
 * at start-up, so choosing one restarts the display.  Add a theme by adding
 * an entry to the table below.
 * ============================================================ */
typedef struct {
    const char *name;
    uint32_t bg, panel, panel_alt, button;
    uint32_t accent, accent_glow, accent_bright, accent_dark;
    uint32_t text, text_soft, text_dim;
    uint32_t amber, orange, red;   /* amber = secondary colour (outlines, icons) */
    uint32_t tertiary;             /* third colour: volume fill, PLAYING chip     */
    uint32_t edge, edge_soft, on_accent, track;
    uint32_t deep, album_bg, placeholder, label_soft, label_dim;
    uint32_t slider_bg, value_text, divider, tick, scale_text;
    uint32_t date, conn_amber;
    uint32_t vis[6];             /* visualizer "THEME" palette, quiet to loud */
} ui_theme_t;

static const ui_theme_t ui_themes[] = {
    {
        .name = "AMBER",
        .bg = 0x0F1113, .panel = 0x191D20, .panel_alt = 0x181B1E, .button = 0x2B3034,
        .accent = 0xD89B3D, .accent_glow = 0xFFD27A, .accent_bright = 0xFFE2A3, .accent_dark = 0x6B4720,
        .text = 0xF0E6D2, .text_soft = 0xBDB5A8, .text_dim = 0x7D776E,
        .amber = 0xE3A63F, .orange = 0xF08A30, .red = 0xC65A45, .tertiary = 0xFFD27A,
        .edge = 0x5A4932, .edge_soft = 0x3A3226, .on_accent = 0x121416, .track = 0x302A22,
        .deep = 0x0A0B0D, .album_bg = 0x252A2E, .placeholder = 0x6E777D, .label_soft = 0xA69B8A, .label_dim = 0x8F887D,
        .slider_bg = 0x38332C, .value_text = 0xC7B486, .divider = 0x4A443A, .tick = 0xC7AA70, .scale_text = 0xB69A62,
        .date = 0xA88A55, .conn_amber = 0xC7A84B,
        .vis = { 0x8A5A22, 0xB97A2C, 0xD89B3D, 0xFFC55A, 0xF08A30, 0xE2513C },
    },
    {
        .name = "ICE",
        .bg = 0x0D1117, .panel = 0x151C25, .panel_alt = 0x121821, .button = 0x263242,
        .accent = 0x4DA3E8, .accent_glow = 0x8FD0FF, .accent_bright = 0xC8E8FF, .accent_dark = 0x1F4E7A,
        .text = 0xE4EEF8, .text_soft = 0xAEBBC9, .text_dim = 0x6F7D8C,
        .amber = 0x62B4F2, .orange = 0x5FD4E8, .red = 0xE05A5A, .tertiary = 0x8FD0FF,
        .edge = 0x2F4B6B, .edge_soft = 0x22344A, .on_accent = 0x0A1118, .track = 0x1E2A38,
        .deep = 0x080B10, .album_bg = 0x202A38, .placeholder = 0x6F7D8C, .label_soft = 0x95A2B1, .label_dim = 0x82909E,
        .slider_bg = 0x223245, .value_text = 0x7EAFD8, .divider = 0x28405A, .tick = 0x7EAFD8, .scale_text = 0x5E90BA,
        .date = 0x608EB5, .conn_amber = 0x6FABDD,
        .vis = { 0x1F4E7A, 0x2E78B8, 0x4DA3E8, 0x8FD0FF, 0x5FD4E8, 0xE05A5A },
    },
    {
        .name = "TEAL",
        .bg = 0x0A1214, .panel = 0x111C1F, .panel_alt = 0x0F181B, .button = 0x1F3236,
        .accent = 0x2BC4B4, .accent_glow = 0x7DF0E2, .accent_bright = 0xC2FFF8, .accent_dark = 0x146058,
        .text = 0xDDF2F0, .text_soft = 0xA0BCB8, .text_dim = 0x64807C,
        .amber = 0x3FD6C4, .orange = 0xE8C34A, .red = 0xE0604A, .tertiary = 0x7DF0E2,
        .edge = 0x2A5A55, .edge_soft = 0x1C3A37, .on_accent = 0x081112, .track = 0x162528,
        .deep = 0x060C0D, .album_bg = 0x1A2A2E, .placeholder = 0x64807C, .label_soft = 0x88A4A0, .label_dim = 0x76928E,
        .slider_bg = 0x1B3233, .value_text = 0x66C0B6, .divider = 0x234A46, .tick = 0x66C0B6, .scale_text = 0x48A298,
        .date = 0x4A9F95, .conn_amber = 0x54C1B5,
        .vis = { 0x146058, 0x1F9A8C, 0x2BC4B4, 0x7DF0E2, 0xE8C34A, 0xE0604A },
    },
    {
        .name = "PHOSPHOR",
        .bg = 0x090E0B, .panel = 0x111A14, .panel_alt = 0x0F1712, .button = 0x1F2D24,
        .accent = 0x3FD16A, .accent_glow = 0x8CFFAB, .accent_bright = 0xC9FFD8, .accent_dark = 0x1B5E32,
        .text = 0xDDF5E3, .text_soft = 0x9DBBA6, .text_dim = 0x5F7A68,
        .amber = 0x55E07E, .orange = 0xC8E04A, .red = 0xE0503C, .tertiary = 0x8CFFAB,
        .edge = 0x2C5A3A, .edge_soft = 0x1E3A27, .on_accent = 0x08110B, .track = 0x17241C,
        .deep = 0x050806, .album_bg = 0x1A261E, .placeholder = 0x5F7A68, .label_soft = 0x84A18D, .label_dim = 0x728E7B,
        .slider_bg = 0x1C3224, .value_text = 0x6EC688, .divider = 0x254A30, .tick = 0x6EC688, .scale_text = 0x4FA669,
        .date = 0x51A169, .conn_amber = 0x60C97F,
        .vis = { 0x1B5E32, 0x2A9B4E, 0x3FD16A, 0x8CFFAB, 0xC8E04A, 0xE0503C },
    },
    {
        .name = "SYNTH",
        .bg = 0x100A1A, .panel = 0x1A1228, .panel_alt = 0x160F22, .button = 0x2B2040,
        .accent = 0xE040C0, .accent_glow = 0xFF80E6, .accent_bright = 0xFFC2F5, .accent_dark = 0x6A1F66,
        .text = 0xF3E8FA, .text_soft = 0xBBA8CC, .text_dim = 0x7B6A8C,
        .amber = 0xFF5FD0, .orange = 0x4FD8FF, .red = 0xFF5A6E, .tertiary = 0xFF80E6,
        .edge = 0x5A2F6E, .edge_soft = 0x3A2148, .on_accent = 0x140A1C, .track = 0x261A36,
        .deep = 0x08050D, .album_bg = 0x251B38, .placeholder = 0x7B6A8C, .label_soft = 0xA18FB2, .label_dim = 0x8E7D9F,
        .slider_bg = 0x331F44, .value_text = 0xCE74C6, .divider = 0x4A285B, .tick = 0xCE74C6, .scale_text = 0xAE55A6,
        .date = 0xA857A3, .conn_amber = 0xD364C4,
        .vis = { 0x6A1F66, 0xA02E98, 0xE040C0, 0xFF80E6, 0x4FD8FF, 0xFF5A6E },
    },
    {
        .name = "MONO",
        .bg = 0x0C0C0C, .panel = 0x171717, .panel_alt = 0x141414, .button = 0x2A2A2A,
        .accent = 0xE6E6E6, .accent_glow = 0xFFFFFF, .accent_bright = 0xFFFFFF, .accent_dark = 0x555555,
        .text = 0xF2F2F2, .text_soft = 0xB8B8B8, .text_dim = 0x7A7A7A,
        .amber = 0xD0D0D0, .orange = 0xBFBFBF, .red = 0xD46A6A, .tertiary = 0xFFFFFF,
        .edge = 0x4A4A4A, .edge_soft = 0x2E2E2E, .on_accent = 0x101010, .track = 0x262626,
        .deep = 0x050505, .album_bg = 0x232323, .placeholder = 0x7A7A7A, .label_soft = 0x9F9F9F, .label_dim = 0x8D8D8D,
        .slider_bg = 0x2F2F2F, .value_text = 0xCFCFCF, .divider = 0x3C3C3C, .tick = 0xCFCFCF, .scale_text = 0xB0B0B0,
        .date = 0xABABAB, .conn_amber = 0xD6D6D6,
        .vis = { 0x555555, 0x888888, 0xE6E6E6, 0xFFFFFF, 0xBFBFBF, 0xD46A6A },
    },
    {
        .name = "NIGHT",
        .bg = 0x0A0505, .panel = 0x140A0A, .panel_alt = 0x120808, .button = 0x241212,
        .accent = 0xB03030, .accent_glow = 0xD04848, .accent_bright = 0xE07070, .accent_dark = 0x501818,
        .text = 0xC86060, .text_soft = 0x9A4A4A, .text_dim = 0x683434,
        .amber = 0xC03838, .orange = 0xD05030, .red = 0xE04040, .tertiary = 0xD04848,
        .edge = 0x4A1E1E, .edge_soft = 0x2E1414, .on_accent = 0x100404, .track = 0x1E0E0E,
        .deep = 0x050202, .album_bg = 0x1E0F0F, .placeholder = 0x683434, .label_soft = 0x864141, .label_dim = 0x773B3B,
        .slider_bg = 0x291212, .value_text = 0xA53D3D, .divider = 0x3C1919, .tick = 0xA53D3D, .scale_text = 0x8C3232,
        .date = 0x883232, .conn_amber = 0xA83939,
        .vis = { 0x501818, 0x7A2424, 0xB03030, 0xD04848, 0xD05030, 0xE04040 },
    },
    {
        .name = "VINTAGE",
        .bg = 0x17100B, .panel = 0x21170F, .panel_alt = 0x1D140D, .button = 0x3A2A1C,
        .accent = 0xD4A03A, .accent_glow = 0xF0C664, .accent_bright = 0xFFE5A8, .accent_dark = 0x6E4C1C,
        .text = 0xF2E5C9, .text_soft = 0xC2AD86, .text_dim = 0x8F7C5C,
        .amber = 0xC9622D, .orange = 0xC9622D, .red = 0xC24A3A, .tertiary = 0x8DAA6B,
        .edge = 0x6B4A2A, .edge_soft = 0x3D2A18, .on_accent = 0x1B120A, .track = 0x2E2015,
        .deep = 0x0E0906, .album_bg = 0x312317, .placeholder = 0x8F7C5C, .label_soft = 0xAE9975, .label_dim = 0x9E8B69,
        .slider_bg = 0x3D2A1A, .value_text = 0xCBA660, .divider = 0x543A21, .tick = 0xCBA660, .scale_text = 0xB28E4B,
        .date = 0xAE8C4D, .conn_amber = 0xCEA555,
        .vis = { 0x5B3A1B, 0x8C5A22, 0xD4A03A, 0xF0C664, 0xC9622D, 0xB23A2E },
    },
    {
        .name = "STEAMPUNK",
        .bg = 0x12100E, .panel = 0x1C1814, .panel_alt = 0x18140F, .button = 0x2E2620,
        .accent = 0xBFA24B, .accent_glow = 0xE6CB7A, .accent_bright = 0xF7E8B5, .accent_dark = 0x5E4E22,
        .text = 0xE6DCC2, .text_soft = 0xB0A487, .text_dim = 0x827759,
        .amber = 0xB86A3E, .orange = 0xB86A3E, .red = 0xB4453A, .tertiary = 0x52A798,
        .edge = 0x5A4A32, .edge_soft = 0x36301F, .on_accent = 0x15110C, .track = 0x2A231A,
        .deep = 0x0B0907, .album_bg = 0x28211C, .placeholder = 0x827759, .label_soft = 0x9E9275, .label_dim = 0x908467,
        .slider_bg = 0x362D20, .value_text = 0xB8A369, .divider = 0x483D28, .tick = 0xB8A369, .scale_text = 0xA08C52,
        .date = 0x9D8A53, .conn_amber = 0xBAA360,
        .vis = { 0x3E5E57, 0x52A798, 0xBFA24B, 0xE6CB7A, 0xB86A3E, 0xA8402F },
    },
    {
        .name = "ART DECO",
        .bg = 0x0B0D0F, .panel = 0x13171A, .panel_alt = 0x101417, .button = 0x1F2A2A,
        .accent = 0xD6B35A, .accent_glow = 0xF0D58C, .accent_bright = 0xFFF0C4, .accent_dark = 0x6F5B27,
        .text = 0xF5EFE0, .text_soft = 0xB9B3A2, .text_dim = 0x817E6E,
        .amber = 0x2FA87E, .orange = 0x2FA87E, .red = 0xC8503C, .tertiary = 0x5CD6A6,
        .edge = 0x1F6B54, .edge_soft = 0x163A32, .on_accent = 0x12100A, .track = 0x1A2222,
        .deep = 0x060809, .album_bg = 0x1B2324, .placeholder = 0x817E6E, .label_soft = 0xA39E8D, .label_dim = 0x928E7E,
        .slider_bg = 0x1B342E, .value_text = 0xC8B37E, .divider = 0x1A5243, .tick = 0xC8B37E, .scale_text = 0xAC9864,
        .date = 0xA79665, .conn_amber = 0xCCB373,
        .vis = { 0x14503F, 0x1F8C68, 0x2FA87E, 0xD6B35A, 0xF0D58C, 0xC8503C },
    },
    {
        .name = "ROYAL",
        .bg = 0x0A0F1E, .panel = 0x121A30, .panel_alt = 0x0F1629, .button = 0x1E2A4A,
        .accent = 0xE0B24E, .accent_glow = 0xFFD983, .accent_bright = 0xFFEDB8, .accent_dark = 0x6C5420,
        .text = 0xEEF1F8, .text_soft = 0xA9B3CB, .text_dim = 0x7080A0,
        .amber = 0x5C8DE0, .orange = 0x5C8DE0, .red = 0xD05252, .tertiary = 0x9DB8F0,
        .edge = 0x2C4172, .edge_soft = 0x1C2A4A, .on_accent = 0x0E1220, .track = 0x1A2440,
        .deep = 0x060914, .album_bg = 0x1A2441, .placeholder = 0x7080A0, .label_soft = 0x929FBA, .label_dim = 0x818FAD,
        .slider_bg = 0x1E2B4C, .value_text = 0xC4B28C, .divider = 0x24365E, .tick = 0xC4B28C, .scale_text = 0xA89977,
        .date = 0xA2967B, .conn_amber = 0xCDB27A,
        .vis = { 0x1E3A78, 0x3B66C4, 0x5C8DE0, 0xE0B24E, 0xFFD983, 0xD05252 },
    },
    {
        .name = "MODERN",
        .bg = 0x0D0E10, .panel = 0x16181C, .panel_alt = 0x131519, .button = 0x25292F,
        .accent = 0x3D8BFF, .accent_glow = 0x7FB4FF, .accent_bright = 0xBFD9FF, .accent_dark = 0x1E4A99,
        .text = 0xF1F4F8, .text_soft = 0xA7B0BC, .text_dim = 0x717A87,
        .amber = 0xFF6F61, .orange = 0xFF6F61, .red = 0xE5384F, .tertiary = 0x35D0BA,
        .edge = 0x333A45, .edge_soft = 0x22272E, .on_accent = 0x06101F, .track = 0x20242A,
        .deep = 0x060708, .album_bg = 0x202328, .placeholder = 0x717A87, .label_soft = 0x919AA7, .label_dim = 0x818A97,
        .slider_bg = 0x252A31, .value_text = 0x729EDE, .divider = 0x2A303A, .tick = 0x729EDE, .scale_text = 0x5782C3,
        .date = 0x5A82BD, .conn_amber = 0x6298E8,
        .vis = { 0x1E4A99, 0x3D8BFF, 0x35D0BA, 0xFFD166, 0xFF6F61, 0xE5384F },
    },
    {
        .name = "NORDIC",
        .bg = 0x0F1418, .panel = 0x171E24, .panel_alt = 0x141A20, .button = 0x27313A,
        .accent = 0x7FA9C9, .accent_glow = 0xA9CBE3, .accent_bright = 0xD5E6F2, .accent_dark = 0x36566E,
        .text = 0xE9EEF1, .text_soft = 0xAAB6BF, .text_dim = 0x74818B,
        .amber = 0xD8B98E, .orange = 0xD8B98E, .red = 0xD0685A, .tertiary = 0x8FB39A,
        .edge = 0x3B4B58, .edge_soft = 0x28333C, .on_accent = 0x0C1318, .track = 0x212A32,
        .deep = 0x080B0E, .album_bg = 0x212A32, .placeholder = 0x74818B, .label_soft = 0x94A1AA, .label_dim = 0x84919B,
        .slider_bg = 0x28323C, .value_text = 0x94B0C4, .divider = 0x323F4A, .tick = 0x94B0C4, .scale_text = 0x7A95AA,
        .date = 0x7993A7, .conn_amber = 0x8EAEC6,
        .vis = { 0x36566E, 0x5E8CAE, 0x7FA9C9, 0x8FB39A, 0xD8B98E, 0xD0685A },
    },
    {
        .name = "SUNSET",
        .bg = 0x140A14, .panel = 0x1F1120, .panel_alt = 0x1A0E1B, .button = 0x3A1F3B,
        .accent = 0xFF7A45, .accent_glow = 0xFFA36E, .accent_bright = 0xFFD0B0, .accent_dark = 0x7A3418,
        .text = 0xFCEEE4, .text_soft = 0xD0B0B8, .text_dim = 0x9A7688,
        .amber = 0xE8467C, .orange = 0xE8467C, .red = 0xE0405A, .tertiary = 0xFFC857,
        .edge = 0x6A2E52, .edge_soft = 0x421D36, .on_accent = 0x1C0C10, .track = 0x2B1628,
        .deep = 0x0B050B, .album_bg = 0x311A32, .placeholder = 0x9A7688, .label_soft = 0xBA99A5, .label_dim = 0xAA8796,
        .slider_bg = 0x3B1C32, .value_text = 0xE8957E, .divider = 0x562644, .tick = 0xE8957E, .scale_text = 0xCC7866,
        .date = 0xC7786A, .conn_amber = 0xEF8D6D,
        .vis = { 0x5A1F5E, 0xB0306F, 0xE8467C, 0xFF7A45, 0xFFC857, 0xFFF0B0 },
    },
    {
        .name = "FOREST",
        .bg = 0x0B100C, .panel = 0x141C15, .panel_alt = 0x111811, .button = 0x243026,
        .accent = 0x9CC25A, .accent_glow = 0xC2E585, .accent_bright = 0xE4F5C0, .accent_dark = 0x47601F,
        .text = 0xEDEBD8, .text_soft = 0xB0B49A, .text_dim = 0x788669,
        .amber = 0xC9834A, .orange = 0xC9834A, .red = 0xC5533C, .tertiary = 0xE6C86E,
        .edge = 0x3A5230, .edge_soft = 0x263820, .on_accent = 0x0B1208, .track = 0x1B251C,
        .deep = 0x060906, .album_bg = 0x1E2920, .placeholder = 0x788669, .label_soft = 0x9AA286, .label_dim = 0x899478,
        .slider_bg = 0x233021, .value_text = 0xA6BB7A, .divider = 0x304528, .tick = 0xA6BB7A, .scale_text = 0x8AA462,
        .date = 0x88A162, .conn_amber = 0xA3BD70,
        .vis = { 0x2F4A1F, 0x5E8A2E, 0x9CC25A, 0xE6C86E, 0xC9834A, 0xC5533C },
    },
    {
        .name = "ROSE GOLD",
        .bg = 0x121012, .panel = 0x1B1719, .panel_alt = 0x171315, .button = 0x2E2629,
        .accent = 0xD9A08C, .accent_glow = 0xF0C0B0, .accent_bright = 0xFFE2D8, .accent_dark = 0x7A4C40,
        .text = 0xF6ECE8, .text_soft = 0xC7B0AB, .text_dim = 0x917A76,
        .amber = 0xB47C96, .orange = 0xB47C96, .red = 0xD6566A, .tertiary = 0xE8D0A0,
        .edge = 0x5A4247, .edge_soft = 0x372A2D, .on_accent = 0x1A0F0E, .track = 0x262022,
        .deep = 0x090707, .album_bg = 0x272123, .placeholder = 0x917A76, .label_soft = 0xB19A96, .label_dim = 0xA18A86,
        .slider_bg = 0x33282B, .value_text = 0xD0A89C, .divider = 0x48363A, .tick = 0xD0A89C, .scale_text = 0xB58D81,
        .date = 0xB18B80, .conn_amber = 0xD3A697,
        .vis = { 0x6A4A55, 0xB47C96, 0xD9A08C, 0xF0C0B0, 0xE8D0A0, 0xD6566A },
    },
    {
        .name = "RECEIVER BLUE",
        .bg = 0x070D16, .panel = 0x0E1826, .panel_alt = 0x0C1522, .button = 0x1A2B40,
        .accent = 0x3FA9F5, .accent_glow = 0x8FD3FF, .accent_bright = 0xCDEBFF, .accent_dark = 0x17507E,
        .text = 0xEAF4FF, .text_soft = 0xA8BDD4, .text_dim = 0x6C8199,
        .amber = 0xF2A93B, .orange = 0xF2A93B, .red = 0xE0554A, .tertiary = 0xF2C46B,
        .edge = 0x284A6E, .edge_soft = 0x1A304A, .on_accent = 0x06101C, .track = 0x16263A,
        .deep = 0x04080E, .album_bg = 0x162437, .placeholder = 0x6C8199, .label_soft = 0x90A5BC, .label_dim = 0x7E93AB,
        .slider_bg = 0x1A2F47, .value_text = 0x74B3E4, .divider = 0x213D5C, .tick = 0x74B3E4, .scale_text = 0x5695C7,
        .date = 0x5893C2, .conn_amber = 0x64B0E9,
        .vis = { 0x17507E, 0x2A7CC0, 0x3FA9F5, 0x8FD3FF, 0xF2C46B, 0xF2A93B },
    },
    {
        .name = "BLUE METERS",
        .bg = 0x050608, .panel = 0x0C0E12, .panel_alt = 0x0A0C10, .button = 0x1A1E26,
        .accent = 0x2F8CFF, .accent_glow = 0x7DBBFF, .accent_bright = 0xC4E0FF, .accent_dark = 0x163F80,
        .text = 0xE8EEF5, .text_soft = 0xA3ADBA, .text_dim = 0x6B7480,
        .amber = 0x3BD16F, .orange = 0x3BD16F, .red = 0xE04848, .tertiary = 0x7DE39C,
        .edge = 0x243650, .edge_soft = 0x161E2C, .on_accent = 0x040A14, .track = 0x151A22,
        .deep = 0x030405, .album_bg = 0x15181F, .placeholder = 0x6B7480, .label_soft = 0x8D96A3, .label_dim = 0x7C8591,
        .slider_bg = 0x19212E, .value_text = 0x699CDC, .divider = 0x1D2A3E, .tick = 0x699CDC, .scale_text = 0x4D80C0,
        .date = 0x507FB9, .conn_amber = 0x5898E7,
        .vis = { 0x163F80, 0x2462C8, 0x2F8CFF, 0x7DBBFF, 0x3BD16F, 0xE04848 },
    },
    {
        .name = "PATINA",
        .bg = 0x0D1211, .panel = 0x151D1B, .panel_alt = 0x121917, .button = 0x243230,
        .accent = 0xC47A45, .accent_glow = 0xE6A571, .accent_bright = 0xFFD8B5, .accent_dark = 0x6A3C1E,
        .text = 0xEEE6DA, .text_soft = 0xB3AE9F, .text_dim = 0x7C7A6E,
        .amber = 0x3FA595, .orange = 0x3FA595, .red = 0xC8503C, .tertiary = 0x7FD1C0,
        .edge = 0x2E5A52, .edge_soft = 0x1E3A35, .on_accent = 0x140C06, .track = 0x1C2624,
        .deep = 0x080C0B, .album_bg = 0x1F2B29, .placeholder = 0x7C7A6E, .label_soft = 0x9D998B, .label_dim = 0x8C8A7D,
        .slider_bg = 0x203330, .value_text = 0xBC9472, .divider = 0x264A44, .tick = 0xBC9472, .scale_text = 0xA07A5A,
        .date = 0x9C7A5C, .conn_amber = 0xBE8C64,
        .vis = { 0x1F5E55, 0x3FA595, 0x7FD1C0, 0xC47A45, 0xE6A571, 0xC8503C },
    },
    {
        .name = "COCOA",
        .bg = 0x140E0B, .panel = 0x1E1612, .panel_alt = 0x1A130F, .button = 0x35271F,
        .accent = 0xE8D8BC, .accent_glow = 0xFFF1D8, .accent_bright = 0xFFFAEE, .accent_dark = 0x7A6450,
        .text = 0xF5ECDD, .text_soft = 0xC4B39E, .text_dim = 0x8C7B69,
        .amber = 0xC98A4B, .orange = 0xC98A4B, .red = 0xC4553F, .tertiary = 0xD9A066,
        .edge = 0x5A4332, .edge_soft = 0x3A2B20, .on_accent = 0x1E140E, .track = 0x2C211A,
        .deep = 0x0B0806, .album_bg = 0x2D211A, .placeholder = 0x8C7B69, .label_soft = 0xAE9D89, .label_dim = 0x9D8C79,
        .slider_bg = 0x382A20, .value_text = 0xD6C6AD, .divider = 0x4A3729, .tick = 0xD6C6AD, .scale_text = 0xBAAA92,
        .date = 0xB5A58E, .conn_amber = 0xDBCBB2,
        .vis = { 0x5A3A24, 0x8C5A34, 0xC98A4B, 0xD9A066, 0xE8D8BC, 0xFFF1D8 },
    },
    {
        .name = "CYBER",
        .bg = 0x0A0A0D, .panel = 0x131318, .panel_alt = 0x101015, .button = 0x22222C,
        .accent = 0xFFE600, .accent_glow = 0xFFF27A, .accent_bright = 0xFFFBC8, .accent_dark = 0x7A6E00,
        .text = 0xF4F4F8, .text_soft = 0xB0B0BE, .text_dim = 0x74748A,
        .amber = 0x00E5FF, .orange = 0x00E5FF, .red = 0xFF3355, .tertiary = 0xFF2BD6,
        .edge = 0x3A3A55, .edge_soft = 0x24243A, .on_accent = 0x0E0E00, .track = 0x1E1E28,
        .deep = 0x050507, .album_bg = 0x1D1D25, .placeholder = 0x74748A, .label_soft = 0x9898A9, .label_dim = 0x86869A,
        .slider_bg = 0x252533, .value_text = 0xD8CB5F, .divider = 0x2F2F48, .tick = 0xD8CB5F, .scale_text = 0xBAAD45,
        .date = 0xB3A74C, .conn_amber = 0xE3D342,
        .vis = { 0x6A00B0, 0xFF2BD6, 0x00E5FF, 0xFFE600, 0xFFF27A, 0xFF3355 },
    },
};
#define UI_THEME_COUNT ((int)(sizeof(ui_themes) / sizeof(ui_themes[0])))
static const ui_theme_t *ui_theme = &ui_themes[0];

#define UI_BG            (ui_theme->bg)
#define UI_PANEL         (ui_theme->panel)
#define UI_PANEL_ALT     (ui_theme->panel_alt)
#define UI_BUTTON        (ui_theme->button)
#define UI_ACCENT        (ui_theme->accent)
#define UI_ACCENT_GLOW   (ui_theme->accent_glow)
#define UI_ACCENT_BRIGHT (ui_theme->accent_bright)
#define UI_ACCENT_DARK   (ui_theme->accent_dark)
#define UI_TEXT          (ui_theme->text)
#define UI_TEXT_SOFT     (ui_theme->text_soft)
#define UI_TEXT_DIM      (ui_theme->text_dim)
#define UI_AMBER         (ui_theme->amber)
#define UI_ORANGE        (ui_theme->orange)
#define UI_RED           (ui_theme->red)
#define UI_TERTIARY      (ui_theme->tertiary)
#define UI_EDGE          (ui_theme->edge)        /* panel outlines                   */
#define UI_EDGE_SOFT     (ui_theme->edge_soft)   /* hairline dividers                */
#define UI_ON_ACCENT     (ui_theme->on_accent)   /* dark text drawn on accent fills  */
#define UI_TRACK         (ui_theme->track)       /* empty part of bars and sliders   */
#define UI_DEEP          (ui_theme->deep)
#define UI_ALBUM_BG      (ui_theme->album_bg)
#define UI_PLACEHOLDER   (ui_theme->placeholder)
#define UI_LABEL_SOFT    (ui_theme->label_soft)
#define UI_LABEL_DIM     (ui_theme->label_dim)
#define UI_SLIDER_BG     (ui_theme->slider_bg)
#define UI_VALUE_TEXT    (ui_theme->value_text)
#define UI_DIVIDER       (ui_theme->divider)
#define UI_TICK          (ui_theme->tick)
#define UI_SCALE_TEXT    (ui_theme->scale_text)
#define UI_DATE          (ui_theme->date)
#define UI_CONN_AMBER    (ui_theme->conn_amber)


