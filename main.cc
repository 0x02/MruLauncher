#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <set>
#include <unordered_map>
using namespace std;

#include <string.h>

#include "scx/Env.hpp"
#include "scx/Dir.hpp"
#include "scx/FileInfo.hpp"
#include "scx/StringHelper.hpp"

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

static const char* _myname = nullptr;

/*
 * command list
 */
typedef vector<string> cmd_list_t;
static cmd_list_t _cmds;

auto dbpath() -> string
{
    auto path = scx::Env::Get("HOME") + "/";
    auto dir = path + ".config/";
    auto info = scx::FileInfo(dir);
    if (info.Exists()) {
        if (info.Type() != scx::FileType::Directory)
            return "";
    } else {
        scx::Dir::MakeDir(dir, 0644);
    }
    path = dir + "mrulauncher.txt";
    return path;
}

auto loaddb() -> cmd_list_t
{
    fstream db;
    vector<string> dbfiles;

    stringstream ss;
    db.open(dbpath().c_str(), ios::in);
    if (db) {
        ss << db.rdbuf();
    }
    db.close();
    dbfiles = scx::String::Split(ss.str(), '\n');

    return dbfiles;
}

auto savedb(const cmd_list_t& list) -> void
{
    fstream db;
    db.open(dbpath().c_str(), ios::out);
    for (const auto& file: list) {
        db << file << endl;
    }
    db.close();
}

/*
 * xcb context
 */
struct xcb_context_t
{
    xcb_connection_t* conn;
    xcb_screen_t*     scrn;
    xcb_window_t      win;
    xcb_gcontext_t    gc;

    auto flush() -> void
    {
        xcb_flush(conn);
    }

    auto quit() -> void
    {
        xcb_flush(conn);
        xcb_disconnect(conn);
    }

    auto takefocus() -> void
    {
        xcb_set_input_focus(conn, XCB_NONE, win,
                            XCB_CURRENT_TIME);
    }

    auto savefocus() -> void
    {
        auto cookie = xcb_get_input_focus(conn);
        auto reply = xcb_get_input_focus_reply(conn, cookie, nullptr);
        focus = reply->focus;
        free(reply);
    }

    auto restorefocus() -> void
    {
        xcb_set_input_focus(conn, XCB_NONE, focus,
                            XCB_CURRENT_TIME);
    }

    auto drawbar(const char* str, uint8_t nch) -> void
    {
        xcb_image_text_8(conn,
                         nch,
                         win, gc,
                         0, 16,
                         str);
    }

private:
    xcb_window_t focus;

};

static xcb_context_t _xcbctx;

/*
 * menu bar context
 */
struct bar_ctx_t
{
    // buffer
    auto data() const -> const char*
    {
        return buf;
    }

    auto size() const -> int
    {
        return sizeof(buf) - 1;
    }

    // input
    auto txt() const -> string
    {
        return string(buf + banner.size(), chidx - banner.size());
    }

    auto addch(char ch) -> void
    {
        if (chidx - banner.size() < chmax) {
            buf[chidx++] = ch;
        }
        updatehint();
    }

    auto delch() -> void
    {
        if (chidx > banner.size()) {
            buf[--chidx] = ' ';
        }
        updatehint();
    }

    // completion
    auto complete() -> void
    {
        if (hints.empty())
            return;

        // clear old selection
        if (hintidx >= 0) {
            int off = banner.size() + chmax;
            for (int i = 0; i < hintidx; ++i) {
                const auto& cmd = hints[i];
                off += hintl.size() + cmd.size() + hintr.size();
            }
            memset(buf+off, ' ', hintl.size());
            off += hintl.size() + hints[hintidx].size();
            memset(buf+off, ' ', hintr.size());
        }

        // approach next hint
        if (++hintidx >= hints.size()) {
            hintidx = 0;
        }
        {
            int off = banner.size() + chmax;
            for (int i = 0; i < hintidx; ++i) {
                const auto& cmd = hints[i];
                off += hintl.size() + cmd.size() + hintr.size();
            }
            memcpy(buf+off, hintl.data(), hintl.size());
            off += hintl.size() + hints[hintidx].size();
            memcpy(buf+off, hintr.data(), hintr.size());
        }

        // set input text
        {
            const auto& cmd = hints[hintidx];
            int off = banner.size();
            memset(buf+off, ' ', chmax);
            memcpy(buf+off, cmd.data(), cmd.size());
            chidx = off + cmd.size();
        }
    }

