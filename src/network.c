// waybar CFFI plugin: network.
//  - Bar pill: wifi-strength / ethernet / disconnected icon + (wifi signal% or IP).
//  - Click → popover: connection details (SSID, signal, IP), live ↓/↑ throughput,
//    a wifi toggle, and a scannable wifi list with click-to-connect (password
//    entry for secured networks).
//  - State via nmcli; throughput from /proc/net/dev. Refreshed every 3s.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

// nf-md glyphs
#define IC_ETH   "\xf3\xb0\x88\x80"   // 󰈀 ethernet
#define IC_OFF   "\xf3\xb0\xa4\xad"   // 󰤭 wifi-off/strength-off
#define IC_DL    "\xf3\xb0\x87\x87"   // 󰇇 (down arrow via text below instead)

typedef struct {
  GtkWidget *box, *icon, *label, *popover;
  char type[16], ssid[128], ip[64], dev[32];
  int signal;
  unsigned long long rx, tx; double rx_rate, tx_rate;   // bytes, bytes/s
  int interval; guint timer; GCancellable *cancel;
  GtkWidget *pw_entry, *pw_row; char pw_ssid[128];
  char *icon_dir; int icon_size;
} Inst;

static const char *wifi_icon_name(int sig) {
  if (sig < 34) return "wifi1.svg";
  if (sig < 67) return "wifi2.svg";
  return "wifi3.svg";
}
// Load an SVG and recolour it to the widget's theme colour (a white/alpha
// silhouette → the CSS `color`, so icons follow matugen like the old glyphs did).
static GdkPixbuf *themed_pixbuf(GtkWidget *w, const char *dir, int size, const char *name) {
  char *p = g_build_filename(dir, name, NULL);
  GdkPixbuf *src = gdk_pixbuf_new_from_file_at_size(p, size, size, NULL);
  g_free(p);
  if (!src) return NULL;
  GdkPixbuf *d = gdk_pixbuf_get_has_alpha(src) ? gdk_pixbuf_copy(src)
                                               : gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0);
  g_object_unref(src);
  GdkRGBA c; GtkStyleContext *sc = gtk_widget_get_style_context(w);
  gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &c);
  guchar R = (guchar)(c.red*255), G = (guchar)(c.green*255), B = (guchar)(c.blue*255);
  int wd = gdk_pixbuf_get_width(d), h = gdk_pixbuf_get_height(d);
  int rs = gdk_pixbuf_get_rowstride(d), nc = gdk_pixbuf_get_n_channels(d);
  guchar *px = gdk_pixbuf_get_pixels(d);
  for (int y = 0; y < h; y++) for (int x = 0; x < wd; x++) {
    guchar *q = px + y*rs + x*nc; q[0]=R; q[1]=G; q[2]=B;
    if (nc == 4) q[3] = (guchar)(q[3]*c.alpha);
  }
  return d;
}
static void icon_restyle(GtkWidget *img, gpointer data) {
  Inst *self = data;
  const char *name = g_object_get_data(G_OBJECT(img), "svg");
  if (!name) return;
  GdkPixbuf *pb = themed_pixbuf(img, self->icon_dir, self->icon_size, name);
  if (pb) { gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb); g_object_unref(pb); }
}
static void set_bar_icon(Inst *self, const char *name) {
  g_object_set_data_full(G_OBJECT(self->icon), "svg", g_strdup(name), g_free);
  icon_restyle(self->icon, self);
}

static const char *wifi_glyph(int sig) {
  if (sig < 0) return IC_OFF;
  if (sig < 25) return "\xf3\xb0\xa4\x9f";   // 󰤟 strength-1
  if (sig < 50) return "\xf3\xb0\xa4\xa2";   // 󰤢 strength-2
  if (sig < 75) return "\xf3\xb0\xa4\xa5";   // 󰤥 strength-3
  return "\xf3\xb0\xa4\xa8";                  // 󰤨 strength-4
}
static void fmt_rate(double bps, char *out, size_t n) {
  if (bps < 1024) g_snprintf(out, n, "%.0f B/s", bps);
  else if (bps < 1048576) g_snprintf(out, n, "%.1f KB/s", bps / 1024);
  else if (bps < 1073741824.0) g_snprintf(out, n, "%.1f MB/s", bps / 1048576);
  else g_snprintf(out, n, "%.1f GB/s", bps / 1073741824.0);
}

