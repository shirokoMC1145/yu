// EH 漫画阅读器 (New 3DS homebrew)
// 读取 sdmc:/eh/<漫画文件夹>/<序号文件名>.(webp|jpg|png)
// 上屏阅读(旋转成横屏全屏)，下屏选择漫画，中文界面(使用系统简体中文字体)

#include <3ds.h>
#include <citro2d.h>
#include <webp/decode.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define ROOT     "sdmc:/eh"
#define COLS     3
#define ROWS     2
#define PER_PAGE (COLS * ROWS)
#define CELL_W   106
#define CELL_H   98
#define GRID_Y   22
#define COV_W    70
#define COV_H    78

#define COL_BG   C2D_Color32(28, 30, 38, 255)
#define COL_BAR  C2D_Color32(44, 48, 62, 255)
#define COL_CELL C2D_Color32(52, 56, 72, 255)
#define COL_BTN  C2D_Color32(70, 76, 98, 255)
#define COL_ACC  C2D_Color32(255, 140, 60, 255)
#define COL_TXT  C2D_Color32(240, 240, 245, 255)
#define COL_DIM  C2D_Color32(150, 155, 170, 255)

// ---------- 类型 ----------
typedef struct { u8 *px; int w, h; } Img;

typedef struct {
    char *name;
    char label[160];
    bool label_ok;
    int cover;                 // 0 未加载 1 已加载 2 失败
    C3D_Tex tex;
    Tex3DS_SubTexture st;
    C2D_Image img;
} Manga;

enum { ST_LIST, ST_READ };

// ---------- 全局 ----------
static C2D_Font font;
static C2D_TextBuf dynbuf, measurebuf;

static Manga *mangas;
static int nmangas, sel;

static char **pg;               // 当前漫画的页文件名(已按序号排序)
static int npg, cur;
static char read_title[160];

static C3D_Tex pagetex;
static Tex3DS_SubTexture pagest;
static C2D_Image pageimg;
static bool page_loaded, page_ok, page_rot;
static bool page_dirty, loading_shown;
static bool stretch = true;     // true=铺满全屏(拉伸)  false=保持比例
static bool flip180 = false;    // 旋转方向取反

static const char *toast;
static int toast_t;

// ---------- 工具 ----------
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long x = strtoul(a, &ea, 10), y = strtoul(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea; b = eb;
        } else {
            int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
            if (c) return c;
            a++; b++;
        }
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
static int cmp_manga(const void *a, const void *b) { return natcmp(((const Manga *)a)->name, ((const Manga *)b)->name); }
static int cmp_str(const void *a, const void *b) { return natcmp(*(char *const *)a, *(char *const *)b); }

static bool is_img(const char *n)
{
    const char *e = strrchr(n, '.');
    if (!e) return false;
    return !strcasecmp(e, ".webp") || !strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".png");
}

static u8 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    u8 *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *size = n;
    return b;
}

