// mycam-tray: the Linux tray icon (IMPROVEMENTS.md §13, phase 4a). A separate program in the desktop session;
// the camera itself stays in the headless mycam-companion service. It watches the service over the control
// socket (control_posix.h) and shows the Windows tray's icons and menu (main.cpp ShowMenu): status, Pause,
// camera, Quality, Mirror, Fill, Connect an iPhone, Reconnect, Start at login, Quit.
//
// The icon goes through AppIndicator (StatusNotifierItem), which Ubuntu's dock shows out of the box. The
// "Connect an iPhone" window is plain GTK for now; it gets the XP look with the shared drawing kit (phase 4b).

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <libayatana-appindicator/app-indicator.h>

#include <algorithm>
#include <string>

#include "control_proto.h"
#include "platform.h"
#include "status_text.h"
#include "../third_party/qrcodegen/qrcodegen.hpp"

namespace mycam {
namespace {

std::string RuntimeDir() {
    const char* dir = getenv("XDG_RUNTIME_DIR");
    return dir && *dir ? dir : "";
}

int Connect() {
    const std::string path = RuntimeDir() + "/mycam.sock";
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (RuntimeDir().empty() || path.size() >= sizeof(addr.sun_path)) return -1;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    return s;
}

bool Send(int s, const std::string& line) {
    const std::string text = line + "\n";
    return s >= 0 && send(s, text.data(), text.size(), MSG_NOSIGNAL) == ssize_t(text.size());
}

// The tray icons are the Windows ones (companion.rc ids 101-105, design/tools/make_icons.ps1): each .ico holds
// PNG frames, and the 32 px one is written out for the dock, which wants a file.
std::string IconPath(int id) {
    const std::string path = RuntimeDir() + "/mycam-tray/icon-" + std::to_string(id) + ".png";
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return path;
    const std::string ico = ResourceData(id);
    if (ico.size() < 6) return "";
    const int count = uint8_t(ico[4]) | uint8_t(ico[5]) << 8;
    for (int i = 0; i < count && 6 + 16 * (i + 1) <= int(ico.size()); ++i) {
        const uint8_t* e = reinterpret_cast<const uint8_t*>(ico.data()) + 6 + 16 * i;
        const uint32_t size = e[8] | e[9] << 8 | e[10] << 16 | uint32_t(e[11]) << 24;
        const uint32_t offset = e[12] | e[13] << 8 | e[14] << 16 | uint32_t(e[15]) << 24;
        if (e[0] != 32 || offset > ico.size() || size > ico.size() - offset) continue;
        g_mkdir_with_parents((RuntimeDir() + "/mycam-tray").c_str(), 0700);
        if (g_file_set_contents(path.c_str(), ico.data() + offset, size, nullptr)) return path;
    }
    return "";
}

// systemctl --user, without a shell. Returns the exit code (-1: couldn't run it).
int Systemctl(const char* verb) {
    const char* argv[] = {"systemctl", "--user", verb, "mycam.service", nullptr};
    int status = -1;
    if (!g_spawn_sync(nullptr, const_cast<char**>(argv), nullptr,
                      GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                      nullptr, nullptr, nullptr, nullptr, &status, nullptr)) {
        return -1;
    }
    return g_spawn_check_wait_status(status, nullptr) ? 0 : 1;
}

// --- Connect an iPhone ---------------------------------------------------------------------------------
// Its own "pair" connection: closing the window closes it, which stops pairing (as Ctrl+C does for --pair).

struct PairWindow {
    GtkWidget* window = nullptr;
    GtkWidget* code = nullptr;
    GtkWidget* note = nullptr;
    int socket = -1;
    guint watch = 0;
    std::string buffer;
    std::string url;
};

PairWindow g_pair;

gboolean DrawQr(GtkWidget* widget, cairo_t* cr, gpointer) {
    const int w = gtk_widget_get_allocated_width(widget), h = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (g_pair.url.empty()) return TRUE;
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(g_pair.url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = qr.getSize() + 8; // A 4-module quiet zone.
    const int cell = std::max(1, std::min(w, h) / modules);
    const int x0 = (w - cell * modules) / 2 + 4 * cell, y0 = (h - cell * modules) / 2 + 4 * cell;
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (int y = 0; y < qr.getSize(); ++y)
        for (int x = 0; x < qr.getSize(); ++x)
            if (qr.getModule(x, y)) cairo_rectangle(cr, x0 + x * cell, y0 + y * cell, cell, cell);
    cairo_fill(cr);
    return TRUE;
}

void ClosePairing() {
    if (g_pair.watch) g_source_remove(g_pair.watch);
    if (g_pair.socket >= 0) close(g_pair.socket);
    g_pair.watch = 0;
    g_pair.socket = -1;
    g_pair.buffer.clear();
    g_pair.url.clear();
}

gboolean OnPairData(gint fd, GIOCondition, gpointer) {
    char chunk[512];
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
        gtk_label_set_text(GTK_LABEL(g_pair.note), "MyCam stopped answering. Close this window and try again.");
        g_pair.watch = 0;
        ClosePairing();
        return G_SOURCE_REMOVE;
    }
    g_pair.buffer.append(chunk, size_t(n));
    for (size_t nl; (nl = g_pair.buffer.find('\n')) != std::string::npos; g_pair.buffer.erase(0, nl + 1)) {
        const std::string line = g_pair.buffer.substr(0, nl);
        if (line.rfind("url ", 0) == 0) {
            g_pair.url = line.substr(4);
            gtk_widget_queue_draw(g_pair.code);
        } else if (line.rfind("paired ", 0) == 0) {
            const std::string text = "Paired with " + line.substr(7) + ". It connects by itself from now on.";
            gtk_label_set_text(GTK_LABEL(g_pair.note), text.c_str());
        }
    }
    return G_SOURCE_CONTINUE;
}

void ShowPairWindow() {
    if (g_pair.window) {
        gtk_window_present(GTK_WINDOW(g_pair.window));
        return;
    }
    g_pair.socket = Connect();
    if (!Send(g_pair.socket, "pair")) {
        ClosePairing();
        return;
    }
    g_pair.watch = g_unix_fd_add(g_pair.socket, G_IO_IN, OnPairData, nullptr);
    GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Connect an iPhone - MyCam");
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(window), 16);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    g_pair.code = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_pair.code, 280, 280);
    g_signal_connect(g_pair.code, "draw", G_CALLBACK(DrawQr), nullptr);
    GtkWidget* steps = gtk_label_new(
        "1.  Open the iPhone's Camera app and point it at this code. Tap the link that appears.\n"
        "2.  The first time, Safari says the connection isn’t private. Tap Show Details, then “visit this "
        "website”.\n"
        "3.  Tap Start and allow the camera.\n\n"
        "The iPhone and this PC must be on the same Wi-Fi. Then choose “MyCam” as the camera in any app.");
    gtk_label_set_line_wrap(GTK_LABEL(steps), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(steps), 52);
    gtk_label_set_xalign(GTK_LABEL(steps), 0);
    g_pair.note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_pair.note), 0);
    gtk_box_pack_start(GTK_BOX(box), g_pair.code, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), steps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), g_pair.note, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(window), box);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer) {
        g_pair.window = nullptr;
        ClosePairing();
    }), nullptr);
    g_pair.window = window;
    gtk_widget_show_all(window);
}