// ─── /proc/net/dev throughput for the active device ──────────────────────────
static void read_throughput(Inst *self) {
  if (!self->dev[0]) { self->rx_rate = self->tx_rate = 0; return; }
  FILE *f = fopen("/proc/net/dev", "r");
  if (!f) return;
  char line[512]; unsigned long long rx = 0, tx = 0; int found = 0;
  while (fgets(line, sizeof line, f)) {
    char *c = strchr(line, ':'); if (!c) continue;
    *c = 0; char *name = g_strstrip(line);
    if (strcmp(name, self->dev) != 0) continue;
    sscanf(c + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx);
    found = 1; break;
  }
  fclose(f);
  if (found) {
    double dt = self->interval;
    if (self->rx || self->tx) {
      self->rx_rate = rx > self->rx ? (rx - self->rx) / dt : 0;
      self->tx_rate = tx > self->tx ? (tx - self->tx) / dt : 0;
    }
    self->rx = rx; self->tx = tx;
  }
}

static void update_bar(Inst *self) {
  GtkStyleContext *c = gtk_widget_get_style_context(self->box);
  gtk_style_context_remove_class(c, "connected");
  gtk_style_context_remove_class(c, "disconnected");
  if (!strcmp(self->type, "wifi")) {
    set_bar_icon(self, wifi_icon_name(self->signal));
    char t[16]; g_snprintf(t, sizeof t, "%d%%", self->signal);
    gtk_label_set_text(GTK_LABEL(self->label), t); gtk_widget_show(self->label);
    gtk_style_context_add_class(c, "connected");
  } else if (!strcmp(self->type, "ethernet")) {
    set_bar_icon(self, "ethernet.svg");
    gtk_widget_hide(self->label);
    gtk_style_context_add_class(c, "connected");
  } else {
    set_bar_icon(self, "disconnected.svg");
    gtk_widget_hide(self->label);
    gtk_style_context_add_class(c, "disconnected");
  }
}

// ─── nmcli state fetch (async) ───────────────────────────────────────────────
typedef struct { Inst *self; GCancellable *cancel; } SCtx;
static void parse_state(Inst *self, const char *out) {
  // format: type|ssid|signal|ip|dev — split preserving empty fields (strtok_r
  // would collapse the empty ssid/signal on ethernet and shift ip/dev off).
  char buf[512]; g_strlcpy(buf, out, sizeof buf); g_strchomp(buf);
  char **f = g_strsplit(buf, "|", 5);
  guint n = g_strv_length(f);
  g_strlcpy(self->type, n > 0 ? f[0] : "", sizeof self->type);
  g_strlcpy(self->ssid, n > 1 ? f[1] : "", sizeof self->ssid);
  self->signal = (n > 2 && *f[2]) ? atoi(f[2]) : -1;
  g_strlcpy(self->ip, n > 3 ? f[3] : "", sizeof self->ip);
  g_strlcpy(self->dev, n > 4 ? f[4] : "", sizeof self->dev);
  g_strfreev(f);
  read_throughput(self);
  update_bar(self);
}
static const char *STATE_CMD =
  "dev=$(nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null | awk -F: '$3==\"connected\"&&$2!=\"loopback\"{print;exit}');"
  "d=$(echo \"$dev\"|cut -d: -f1); t=$(echo \"$dev\"|cut -d: -f2);"
  "ssid=\"\"; sig=\"\";"
  "if [ \"$t\" = wifi ]; then l=$(nmcli -t -f IN-USE,SIGNAL,SSID device wifi 2>/dev/null|awk -F: '$1==\"*\"{print;exit}'); sig=$(echo \"$l\"|cut -d: -f2); ssid=$(echo \"$l\"|cut -d: -f3-); fi;"
  "ip=$(nmcli -t -f IP4.ADDRESS device show \"$d\" 2>/dev/null|head -1|cut -d: -f2|cut -d/ -f1);"
  "echo \"$t|$ssid|$sig|$ip|$d\"";