// ---------- 图片解码 + 压缩 ----------
static void box_resize(const u8 *src, int sw, int sh, u8 *dst, int dw, int dh)
{
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((long long)dy * sh / dh), y1 = (int)((long long)(dy + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int x0 = (int)((long long)dx * sw / dw), x1 = (int)((long long)(dx + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            u32 r = 0, g = 0, b = 0, a = 0, cnt = (u32)((x1 - x0) * (y1 - y0));
            for (int y = y0; y < y1; y++) {
                const u8 *p = src + ((size_t)y * sw + x0) * 4;
                for (int x = x0; x < x1; x++, p += 4) { r += p[0]; g += p[1]; b += p[2]; a += p[3]; }
            }
            u8 *q = dst + ((size_t)dy * dw + dx) * 4;
            q[0] = r / cnt; q[1] = g / cnt; q[2] = b / cnt; q[3] = a / cnt;
        }
    }
}

static void fit_dims(int w, int h, int bw, int bh, int *ow, int *oh)
{
    float s = (float)bw / w, s2 = (float)bh / h;
    if (s2 < s) s = s2;
    int nw = (int)(w * s + 0.5f), nh = (int)(h * s + 0.5f);
    *ow = nw < 1 ? 1 : nw;
    *oh = nh < 1 ? 1 : nh;
}

// page_mode=true : 竖图 -> 缩进 240x400(之后旋转90°显示)；横图 -> 缩进 400x240
// page_mode=false: 封面缩略图 COV_W x COV_H
static bool load_image(const char *path, bool page_mode, Img *out, bool *rot)
{
    size_t size;
    u8 *buf = read_file(path, &size);
    if (!buf) return false;

    int w = 0, h = 0;
    bool is_webp = size > 12 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4);
    WebPDecoderConfig cfg;
    if (is_webp) {
        if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(buf, size, &cfg.input) != VP8_STATUS_OK) { free(buf); return false; }
        w = cfg.input.width; h = cfg.input.height;
    } else if (!stbi_info_from_memory(buf, (int)size, &w, &h, NULL)) { free(buf); return false; }
    if (w < 1 || h < 1) { free(buf); return false; }

    int bw, bh;
    *rot = false;
    if (page_mode) {
        if (w <= h) { *rot = true; bw = 240; bh = 400; }
        else        { bw = 400; bh = 240; }
    } else { bw = COV_W; bh = COV_H; }

    int nw, nh;
    fit_dims(w, h, bw, bh, &nw, &nh);
    u8 *px = malloc((size_t)nw * nh * 4);
    if (!px) { free(buf); return false; }

    bool ok = false;
    if (is_webp) {   // libwebp 解码时直接缩放，又快又省内存
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = nw;
        cfg.options.scaled_height = nh;
        cfg.output.colorspace = MODE_RGBA;
        cfg.output.is_external_memory = 1;
        cfg.output.u.RGBA.rgba = px;
        cfg.output.u.RGBA.stride = nw * 4;
        cfg.output.u.RGBA.size = (size_t)nw * nh * 4;
        ok = WebPDecode(buf, size, &cfg) == VP8_STATUS_OK;
    } else {
        int cw, ch, comp;
        u8 *raw = stbi_load_from_memory(buf, (int)size, &cw, &ch, &comp, 4);
        if (raw) { box_resize(raw, cw, ch, px, nw, nh); stbi_image_free(raw); ok = true; }
    }
    free(buf);
    if (!ok) { free(px); return false; }
    out->px = px; out->w = nw; out->h = nh;
    return true;
}

// ---------- 纹理 ----------
static int next_pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static bool make_tex(C3D_Tex *tex, Tex3DS_SubTexture *st, C2D_Image *img, const Img *im)
{
    int tw = next_pow2(im->w), th = next_pow2(im->h);
    if (!C3D_TexInit(tex, tw, th, GPU_RGBA8)) return false;
    u32 *dst = (u32 *)tex->data;
    const u32 *src = (const u32 *)im->px;
    memset(dst, 0, (size_t)tw * th * 4);
    for (int y = 0; y < im->h; y++) {
        int my = th - 1 - y;    // 3DS 纹理自下而上存放
        for (int x = 0; x < im->w; x++) {
            u32 o = (((my >> 3) * (tw >> 3) + (x >> 3)) << 6) +
                    ((x & 1) | ((my & 1) << 1) | ((x & 2) << 1) | ((my & 2) << 2) | ((x & 4) << 2) | ((my & 4) << 3));
            dst[o] = __builtin_bswap32(src[(size_t)y * im->w + x]);
        }
    }
    GSPGPU_FlushDataCache(tex->data, (u32)tw * th * 4);
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    st->width = im->w; st->height = im->h;
    st->left = 0.0f; st->top = 1.0f;
    st->right = (float)im->w / tw; st->bottom = 1.0f - (float)im->h / th;
    img->tex = tex; img->subtex = st;
    return true;
}

static void free_page_tex(void)
{
    if (page_loaded) { C3D_TexDelete(&pagetex); page_loaded = false; }
    page_ok = false;
}