// --- The tray --------------------------------------------------------------------------------------------

struct Tray {
    AppIndicator* indicator = nullptr;
    GtkWidget* status = nullptr;
    GtkWidget* pause = nullptr;
    GtkWidget* back = nullptr;
    GtkWidget* front = nullptr;
    GtkWidget* quality = nullptr;
    GtkWidget* qualities[3] = {};
    GtkWidget* mirror = nullptr;
    GtkWidget* fill = nullptr;
    GtkWidget* iphone = nullptr;
    GtkWidget* reconnect = nullptr;
    GtkWidget* autostart = nullptr;
    GtkWidget* start = nullptr;
    int socket = -1;
    guint watch = 0;
    std::string buffer;
    TrayStatus last;
    bool updating = false; // Setting check marks from a status, not the user clicking them.
};

Tray g_tray;

void SetIcon(int id, const std::string& tooltip) {
    const std::string path = IconPath(id);
    if (!path.empty()) app_indicator_set_icon_full(g_tray.indicator, path.c_str(), tooltip.c_str());
    app_indicator_set_title(g_tray.indicator, tooltip.c_str());
}

void ShowStatus(const TrayStatus& s, bool running) {
    g_tray.updating = true;
    g_tray.last = s;
    const std::string headline = running ? s.headline : "MyCam isn't running";
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.status), ("MyCam: " + headline).c_str());
    SetIcon(running ? s.icon : kIconDisconnected, running ? "MyCam: " + headline + (s.detail.empty() ? "" : "\n" + s.detail)
                                                          : "MyCam: " + headline);
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_tray.pause), s.paused ? "Resume camera" : "Pause camera");
    gtk_widget_set_sensitive(g_tray.pause, running && s.canPause);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s.facing ? g_tray.front : g_tray.back), TRUE);
    for (GtkWidget* w : {g_tray.back, g_tray.front, g_tray.mirror, g_tray.fill, g_tray.iphone, g_tray.reconnect})
        gtk_widget_set_sensitive(w, running);
    gtk_widget_set_sensitive(g_tray.quality, running && s.linked);
    for (int q = 0; q < 3; ++q) {
        gtk_widget_set_sensitive(g_tray.qualities[q], s.qualityOk[q]);
        if (q == s.quality) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.qualities[q]), TRUE);
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.mirror), s.mirror);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.fill), s.fill);
    gtk_widget_set_visible(g_tray.start, !running);
    g_tray.updating = false;
}