static void sctx_free(SCtx *c) { g_object_unref(c->cancel); g_free(c); }
static void on_state(GObject *src, GAsyncResult *res, gpointer data) {
  SCtx *c = data; char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL);
  if (g_cancellable_is_cancelled(c->cancel)) { g_free(out); sctx_free(c); return; }
  if (ok && out) parse_state(c->self, out);
  g_free(out); sctx_free(c);
}
static void fetch_state(Inst *self) {
  GSubprocess *sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                     NULL, "sh", "-c", STATE_CMD, NULL);
  if (!sp) return;
  SCtx *c = g_new0(SCtx, 1); c->self = self; c->cancel = g_object_ref(self->cancel);
  g_subprocess_communicate_utf8_async(sp, NULL, self->cancel, on_state, c);
  g_object_unref(sp);
}

// ─── wifi actions ────────────────────────────────────────────────────────────
static void rebuild_popover(Inst *self);
static void nm_async(const char *const *argv) {
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
typedef struct { Inst *self; char ssid[128]; } WCtx;
static void on_connect_done(GObject *src, GAsyncResult *res, gpointer data) {
  WCtx *w = data;
  g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
  fetch_state(w->self); rebuild_popover(w->self);
  g_free(w);
}
static void wifi_connect(Inst *self, const char *ssid, const char *pw) {
  GSubprocess *sp;
  if (pw && *pw)
    sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                          "nmcli", "device", "wifi", "connect", ssid, "password", pw, NULL);
  else
    sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                          "nmcli", "device", "wifi", "connect", ssid, NULL);
  if (!sp) return;
  WCtx *w = g_new0(WCtx, 1); w->self = self; g_strlcpy(w->ssid, ssid, sizeof w->ssid);
  g_subprocess_wait_async(sp, self->cancel, on_connect_done, w);
  g_object_unref(sp);
}

typedef struct { Inst *self; char ssid[128]; int secured; } RowCtx;
static void on_wifi_row(GtkButton *b, gpointer d) {
  (void)b; RowCtx *r = d; Inst *self = r->self;
  if (r->secured) {   // reveal password entry targeting this ssid
    g_strlcpy(self->pw_ssid, r->ssid, sizeof self->pw_ssid);
    rebuild_popover(self);
  } else wifi_connect(self, r->ssid, NULL);
}
static void rowctx_free(gpointer p, GClosure *c) { (void)c; g_free(p); }
static void on_pw_activate(GtkEntry *e, gpointer d) {
  Inst *self = d;
  wifi_connect(self, self->pw_ssid, gtk_entry_get_text(e));
  self->pw_ssid[0] = 0;
}
static void on_wifi_toggle(GtkButton *b, gpointer d) {
  (void)b; Inst *self = d;
  const char *on[] = {"nmcli","radio","wifi","on",NULL};
  const char *off[] = {"nmcli","radio","wifi","off",NULL};
  nm_async(self->type[0] || self->signal >= 0 ? off : on);   // rough toggle
  rebuild_popover(self);
}