// ---------- 文字 ----------
static float text_w(const char *s, float sc)
{
    C2D_Text t; float w, h;
    C2D_TextBufClear(measurebuf);
    C2D_TextFontParse(&t, font, measurebuf, s);
    C2D_TextGetDimensions(&t, sc, sc, &w, &h);
    return w;
}

static void fit_text(const char *src, char *dst, size_t dn, float sc, float maxw)   // dn >= 160
{
    char tmp[200];
    snprintf(dst, dn, "%s", src);
    if (text_w(dst, sc) <= maxw) return;
    for (;;) {
        size_t n = strlen(dst);
        if (n <= 1) break;
        size_t i = n - 1;
        while (i > 0 && ((unsigned char)dst[i] & 0xC0) == 0x80) i--;
        dst[i] = 0;
        snprintf(tmp, sizeof tmp, "%s..", dst);
        if (text_w(tmp, sc) <= maxw) break;
    }
    snprintf(tmp, sizeof tmp, "%s..", dst);
    snprintf(dst, dn, "%s", tmp);
}

static void draw_text(const char *s, float x, float y, float sc, u32 clr, u32 flags)
{
    C2D_Text t;
    C2D_TextFontParse(&t, font, dynbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, flags | C2D_WithColor, x, y, 0.5f, sc, sc, clr);
}

static void draw_btn(float x, float y, float w, float h, const char *s)
{
    C2D_DrawRectSolid(x, y, 0.4f, w, h, COL_BTN);
    draw_text(s, x + w / 2, y + (h - 17) / 2, 0.55f, COL_TXT, C2D_AlignCenter);
}

static bool hit(const touchPosition *t, float x, float y, float w, float h)
{
    return t->px >= x && t->px < x + w && t->py >= y && t->py < y + h;
}

// ---------- 漫画目录 ----------
static void scan_mangas(void)
{
    DIR *d = opendir(ROOT);
    if (!d) return;
    struct dirent *e;
    int cap = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || e->d_type != DT_DIR) continue;
        if (nmangas == cap) { cap = cap ? cap * 2 : 64; mangas = realloc(mangas, cap * sizeof(Manga)); }
        Manga *m = &mangas[nmangas++];
        memset(m, 0, sizeof *m);
        m->name = strdup(e->d_name);
    }
    closedir(d);
    if (nmangas) qsort(mangas, nmangas, sizeof(Manga), cmp_manga);
}

static bool first_image(const char *mname, char *out, size_t n)
{
    char dir[600];
    snprintf(dir, sizeof dir, ROOT "/%s", mname);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR || e->d_name[0] == '.' || !is_img(e->d_name)) continue;
        if (!found || natcmp(e->d_name, out) < 0) { snprintf(out, n, "%s", e->d_name); found = true; }
    }
    closedir(d);
    return found;
}

static void load_cover(Manga *m)
{
    char fn[300], path[900];
    m->cover = 2;
    if (!first_image(m->name, fn, sizeof fn)) return;
    snprintf(path, sizeof path, ROOT "/%s/%s", m->name, fn);
    Img im; bool rot;
    if (!load_image(path, false, &im, &rot)) return;
    if (make_tex(&m->tex, &m->st, &m->img, &im)) m->cover = 1;
    free(im.px);
}

static void free_pages(void)
{
    for (int i = 0; i < npg; i++) free(pg[i]);
    free(pg); pg = NULL; npg = 0;
}

static bool list_pages(const char *mname)
{
    char dir[600];
    snprintf(dir, sizeof dir, ROOT "/%s", mname);
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    int cap = 0;
    while ((e = readdir(d))) {
        if (e->d_type == DT_DIR || e->d_name[0] == '.' || !is_img(e->d_name)) continue;
        if (npg == cap) { cap = cap ? cap * 2 : 64; pg = realloc(pg, cap * sizeof(char *)); }
        pg[npg++] = strdup(e->d_name);
    }
    closedir(d);
    if (npg) qsort(pg, npg, sizeof(char *), cmp_str);
    return npg > 0;
}