void Disconnected();

gboolean OnData(gint fd, GIOCondition, gpointer) {
    char chunk[1024];
    const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0 || g_tray.buffer.size() > 16384) {
        g_tray.watch = 0;
        Disconnected();
        return G_SOURCE_REMOVE;
    }
    g_tray.buffer.append(chunk, size_t(n));
    for (size_t nl; (nl = g_tray.buffer.find('\n')) != std::string::npos; g_tray.buffer.erase(0, nl + 1)) {
        TrayStatus s;
        if (ParseTrayStatus(g_tray.buffer.substr(0, nl), &s)) ShowStatus(s, true);
    }
    return G_SOURCE_CONTINUE;
}

gboolean TryConnect(gpointer) {
    if (g_tray.socket >= 0) return G_SOURCE_CONTINUE;
    const int s = Connect();
    if (s < 0) return G_SOURCE_CONTINUE;
    if (!Send(s, "watch")) {
        close(s);
        return G_SOURCE_CONTINUE;
    }
    g_tray.socket = s;
    g_tray.watch = g_unix_fd_add(s, G_IO_IN, OnData, nullptr);
    return G_SOURCE_CONTINUE;
}

void Disconnected() {
    if (g_tray.watch) g_source_remove(g_tray.watch);
    if (g_tray.socket >= 0) close(g_tray.socket);
    g_tray.watch = 0;
    g_tray.socket = -1;
    g_tray.buffer.clear();
    ShowStatus(TrayStatus(), false); // Tries again every 2 s (main).
}

void Command(const std::string& line) {
    if (!g_tray.updating) Send(g_tray.socket, line);
}

