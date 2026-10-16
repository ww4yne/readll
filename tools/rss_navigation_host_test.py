"""中文：运行真实 RSS 导航、列表和首页本地概览函数。
English: Exercise production RSS navigation, list rendering and local Home overview.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
import subprocess
import tempfile
from home_cover_cache_host_test import ROOT


def function(name, path):
    text = path.read_text()
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    scan = re.sub(tokens, lambda m: " " * len(m[0]), text)
    match = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;{}]*\)\s*\{", scan, re.M)
    assert match, name
    at, depth = match.end(), 1
    while depth:
        depth += (scan[at] == "{") - (scan[at] == "}")
        at += 1
    return text[match.start():at]

rss = ROOT / "main/apps/app_rss.c"
home = ROOT / "main/apps/app_dashboard.c"
source = rss.read_text()
defines = "\n".join(re.findall(r"^#define RSS_(?:LIST_\w+|FOOTER_TOP|TITLE_PX|SOURCE_ICON_SIZE|GRAY_DARK)[^\n]*", source, re.M))
pending_type = source[source.index("typedef struct {"):source.index("} rss_pending_file_t;") + len("} rss_pending_file_t;")]
unit = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "rss_sync.h"
#include "rss_overview.h"
#include "ui_nav_layout.h"
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define UI_GRAY_BLACK 0
#define UI_GRAY_LIGHT 224
#define EPD_DRAW_ALIGN_LEFT 0
#define EPD_DRAW_ALIGN_CENTER 1
#define EPD_DRAW_ALIGN_RIGHT 2
#define RSS_CACHE_FILE "overview.cache"
#define RSS_PENDING_FILE "overview.pending"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define READ_PICO_MINIFLUX_BASE_URL_MAX 256
#define READ_PICO_MINIFLUX_TOKEN_MAX 256
#define ESP_ERR_NOT_FOUND 999
typedef struct {int x,y,width,height;} EpdRect;
typedef enum {APP_REDRAW_NONE,APP_REDRAW_PAGE} app_redraw_t;
typedef struct {int id;} app_desc_t;
typedef struct {const app_desc_t *request_app;} app_ctx_t;
typedef struct {int type,x0,y0;} ui_gesture_event_t;
enum {UI_GESTURE_TAP,UI_GESTURE_SWIPE_L,UI_GESTURE_SWIPE_R,UI_GESTURE_PRESS};
enum {UI_KEY_1,UI_KEY_2,UI_KEY_3};
enum {RSS_VIEW_LIST,RSS_VIEW_ARTICLE,RSS_VIEW_SETTINGS};
static app_desc_t apps[6];
static const app_desc_t *app_home_page(void){return &apps[0];}
static const app_desc_t *app_at(int i){assert(i>=0&&i<6);return &apps[i];}
static int s_view,s_list_page;
static rss_cache_t storage,*s_cache=&storage;
static rss_sync_t s_sync;
static size_t s_visible[128],s_visible_count;
static bool s_config_invalid,s_configured=true,s_storage_ready=true;
static char s_notice[128],s_last_sync_status[64];
static size_t s_article_page,s_article_page_count=3;
static int settings_draws,article_draws,nav_draws,row_lines,opens,marks,closes;
static bool open_ok=true;
static char texts[32][192];
static int text_count;
static void ui_nav_draw(uint8_t *fb,int active){(void)fb;assert(active==5);++nav_draws;}
static void ui_nav_status(uint8_t *fb){(void)fb;}
static void ui_clear_page(uint8_t *fb){(void)fb;}
static int ui_text_title_fit(char *text,int px,int width,const char *sample){(void)text;(void)width;(void)sample;return px;}
static void ui_text_vc(uint8_t *fb,int x,int y,int px,const char *text,int align,bool bold){
 (void)fb;(void)x;(void)align;(void)bold;assert(px>0&&text_count<32);
 if(y==1068)assert(y+px/2<=UI_NAV_TOUCH_TOP);
 snprintf(texts[text_count++],192,"%s",text);
}
#define ui_text_fixed ui_text_vc
#define ui_text_fixed_vc ui_text_vc
static void ui_hairline(uint8_t *fb,int y,int x,int w,int gray){(void)fb;(void)x;(void)w;(void)gray;if(y>=RSS_LIST_TOP){assert(y<RSS_LIST_BOTTOM);++row_lines;}}
static void ui_draw_control_frame(uint8_t *fb,EpdRect r,int radius,int gray){(void)fb;(void)radius;(void)gray;assert(r.y+r.height<RSS_LIST_BOTTOM);}
static void draw_gear(uint8_t *fb,EpdRect r){(void)fb;(void)r;}
static void draw_double_check(uint8_t *fb,EpdRect r){(void)fb;(void)r;}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static bool open_article(int64_t id){assert(id>0);++opens;if(open_ok)s_view=RSS_VIEW_ARTICLE;return open_ok;}
static bool mark_page_through(size_t i){assert(i<s_visible_count);++marks;return true;}
static bool mark_current_page_read(void){++marks;return true;}
static void close_article(void){++closes;}
static app_redraw_t settings_gesture(const ui_gesture_event_t *ev){(void)ev;return APP_REDRAW_NONE;}
static void draw_settings(uint8_t *fb){(void)fb;++settings_draws;}
static void draw_article(uint8_t *fb){(void)fb;++article_draws;}
static bool alloc_fail,store_ok=true;
static int config_error;
static void *heap_caps_malloc(size_t n,int caps){assert(caps==3);return alloc_fail?NULL:malloc(n);}
static bool prepare_storage(void){return store_ok;}
static esp_err_t read_pico_transfer_miniflux_get(char *base,size_t b,char *token,size_t t){snprintf(base,b,"https://example.test");snprintf(token,t,"not-a-real-token");return config_error;}
static rss_cache_result_t cache_result=RSS_CACHE_OK;
rss_cache_result_t rss_cache_load(const char *path,rss_cache_t *cache){assert(!strcmp(path,RSS_CACHE_FILE));*cache=storage;return cache_result;}
''' + pending_type + "\n"
# Layout macros are needed by the drawing stubs above.
at = unit.index("typedef struct {int x,y,width,height;}")
unit = unit[:at] + defines + "\n" + unit[at:]
for path, names in (
    (ROOT / "main/ui/ui_nav.c", ("ui_nav_hit", "ui_nav_request")),
    (rss, ("secure_clear", "pending_read_file", "rss_overview_load",
           "settings_action_rect", "read_all_action_rect", "source_initial",
           "list_footer_middle", "draw_list", "render", "article_move",
           "list_gesture", "article_gesture", "on_gesture", "on_key", "main_page_visible")),
):
    for name in names:
        unit += function(name, path) + "\n"
unit += "static rss_overview_t s_rss_overview;\n"
unit += r'''
static void ui_draw_round_rect(uint8_t *fb,EpdRect r,int radius,int gray){(void)fb;(void)radius;(void)gray;assert(r.y>=1005&&r.y+r.height<=UI_NAV_TOUCH_TOP);}
'''
unit += function("rss_card_rect", home) + "\n" + function("render_rss_overview", home)
home_route = re.search(r"if\(hit\(rss_card_rect\(\),x,y\)\)\{[^}]+\}",
                       function("on_gesture", home))
assert home_route
unit += "\n#define hit ui_rect_hit\n"
unit += "static app_redraw_t home_card_tap(app_ctx_t *ctx,int x,int y){" + home_route[0] + "return APP_REDRAW_NONE;}\n"
book_control = function("control_at", ROOT / "main/apps/app_book.c")
start = book_control.index("        int tab = ui_nav_hit(x, y);")
end = book_control.index("        return -1;", start) + len("        return -1;")
unit += "static int import_nav_control(int x,int y,EpdRect *rect){" + book_control[start:end] + "}\n"
unit += r'''
static bool contains(const char *text){for(int i=0;i<text_count;++i)if(strstr(texts[i],text))return true;return false;}
static void reset_draw(void){nav_draws=row_lines=text_count=0;}
static void write_pending(void){
 rss_pending_file_t p={.magic=UINT32_C(0x52535031),.version=1,.count=2};
 p.operations[0]=(rss_sync_operation_t){.entry_id=1,.desired={.read_known=true,.read=true}};
 p.operations[1]=(rss_sync_operation_t){.entry_id=2,.desired={.read_known=true,.read=false}};
 FILE *f=fopen(RSS_PENDING_FILE,"wb");assert(f&&fwrite(&p,1,sizeof(p),f)==sizeof(p)&&!fclose(f));
}
int main(void){
 uint8_t fb=0;app_ctx_t ctx={0};
 for(int x=0;x<684;++x){
   EpdRect r={0};assert(import_nav_control(x,1141,&r)==705+ui_nav_hit(x,1141));
   assert(r.x<=x&&x<r.x+r.width&&r.x+r.width<=684&&r.width<=137);
 }
 EpdRect card=rss_card_rect();
 for(int y=card.y-1;y<=card.y+card.height;++y)for(int x=card.x-1;x<=card.x+card.width;++x){
   ctx.request_app=NULL;home_card_tap(&ctx,x,y);
   assert(ctx.request_app==(ui_rect_hit(card,x,y)?&apps[5]:NULL));
 }
 s_visible_count=24;
 for(int i=0;i<24;++i){s_visible[i]=i;storage.metadata[i].entry.id=i+1;snprintf(storage.metadata[i].entry.title,RSS_ENTRY_TITLE_MAX,"Article %d",i+1);}
 storage.metadata_count=24;storage.metadata[1].entry.status=RSS_ENTRY_STATUS_READ;
 assert(RSS_LIST_ROWS==12&&RSS_LIST_ROW_HEIGHT==75&&RSS_LIST_BOTTOM<UI_NAV_TOUCH_TOP);
 for(int slot=0;slot<5;++slot){
   s_view=RSS_VIEW_LIST;ctx.request_app=NULL;
   ui_gesture_event_t e={UI_GESTURE_TAP,(slot*2+1)*684/10,1141};
   assert(on_gesture(&ctx,&e)==APP_REDRAW_NONE);
   assert(ctx.request_app==(slot==2?NULL:&apps[ui_nav_app_index(slot)]));
 }
 for(int view=RSS_VIEW_ARTICLE;view<=RSS_VIEW_SETTINGS;++view){
   s_view=view;ctx.request_app=NULL;assert(!main_page_visible(&ctx));
   ui_gesture_event_t e={UI_GESTURE_TAP,650,1141};on_gesture(&ctx,&e);assert(!ctx.request_app);
   reset_draw();render(&ctx,&fb);assert(!nav_draws);
 }
 s_view=RSS_VIEW_LIST;assert(main_page_visible(&ctx));
 reset_draw();render(&ctx,&fb);assert(nav_draws==1&&row_lines==12&&contains("1 / 2"));
 s_sync.state=RSS_SYNC_FAILED;s_sync.error.client_error.http_status=401;
 reset_draw();render(&ctx,&fb);assert(contains("认证失败")&&nav_draws==1);
 snprintf(s_notice,sizeof(s_notice),"保存失败");reset_draw();render(&ctx,&fb);assert(contains("保存失败"));s_notice[0]=0;
 s_cache=NULL;reset_draw();render(&ctx,&fb);assert(nav_draws==1&&contains("内存不足"));
 ui_gesture_event_t e={UI_GESTURE_TAP,68,1141};ctx.request_app=NULL;on_gesture(&ctx,&e);assert(ctx.request_app==&apps[0]);
 e=(ui_gesture_event_t){UI_GESTURE_TAP,200,200};assert(on_gesture(&ctx,&e)==APP_REDRAW_NONE);
 s_cache=&storage;open_ok=false;assert(on_gesture(&ctx,&e)==APP_REDRAW_PAGE&&opens==1&&!marks);
 open_ok=true;e.y0=RSS_LIST_BOTTOM-1;assert(on_gesture(&ctx,&e)==APP_REDRAW_PAGE&&opens==2&&marks==1);
 s_view=RSS_VIEW_LIST;s_list_page=0;assert(on_key(&ctx,UI_KEY_3)==APP_REDRAW_PAGE&&s_list_page==1);
 assert(on_key(&ctx,UI_KEY_1)==APP_REDRAW_PAGE&&!s_list_page);
 ctx.request_app=NULL;on_key(&ctx,UI_KEY_2);assert(ctx.request_app==&apps[0]);
 s_view=RSS_VIEW_ARTICLE;assert(on_key(&ctx,UI_KEY_2)==APP_REDRAW_PAGE&&s_view==RSS_VIEW_LIST&&closes==1);
 FILE *f=fopen(RSS_CACHE_FILE,"wb");assert(f&&!fclose(f));write_pending();
 rss_overview_t out;rss_overview_load(&out);
 assert(out.counts_known&&out.unread==23&&out.pending==2&&!strstr(out.status,"成功"));
 s_rss_overview=out;reset_draw();render_rss_overview(&fb);assert(contains("缓存未读 23")&&contains("待同步 2"));
 snprintf(s_last_sync_status,sizeof(s_last_sync_status),"认证失败");rss_overview_load(&out);assert(strstr(out.status,"认证失败"));
 assert(!rename(RSS_PENDING_FILE,RSS_PENDING_FILE ".bak"));rss_overview_load(&out);assert(out.counts_known&&out.unread==23&&out.pending==2);
 f=fopen(RSS_PENDING_FILE ".bak","wb");assert(f&&!fclose(f));rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"待同步"));
 remove(RSS_PENDING_FILE ".bak");cache_result=RSS_CACHE_CORRUPT;rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"缓存读取失败"));
 cache_result=RSS_CACHE_OK;alloc_fail=true;rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"内存不足"));alloc_fail=false;
 store_ok=false;rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"存储"));store_ok=true;
 config_error=ESP_ERR_NOT_FOUND;rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"未配置"));
 config_error=ESP_FAIL;rss_overview_load(&out);assert(!out.counts_known&&strstr(out.status,"读取失败"));
 s_rss_overview=out;reset_draw();render_rss_overview(&fb);assert(contains("缓存未读 --")&&!contains("缓存未读 0"));
 remove(RSS_CACHE_FILE);
 assert(settings_draws&&article_draws);
 puts("PASS: 5 RSS routes, root/subview/OOM navigation, all 12 rows, visible errors, keys and failed-open guard; Home counts overlay pending reads/unreads, recover backup and expose config/cache/storage/memory failures without network or bodies");
}
'''
assert "rss_overview_load(&s_rss_overview)" in function("on_enter", home)
assert "render_rss_overview(fb)" in function("render_root", home)
assert "hit(rss_card_rect(),x,y)" in function("on_gesture", home)
with tempfile.TemporaryDirectory(dir=ROOT / "build") as directory:
    c, binary = Path(directory) / "test.c", Path(directory) / "test"
    c.write_text(unit)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(ROOT / "main/rss"),
                    "-I" + str(ROOT / "main/ui"), "-I" + str(ROOT / "tools/rss_test/stubs"),
                    str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], cwd=directory, check=True)