// ─── popover ─────────────────────────────────────────────────────────────────
static gboolean on_pop_key(GtkWidget *w, GdkEventKey *e, gpointer d) {
  (void)d; if (e->keyval == GDK_KEY_Escape) { gtk_popover_popdown(GTK_POPOVER(w)); return TRUE; }
  return FALSE;
}
// Grant the bar's layer surface on-demand keyboard focus while a popover is open,
// so Escape reaches it and the modal grab dismisses on click-outside; release on close.
static void pop_kb(GtkWidget *ref, gboolean on) {
  GtkWidget *top = gtk_widget_get_toplevel(ref);
  if (GTK_IS_WINDOW(top) && gtk_layer_is_layer_window(GTK_WINDOW(top)))
    gtk_layer_set_keyboard_mode(GTK_WINDOW(top),
      on ? GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND : GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
}
static void on_pop_closed(GtkWidget *pop, gpointer data) { (void)pop; pop_kb(GTK_WIDGET(data), FALSE); }
static GtkWidget *info_row(const char *k, const char *v) {
  GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *kl = gtk_label_new(k); gtk_widget_set_halign(kl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(kl), "nw-key");
  GtkWidget *vl = gtk_label_new(v && *v ? v : "—"); gtk_widget_set_halign(vl, GTK_ALIGN_END);
  gtk_widget_set_hexpand(vl, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(vl), "nw-val");
  gtk_box_pack_start(GTK_BOX(r), kl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(r), vl, TRUE, TRUE, 0);
  return r;
}
static void rebuild_popover(Inst *self) {
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->popover));
  if (old) gtk_widget_destroy(old);
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(v, 16); gtk_widget_set_margin_end(v, 16);
  gtk_widget_set_margin_top(v, 14); gtk_widget_set_margin_bottom(v, 14);
  gtk_widget_set_size_request(v, 340, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "nw-pop");

  GtkWidget *hd = gtk_label_new(self->type[0] ? (strcmp(self->type,"wifi")==0 ? "Wi-Fi" : "Ethernet") : "Disconnected");
  gtk_widget_set_halign(hd, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(hd), "nw-head");
  gtk_box_pack_start(GTK_BOX(v), hd, FALSE, FALSE, 0);
  if (!strcmp(self->type, "wifi")) {
    gtk_box_pack_start(GTK_BOX(v), info_row("Network", self->ssid), FALSE, FALSE, 0);
    char s[16]; g_snprintf(s, sizeof s, "%d%%", self->signal);
    gtk_box_pack_start(GTK_BOX(v), info_row("Signal", s), FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(v), info_row("IP", self->ip), FALSE, FALSE, 0);
  char dn[24], up[24]; fmt_rate(self->rx_rate, dn, sizeof dn); fmt_rate(self->tx_rate, up, sizeof up);
  char thru[64]; g_snprintf(thru, sizeof thru, "\xe2\x86\x93 %s   \xe2\x86\x91 %s", dn, up);
  GtkWidget *tl = gtk_label_new(thru); gtk_widget_set_halign(tl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(tl), "nw-thru");
  gtk_box_pack_start(GTK_BOX(v), tl, FALSE, FALSE, 2);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
  GtkWidget *wh = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *whl = gtk_label_new("Wi-Fi networks"); gtk_widget_set_halign(whl, GTK_ALIGN_START); gtk_widget_set_hexpand(whl, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(whl), "nw-head");
  GtkWidget *tog = gtk_button_new_with_label("Toggle Wi-Fi");
  g_signal_connect(tog, "clicked", G_CALLBACK(on_wifi_toggle), self);
  gtk_box_pack_start(GTK_BOX(wh), whl, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(wh), tog, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), wh, FALSE, FALSE, 0);

  // wifi list (synchronous, quick): IN-USE:SIGNAL:SSID:SECURITY
  GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  char *out = NULL;
  if (g_spawn_command_line_sync("nmcli -t -f IN-USE,SIGNAL,SSID,SECURITY device wifi", &out, NULL, NULL, NULL) && out) {
    char *save = NULL; int shown = 0;
    for (char *ln = strtok_r(out, "\n", &save); ln && shown < 8; ln = strtok_r(NULL, "\n", &save)) {
      char *fs = NULL; char *inuse = strtok_r(ln, ":", &fs);
      char *sig = strtok_r(NULL, ":", &fs); char *ssid = strtok_r(NULL, ":", &fs); char *sec = strtok_r(NULL, ":", &fs);
      if (!ssid || !*ssid) continue;
      int secured = sec && *sec && strcmp(sec, "--") != 0;
      char lbl[192]; g_snprintf(lbl, sizeof lbl, "%s %s%s  %s%%", wifi_glyph(sig?atoi(sig):0),
                                ssid, secured ? " \xf3\xb0\x8c\xbe" : "", sig ?: "0");   // lock glyph 󰌾
      GtkWidget *btn = gtk_button_new_with_label(lbl);
      gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(btn)), GTK_ALIGN_START);
      if (inuse && !strcmp(inuse, "*")) gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nw-active");
      gtk_style_context_add_class(gtk_widget_get_style_context(btn), "nw-wifi");
      RowCtx *rc = g_new0(RowCtx, 1); rc->self = self; rc->secured = secured; g_strlcpy(rc->ssid, ssid, sizeof rc->ssid);
      g_signal_connect_data(btn, "clicked", G_CALLBACK(on_wifi_row), rc, rowctx_free, 0);
      gtk_box_pack_start(GTK_BOX(list), btn, FALSE, FALSE, 0);
      shown++;
    }
  }
  g_free(out);
  gtk_box_pack_start(GTK_BOX(v), list, FALSE, FALSE, 0);

  if (self->pw_ssid[0]) {   // password prompt for a secured network
    GtkWidget *pr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(e), FALSE);
    char ph[160]; g_snprintf(ph, sizeof ph, "Password for %s", self->pw_ssid);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), ph);
    gtk_widget_set_hexpand(e, TRUE);
    g_signal_connect(e, "activate", G_CALLBACK(on_pw_activate), self);
    gtk_box_pack_start(GTK_BOX(pr), e, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), pr, FALSE, FALSE, 0);
  }

  gtk_container_add(GTK_CONTAINER(self->popover), v);
  gtk_widget_show_all(v);
  if (self->pw_ssid[0]) {  // focus the password field
    GList *ch = gtk_container_get_children(GTK_CONTAINER(v));
    (void)ch; g_list_free(ch);
  }
}
static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer data) {
  (void)w; if (ev->button != 1) return FALSE;
  Inst *self = data; self->pw_ssid[0] = 0;
  fetch_state(self); rebuild_popover(self);
  pop_kb(self->box, TRUE);
  gtk_popover_popup(GTK_POPOVER(self->popover));
  gtk_widget_grab_focus(self->popover);
  return TRUE;
}
static gboolean tick(gpointer data) { fetch_state(data); return G_SOURCE_CONTINUE; }