GtkWidget* Item(GtkWidget* menu, const char* label, void (*onClick)()) {
    GtkWidget* item = gtk_menu_item_new_with_mnemonic(label);
    if (onClick) g_signal_connect_swapped(item, "activate", G_CALLBACK(onClick), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* Check(GtkWidget* menu, GSList** group, const char* label, void (*onToggle)(GtkCheckMenuItem*)) {
    GtkWidget* item = group ? gtk_radio_menu_item_new_with_mnemonic(*group, label) : gtk_check_menu_item_new_with_mnemonic(label);
    if (group) *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    g_signal_connect(item, "toggled", G_CALLBACK(onToggle), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

void Separator(GtkWidget* menu) { gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new()); }

bool Active(GtkCheckMenuItem* item) { return gtk_check_menu_item_get_active(item); }

GtkWidget* BuildMenu() {
    GtkWidget* menu = gtk_menu_new();
    g_tray.status = Item(menu, "MyCam", nullptr);
    gtk_widget_set_sensitive(g_tray.status, FALSE);
    g_tray.start = Item(menu, "_Start MyCam", [] { Systemctl("start"); });
    Separator(menu);
    g_tray.pause = Item(menu, "_Pause camera", [] { Command(g_tray.last.paused ? "pause 0" : "pause 1"); });
    Separator(menu);
    GSList* facing = nullptr;
    g_tray.back = Check(menu, &facing, "_Back camera", [](GtkCheckMenuItem* i) { if (Active(i)) Command("facing 0"); });
    g_tray.front = Check(menu, &facing, "_Front camera", [](GtkCheckMenuItem* i) { if (Active(i)) Command("facing 1"); });
    g_tray.quality = Item(menu, "_Quality", nullptr);
    GtkWidget* qualityMenu = gtk_menu_new();
    GSList* quality = nullptr;
    g_tray.qualities[0] = Check(qualityMenu, &quality, "720p", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 0"); });
    g_tray.qualities[1] = Check(qualityMenu, &quality, "1080p", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 1"); });
    g_tray.qualities[2] = Check(qualityMenu, &quality, "4K", [](GtkCheckMenuItem* i) { if (Active(i)) Command("quality 2"); });
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(g_tray.quality), qualityMenu);
    g_tray.mirror = Check(menu, nullptr, "_Mirror image", [](GtkCheckMenuItem* i) { Command(Active(i) ? "mirror 1" : "mirror 0"); });
    g_tray.fill = Check(menu, nullptr, "Fill the frame", [](GtkCheckMenuItem* i) { Command(Active(i) ? "fill 1" : "fill 0"); });
    Separator(menu);
    g_tray.iphone = Item(menu, "Connect an _iPhone…", ShowPairWindow);
    g_tray.reconnect = Item(menu, "Reconnect phone", [] { Command("reconnect"); });
    g_tray.autostart = Check(menu, nullptr, "Start at login", [](GtkCheckMenuItem* i) {
        if (!g_tray.updating) Systemctl(Active(i) ? "enable" : "disable");
    });
    Separator(menu);
    Item(menu, "Quit MyCam", [] {
        Systemctl("stop"); // Like Exit on Windows: the camera goes too. "Start at login" brings it back.
        gtk_main_quit();
    });
    gtk_widget_show_all(menu);
    return menu;
}

} // namespace
} // namespace mycam

int main(int argc, char** argv) {
    using namespace mycam;
    gtk_init(&argc, &argv);
    if (RuntimeDir().empty()) {
        fprintf(stderr, "mycam-tray: no XDG_RUNTIME_DIR; start it from the desktop session\n");
        return 1;
    }
    // One tray per session: a second start (autostart plus a menu launch, say) just leaves.
    const int lock = open((RuntimeDir() + "/mycam-tray.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 0;
    g_tray.indicator = app_indicator_new("mycam", "camera-web", APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_status(g_tray.indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(g_tray.indicator, GTK_MENU(BuildMenu()));
    ShowStatus(TrayStatus(), false);
    g_tray.updating = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tray.autostart), Systemctl("is-enabled") == 0);
    g_tray.updating = false;
    TryConnect(nullptr);
    g_timeout_add_seconds(2, TryConnect, nullptr);
    gtk_main();
    return 0;
}