    bar_ctx_t()
    {
        memset(buf, ' ', sizeof(buf));
        memcpy(buf, banner.data(), banner.size());
        chidx = banner.size();
    }

private:
    auto updatehint() -> void
    {
        // clear hint
        int off = banner.size() + chmax;
        memset(buf+off, ' ', sizeof(buf)-off);
        hintidx = -1;
        hints.clear();

        // update hint
        if (chidx > banner.size()) {
            const string& str = txt();
            for (size_t i = 0; i < _cmds.size(); ++i) {
                const auto& cmd = _cmds[i];
                int n = str.size();
                if (n > cmd.size()) // not found
                    continue;
                if (strncmp(cmd.c_str(), str.c_str(), n) == 0) {
                    int len = hintl.size() + cmd.size() + hintr.size();
                    if (off + len >= sizeof(buf))
                        break;
                    off += hintl.size();
                    memcpy(buf+off, cmd.c_str(), cmd.size());
                    off += cmd.size() + hintr.size();
                    hints.push_back(cmd); // record it
                }
            }
        }
    }

private:
    char buf[256];
    int chidx;
    cmd_list_t hints;
    int hintidx;
    const int chmax = 30;
    const string banner = "> ";
    const string hintl = " [ ";
    const string hintr = " ] ";
};

static bar_ctx_t _barctx;

/*
 * usage: mrulauncher update
 *
 */
auto update(int narg, char** args) -> void
{
    vector<string> paths;

    // setup search path
    {
        paths.reserve(narg);
        for (int i = 0; i < narg; ++i) {
            paths.push_back(args[i]);
        }
        if (paths.empty()) {
            const auto& envpath = scx::Env::Get("PATH");
            paths = scx::String::Split(envpath, ':');
        }
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    }

    vector<string> newfiles;
    newfiles.reserve(999);

    // search newfiles
    {
        string msg = "Search in following directories:";
        cout << msg << endl;
        for (size_t i = 0; i < paths.size(); ++i) {
            const auto& path = paths[i];
            cout << path << endl;
            scx::Dir::WalkDir(
                path, 
                [&newfiles, &path] (const string& name) { 
                    if (name != "." && name != "..") {
                        newfiles.push_back(name);
                    }
                }
            );
        }
        cout << string(msg.size(), '-') << endl;
    }

    // update database
    {
        string msg = "Statistics:";
        cout << msg << endl;

        auto dbfiles = loaddb();

        // drop obsolete
        int ndec = 0;
        set<string> fileset;
        for (const auto& file: newfiles) {
            fileset.insert(file);
        }
        auto pos = std::remove_if(
            dbfiles.begin(), dbfiles.end(),
            [&fileset, &ndec](const string& file) {
                if (fileset.find(file) == fileset.end()) {
                    ++ndec;
                    return true;
                }
                return false;
            }
        );
        dbfiles.erase(pos, dbfiles.end());

        // append new
        int ninc = 0;
        fileset.clear();
        for (const auto& file: dbfiles) {
            fileset.insert(file);
        }
        for (const auto& file: newfiles) {
            if (fileset.find(file) == fileset.end()) {
                dbfiles.push_back(file);
                ++ninc;
            }
        }

        savedb(dbfiles);

        cout << "+ " << ninc << endl;
        cout << "- " << ndec << endl;
        cout << "= " << dbfiles.size() << endl;
        cout << string(msg.size(), '-') << endl;
    }
}

 /* 
  * usage: mrulauncher help
  *
  */
auto help(int, char**) -> void
{
    cout << "Usage: " << endl;
    cout << _myname << endl;
    cout << "    Show menu on top of screen." << endl;
    cout << _myname << " update <path1> <path2> ..." << endl;
    cout << "    Update database, <path> is optional." << endl;
}

/*
 * usage: mrulauncher
 *
 */
static unordered_map<xcb_keycode_t, uint32_t> _codemap;

auto keypress(xcb_key_press_event_t* e) -> void
{
    //cout << hex << (int)e->detail << endl;
    auto iter = _codemap.find(e->detail);
    if (iter == _codemap.end())
        return;

    uint32_t key = iter->second;
    bool shift = e->state & XCB_MOD_MASK_SHIFT;

    /* get input character */
    {
        char ch;
        bool isch = false;
        if (key >= XK_0 && key <= XK_9) {
            ch = key - XK_0 + '0';
            isch = true;
        } else if (key >= XK_a && key <= XK_z) {
            ch = key - XK_a + (shift ? 'A' : 'a');
            isch = true;
        } else if (key == XK_period) {
            ch = '.';
            isch = true;
        } else if (key == XK_minus) {
            ch = shift ? '_' : '-';
            isch = true;
        } else if (key == XK_space) {
            ch = ' ';
            isch = true;
        }
        if (isch) {
            _barctx.addch(ch);
        }
    }

    /* handle special keys */
    switch (key) {
    case XK_Home: {
    }
        break;

    case XK_End: {
    }
        break;

    case XK_Tab: {
        _barctx.complete();
    }
        break;

    case XK_BackSpace: {
        _barctx.delch();
    }
        break;

    case XK_Delete: {
    }
        break;

    case XK_Return: {
        const auto& args = scx::String::Split(_barctx.txt(), ' ');
        if (args.empty())
            break;

        for (size_t i = 0; i < _cmds.size(); ++i) {
            const auto cmd = _cmds[i]; // need copy
            if (cmd != args[0]) continue;

            _cmds.erase(_cmds.begin()+i);
            _cmds.insert(_cmds.begin(), cmd);
            savedb(_cmds);

            _xcbctx.restorefocus();
            _xcbctx.quit();

            char** const argv = new char*[args.size() + 1];
            for (size_t i = 0; i < args.size(); ++i) {
                const auto arg = args[i];
                argv[i] = new char[arg.size()+1];
                memcpy(argv[i], arg.data(), arg.size());
                argv[i][arg.size()] = '\0';
            }
            argv[args.size()] = nullptr;
            execvp(argv[0],  argv);
            exit(-1);
        }
    }
        break;

    case XK_Escape: {
        _xcbctx.restorefocus();
        _xcbctx.quit();
        exit(0);
    }
        break;

    }

    _xcbctx.drawbar(_barctx.data(), _barctx.size());
    _xcbctx.flush();
}