static bool open_manga(int idx)
{
    free_pages();
    if (!list_pages(mangas[idx].name)) { toast = "该文件夹里没有图片"; toast_t = 120; return false; }
    fit_text(mangas[idx].name, read_title, sizeof read_title, 0.5f, 300);
    cur = 0; page_dirty = true; loading_shown = false;
    return true;
}

static void goto_page(int p)
{
    if (p < 0) p = 0;
    if (p >= npg) p = npg - 1;
    if (p == cur) return;
    cur = p; page_dirty = true; loading_shown = false;
}

// ---------- 主程序 ----------
int main(void)
{
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    osSetSpeedupEnable(true);   // New 3DS 804MHz

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    font = C2D_FontLoadSystem(CFG_REGION_CHN);   // 系统简体中文字体，失败则用默认字体
    dynbuf = C2D_TextBufNew(4096);
    measurebuf = C2D_TextBufNew(512);

    scan_mangas();

    int state = ST_LIST;
    int top_for = -1;
    char top_label[160] = "";
    bool dragging = false;
    int drag_page = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown(), kHeld = hidKeysHeld(), kUp = hidKeysUp();
        touchPosition tp;
        hidTouchRead(&tp);
        if (kDown & KEY_START) break;

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);   // 此时上一帧 GPU 已完成，可以安全增删纹理
        C2D_TextBufClear(dynbuf);

        // ===== 逻辑 =====
        int gpage = nmangas ? sel / PER_PAGE : 0;
        int gpages = nmangas ? (nmangas + PER_PAGE - 1) / PER_PAGE : 1;

        if (state == ST_LIST) {
            if (nmangas) {
                if ((kDown & KEY_RIGHT) && sel + 1 < nmangas) sel++;
                if ((kDown & KEY_LEFT) && sel > 0) sel--;
                if ((kDown & KEY_DOWN) && sel + COLS < nmangas) sel += COLS;
                if ((kDown & KEY_UP) && sel - COLS >= 0) sel -= COLS;
                if ((kDown & KEY_R) && gpage + 1 < gpages) sel = (gpage + 1) * PER_PAGE;
                if ((kDown & KEY_L) && gpage > 0) sel = (gpage - 1) * PER_PAGE;
                bool open = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH) {
                    for (int k = 0; k < PER_PAGE; k++) {
                        int idx = gpage * PER_PAGE + k;
                        if (idx >= nmangas) break;
                        if (hit(&tp, (k % COLS) * CELL_W + 1, GRID_Y + (k / COLS) * CELL_H, CELL_W - 2, CELL_H)) { sel = idx; open = true; }
                    }
                }
                if (open && open_manga(sel)) state = ST_READ;
                gpage = sel / PER_PAGE;
            }
            // 懒加载封面(每帧最多一张)，并释放不在当前页的封面
            int loaded = 0;
            for (int i = 0; i < nmangas; i++) {
                Manga *m = &mangas[i];
                bool vis = nmangas && (i / PER_PAGE == gpage);
                if (!vis && m->cover == 1) { C3D_TexDelete(&m->tex); m->cover = 0; }
                else if (vis && m->cover == 0 && !loaded) { load_cover(m); loaded = 1; }
            }
        } else {   // ST_READ
            if (kDown & (KEY_DOWN | KEY_A)) goto_page(cur + 1);
            if (kDown & KEY_UP) goto_page(cur - 1);
            if (kDown & KEY_R) goto_page(cur + 10);
            if (kDown & KEY_L) goto_page(cur - 10);
            if (kDown & KEY_Y) stretch = !stretch;
            if (kDown & KEY_X) flip180 = !flip180;
            bool back = (kDown & KEY_B) != 0;
            if (kDown & KEY_TOUCH) {
                if (hit(&tp, 8, 190, 96, 34)) back = true;
                if (hit(&tp, 112, 190, 96, 34)) stretch = !stretch;
                if (hit(&tp, 216, 190, 96, 34)) flip180 = !flip180;
            }
            if (kHeld & KEY_TOUCH) {
                if (hit(&tp, 10, 130, 300, 40)) {
                    dragging = true;
                    int p = (tp.px - 20) * npg / 280;
                    drag_page = p < 0 ? 0 : (p >= npg ? npg - 1 : p);
                }
            }
            if ((kUp & KEY_TOUCH) && dragging) { dragging = false; goto_page(drag_page); }
            if (back) {
                free_page_tex(); free_pages(); page_dirty = false; dragging = false; state = ST_LIST;
            }
            if (state == ST_READ && page_dirty) {
                if (!loading_shown) {
                    loading_shown = true;       // 先画一帧“加载中”，下一帧再真正解码
                } else {
                    free_page_tex();
                    char path[900];
                    snprintf(path, sizeof path, ROOT "/%s/%s", mangas[sel].name, pg[cur]);
                    Img im; bool rot = false;
                    if (load_image(path, true, &im, &rot)) {
                        if (make_tex(&pagetex, &pagest, &pageimg, &im)) { page_loaded = true; page_ok = true; page_rot = rot; }
                        free(im.px);
                    }
                    page_dirty = false; loading_shown = false;
                }
            }
        }

        // ===== 上屏 =====
        C2D_TargetClear(top, state == ST_READ ? C2D_Color32(0, 0, 0, 255) : COL_BG);
        C2D_SceneBegin(top);
        if (state == ST_READ) {
            if (page_ok) {
                float w = pagest.width, h = pagest.height, sx = 1, sy = 1, ang;
                if (page_rot) {
                    ang = C3D_AngleFromDegrees(flip180 ? -90.0f : 90.0f);
                    if (stretch) { sx = 240.0f / w; sy = 400.0f / h; }
                } else {
                    ang = C3D_AngleFromDegrees(flip180 ? 180.0f : 0.0f);
                    if (stretch) { sx = 400.0f / w; sy = 240.0f / h; }
                }
                C2D_DrawImageAtRotated(pageimg, 200, 120, 0.5f, ang, NULL, sx, sy);
            } else if (!page_dirty) {
                draw_text("这一页无法解码", 200, 108, 0.7f, COL_DIM, C2D_AlignCenter);
            }
            if (page_dirty) {
                C2D_DrawRectSolid(130, 100, 0.8f, 140, 40, C2D_Color32(0, 0, 0, 190));
                draw_text("加载中...", 200, 108, 0.6f, COL_TXT, C2D_AlignCenter);
            }
        } else {
            draw_text("EH 漫画阅读器", 200, 40, 0.9f, COL_ACC, C2D_AlignCenter);
            if (nmangas) {
                if (top_for != sel) { fit_text(mangas[sel].name, top_label, sizeof top_label, 0.6f, 380); top_for = sel; }
                draw_text(top_label, 200, 105, 0.6f, COL_TXT, C2D_AlignCenter);
                draw_text("在下屏点选封面，或用十字键选择后按 A", 200, 150, 0.45f, COL_DIM, C2D_AlignCenter);
            } else {
                draw_text("没有找到漫画", 200, 95, 0.7f, COL_TXT, C2D_AlignCenter);
                draw_text("请把漫画文件夹放进 SD 卡的 eh 文件夹", 200, 130, 0.5f, COL_DIM, C2D_AlignCenter);
            }
        }

        // ===== 下屏 =====
        C2D_TargetClear(bot, COL_BG);
        C2D_SceneBegin(bot);
        if (state == ST_LIST) {
            char hdr[96];
            C2D_DrawRectSolid(0, 0, 0.3f, 320, 20, COL_BAR);
            snprintf(hdr, sizeof hdr, "选择漫画   第 %d/%d 页   共 %d 本", gpage + 1, gpages, nmangas);
            draw_text(hdr, 6, 2, 0.5f, COL_TXT, 0);
            for (int k = 0; k < PER_PAGE; k++) {
                int idx = gpage * PER_PAGE + k;
                if (idx >= nmangas) break;
                Manga *m = &mangas[idx];
                float cx = (k % COLS) * CELL_W + 1, cy = GRID_Y + (k / COLS) * CELL_H;
                C2D_DrawRectSolid(cx + 2, cy, 0.3f, CELL_W - 4, CELL_H - 2, COL_CELL);
                if (idx == sel) {
                    float x = cx + 2, y = cy, w = CELL_W - 4, h = CELL_H - 2;
                    C2D_DrawRectSolid(x, y, 0.45f, w, 2, COL_ACC);
                    C2D_DrawRectSolid(x, y + h - 2, 0.45f, w, 2, COL_ACC);
                    C2D_DrawRectSolid(x, y, 0.45f, 2, h, COL_ACC);
                    C2D_DrawRectSolid(x + w - 2, y, 0.45f, 2, h, COL_ACC);
                }
                if (m->cover == 1) {
                    float ix = cx + (CELL_W - m->st.width) / 2.0f, iy = cy + 2 + (COV_H - m->st.height) / 2.0f;
                    C2D_DrawImageAt(m->img, ix, iy, 0.5f, NULL, 1.0f, 1.0f);
                } else {
                    draw_text(m->cover == 2 ? "无封面" : "...", cx + CELL_W / 2, cy + 32, 0.5f, COL_DIM, C2D_AlignCenter);
                }
                if (!m->label_ok) { fit_text(m->name, m->label, sizeof m->label, 0.42f, CELL_W - 10); m->label_ok = true; }
                draw_text(m->label, cx + CELL_W / 2, cy + COV_H + 3, 0.42f, COL_TXT, C2D_AlignCenter);
            }
            draw_text("十字键选择  A 打开  L/R 翻页  点按封面打开", 160, 224, 0.4f, COL_DIM, C2D_AlignCenter);
        } else {
            char buf[64];
            C2D_DrawRectSolid(0, 0, 0.3f, 320, 20, COL_BAR);
            draw_text(read_title, 6, 2, 0.5f, COL_TXT, 0);
            snprintf(buf, sizeof buf, "第 %d / %d 页", (dragging ? drag_page : cur) + 1, npg);
            draw_text(buf, 160, 62, 0.8f, COL_ACC, C2D_AlignCenter);
            // 进度条(可触摸拖动跳页)
            C2D_DrawRectSolid(20, 146, 0.3f, 280, 8, COL_BAR);
            float frac = npg > 1 ? (float)(dragging ? drag_page : cur) / (npg - 1) : 1.0f;
            C2D_DrawRectSolid(20, 146, 0.4f, 280 * frac, 8, COL_ACC);
            C2D_DrawRectSolid(20 + 280 * frac - 4, 140, 0.5f, 8, 20, COL_TXT);
            draw_text("↑上一页  ↓下一页  L/R 跳10页", 160, 104, 0.45f, COL_DIM, C2D_AlignCenter);
            draw_btn(8, 190, 96, 34, "返回(B)");
            draw_btn(112, 190, 96, 34, stretch ? "适应(Y)" : "铺满(Y)");
            draw_btn(216, 190, 96, 34, "翻转(X)");
        }
        if (toast_t > 0) {
            toast_t--;
            C2D_DrawRectSolid(60, 100, 0.9f, 200, 40, C2D_Color32(0, 0, 0, 210));
            draw_text(toast, 160, 109, 0.55f, COL_TXT, C2D_AlignCenter);
        }

        C3D_FrameEnd(0);
    }

    free_page_tex();
    free_pages();
    for (int i = 0; i < nmangas; i++) {
        if (mangas[i].cover == 1) C3D_TexDelete(&mangas[i].tex);
        free(mangas[i].name);
    }
    free(mangas);
    C2D_TextBufDelete(dynbuf);
    C2D_TextBufDelete(measurebuf);
    if (font) C2D_FontFree(font);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