static GtkWidget *mklabel(const char *t, const char *cls) {
  GtkWidget *l = gtk_label_new(t); gtk_widget_set_valign(l, GTK_ALIGN_CENTER); gtk_style_context_add_class(gtk_widget_get_style_context(l), cls); return l;
}
void *wbcffi_init(const wbcffi_init_info *info, const wbcffi_config_entry *entries, size_t entries_len) {
  Inst *self = g_new0(Inst, 1);
  self->interval = 3; self->signal = -1; self->icon_size = 24;
  for (size_t i = 0; i < entries_len; i++) {
    if (!strcmp(entries[i].key, "interval")) { self->interval = atoi(entries[i].value); if (self->interval < 1) self->interval = 1; }
    else if (!strcmp(entries[i].key, "icon-size")) { self->icon_size = atoi(entries[i].value); if (self->icon_size < 8) self->icon_size = 8; }
    else if (!strcmp(entries[i].key, "icon-dir")) { g_free(self->icon_dir); self->icon_dir = g_strdup(entries[i].value); }
  }
  if (!self->icon_dir) {
    const char *dh = g_getenv("XDG_DATA_HOME");
    self->icon_dir = (dh && *dh) ? g_build_filename(dh, "waybar-network", NULL)
                                 : g_build_filename(g_get_home_dir(), ".local/share/waybar-network", NULL);
  }
  self->cancel = g_cancellable_new();

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_event_box_new();
  gtk_widget_set_name(self->box, "network");
  gtk_widget_add_events(self->box, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_margin_start(self->box, 6); gtk_widget_set_margin_end(self->box, 6);
  GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  self->icon = gtk_image_new();
  gtk_widget_set_valign(self->icon, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->icon), "nw-icon");
  g_signal_connect(self->icon, "style-updated", G_CALLBACK(icon_restyle), self);
  set_bar_icon(self, "disconnected.svg");
  self->label = mklabel("", "nw-label");
  gtk_box_pack_start(GTK_BOX(h), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(h), self->label, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(self->box), h);
  self->popover = gtk_popover_new(self->box);
  gtk_popover_set_position(GTK_POPOVER(self->popover), GTK_POS_BOTTOM);
  gtk_popover_set_constrain_to(GTK_POPOVER(self->popover), GTK_POPOVER_CONSTRAINT_NONE);
  gtk_popover_set_modal(GTK_POPOVER(self->popover), TRUE);
  gtk_widget_add_events(self->popover, GDK_KEY_PRESS_MASK);
  g_signal_connect(self->popover, "key-press-event", G_CALLBACK(on_pop_key), NULL);
  g_signal_connect(self->popover, "closed", G_CALLBACK(on_pop_closed), self->box);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  fetch_state(self);
  self->timer = g_timeout_add_seconds(self->interval, tick, self);
  return self;
}
void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  if (self->cancel) g_cancellable_cancel(self->cancel);
  if (self->timer) g_source_remove(self->timer);
  g_clear_object(&self->cancel);
  g_free(self->icon_dir);
  g_free(self);
}