auto show() -> void
{
    _cmds = loaddb();

    _xcbctx.conn = xcb_connect(nullptr, nullptr);
    auto setup = xcb_get_setup(_xcbctx.conn);
    auto iter = xcb_setup_roots_iterator(setup);
    _xcbctx.scrn = iter.data;

    /* init key code map */
    {
        auto ksymbols = xcb_key_symbols_alloc(_xcbctx.conn);
        auto regkey = [ksymbols] (uint32_t key) -> void {
            auto pcode = xcb_key_symbols_get_keycode(ksymbols, key);
            _codemap[*pcode] = key;
            free(pcode);
        };
        for (uint32_t key = XK_0; key <= XK_9; ++key) {
            regkey(key);
        }
        for (uint32_t key = XK_a; key <= XK_z; ++key) {
            regkey(key);
        }
        for (uint32_t key: { 
             XK_space, XK_minus, XK_period,
             XK_Home, XK_End, XK_Tab, 
             XK_BackSpace, XK_Delete, 
             XK_Return, XK_Escape }) {
            regkey(key);
        }
        xcb_key_symbols_free(ksymbols);
    }

    /* grab keyboard */
    for (int i = 0; i < 1000; ++i) {
        auto cookie = xcb_grab_keyboard(_xcbctx.conn, 1, _xcbctx.scrn->root,
                          XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        auto reply = xcb_grab_keyboard_reply(_xcbctx.conn, cookie, nullptr);
        if (reply != nullptr) {
            free(reply);
            break;
        } else {
            cout << "." << endl;
            usleep(1000);
        }
    }

    /* create main window */
    _xcbctx.win = xcb_generate_id(_xcbctx.conn);

    uint32_t mask;
    uint32_t values[5];
    mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    values[0] = _xcbctx.scrn->black_pixel;
    values[1] = 1;
    values[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_VISIBILITY_CHANGE;

    xcb_create_window(_xcbctx.conn,
                      _xcbctx.scrn->root_depth,
                      _xcbctx.win,
                      _xcbctx.scrn->root,
                      0, 0,
                      _xcbctx.scrn->width_in_pixels, 20,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      _xcbctx.scrn->root_visual,
                      mask, values);
    xcb_map_window(_xcbctx.conn, _xcbctx.win);

    _xcbctx.savefocus();
    _xcbctx.takefocus();

    /* init gc & font */
    auto fid = xcb_generate_id(_xcbctx.conn);
    const char* fname = "7x14";
    xcb_open_font(_xcbctx.conn, fid, strlen(fname), fname);

    _xcbctx.gc = xcb_generate_id(_xcbctx.conn);
    mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
    values[0] = _xcbctx.scrn->white_pixel;
    values[1] = _xcbctx.scrn->black_pixel;
    values[2] = fid;
    xcb_create_gc(_xcbctx.conn, _xcbctx.gc, _xcbctx.win, mask, values);

    xcb_close_font(_xcbctx.conn, fid);

    _xcbctx.drawbar(_barctx.data(), _barctx.size());
    _xcbctx.flush();

    /* event loop */
    while (true) {
        auto e = xcb_wait_for_event(_xcbctx.conn);
        if (e == nullptr)
            break;

        switch (e->response_type & ~0x80) {
        case XCB_EXPOSE: {
            cout << "exp" << endl;
        }
            break;

        case XCB_VISIBILITY_NOTIFY: {
            /*
            const static uint32_t values[] = { XCB_STACK_MODE_ABOVE };
            xcb_configure_window(_xcbctx.conn, _xcbctx.win,
                                 XCB_CONFIG_WINDOW_STACK_MODE,
                                 values);
            xcb_flush(_xcbctx.conn);
            */
            cout << "vis" << endl;
        }
            break;

        case XCB_KEY_PRESS: {
            auto ke = reinterpret_cast<xcb_key_press_event_t*>(e);
            keypress(ke);
        }
            break;
        }

        free(e);
    }
}

auto main(int argc, char* argv[]) -> int
{
    _myname = argv[0];
    if (argc == 1) {
        show();
    } else if (argc > 1) {
        const char* cmd = argv[1];
        int narg = argc - 2;
        char** args = argv + 2;
        if (::strcmp("update", cmd) == 0) {
            update(narg, args);
        } else {
            help(narg, args);
        }
    }
    return 0;
}
