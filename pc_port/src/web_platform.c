/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Browser-only platform queries. The page decides these before main() runs
 * (web_port/www/shell.js) and hands them over as environment variables, which
 * is the one channel Emscripten fills before the C runtime starts. */
#if defined(__EMSCRIPTEN__)

#include <stdlib.h>
#include <SDL.h>
#include <emscripten.h>
#include <emscripten/html5.h>

#include "pc_config.h"
#include "web_platform.h"

int Web_IsTouchDevice(void)
{
    const char* v = getenv("SH_WEB_TOUCH");

    return v != NULL && v[0] == '1';
}

void Web_CanvasSize(int* w, int* h)
{
    double cw = 0.0, ch = 0.0;

    if (emscripten_get_element_css_size("#canvas", &cw, &ch) == EMSCRIPTEN_RESULT_SUCCESS &&
        cw >= 1.0 && ch >= 1.0)
    {
        *w = (int)cw;
        *h = (int)ch;
    }
}

/* Runs as the event is queued, ahead of PsyCross's own handling, so it reads
 * the event rather than g_windowWidth. */
static int SDLCALL Web_ResizeWatch(void* userdata, SDL_Event* ev)
{
    (void)userdata;
    if (ev->type == SDL_WINDOWEVENT && ev->window.event == SDL_WINDOWEVENT_RESIZED &&
        ev->window.data1 > 0 && ev->window.data2 > 0)
    {
        g_PcConfig.windowWidth  = ev->window.data1;
        g_PcConfig.windowHeight = ev->window.data2;
    }
    return 1;
}

void Web_TrackWindowSize(void)
{
    SDL_AddEventWatch(Web_ResizeWatch, NULL);
}

/* The chat's touch typing (sh_net_chat.c). A phone browser raises its
 * keyboard only for a focused text field, and the game is a canvas, so a
 * hidden one takes the typing. It holds one space as a sentinel: a backspace
 * on an empty field fires no input event, but deleting the sentinel does.
 * Keys typed into it are kept from the window, where SDL would read them
 * again. iOS Safari focuses a field only inside a touch handler, so a touch
 * while typing is wanted (any tap) focuses it if the first try did not.
 * Codes in the queue: 8 backspace, 10 Enter, 27 Escape. String ops only: a
 * slash pair in EM_JS reads as a C comment. */
EM_JS(void, Web_TextBegin, (void), {
    var el = document.getElementById("shChatType");
    if (!el) {
        el = document.createElement("input");
        el.id = "shChatType";
        el.type = "text";
        el.setAttribute("autocomplete", "off");
        el.setAttribute("autocorrect", "off");
        el.setAttribute("autocapitalize", "off");
        el.setAttribute("spellcheck", "false");
        el.setAttribute("enterkeyhint", "send");
        el.style.cssText = "position:fixed;left:0;top:0;width:1px;height:1px;opacity:0;" +
                           "font-size:16px;border:0;padding:0;margin:0;z-index:-1;";
        document.body.appendChild(el);
        var reset = function () { el.value = " "; try { el.setSelectionRange(1, 1); } catch (e) {} };
        el.addEventListener("input", function () {
            var v = el.value;
            if (v.length === 0) Module.shTextQ += String.fromCharCode(8);
            else if (v.length > 1) Module.shTextQ += v.slice(1);
            reset();
        });
        el.addEventListener("keydown", function (e) {
            if (e.key === "Enter") { Module.shTextQ += String.fromCharCode(10); e.preventDefault(); }
            else if (e.key === "Escape") { Module.shTextQ += String.fromCharCode(27); e.preventDefault(); }
            e.stopPropagation();
        });
        el.addEventListener("keyup", function (e) { e.stopPropagation(); });
        el.addEventListener("keypress", function (e) { e.stopPropagation(); });
        document.addEventListener("touchend", function () {
            if (Module.shTextWant && document.activeElement !== el) el.focus();
        }, true);
        Module.shTextReset = reset;
    }
    Module.shTextWant = 1;
    Module.shTextQ = "";
    Module.shTextReset();
    el.focus();
});

EM_JS(void, Web_TextEnd, (void), {
    var el = document.getElementById("shChatType");
    Module.shTextWant = 0;
    Module.shTextQ = "";
    if (el) el.blur();
});

EM_JS(int, Web_TextTake, (char* out, int cap), {
    var q = Module.shTextQ || "";
    var n = Math.min(q.length, cap - 1), i, c;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        c = q.charCodeAt(i);
        if (c === 8 || c === 10 || c === 27) HEAPU8[out + i] = c;
        else HEAPU8[out + i] = (c >= 32 && c <= 126) ? c : 63;
    }
    Module.shTextQ = q.slice(n);
    return n;
});

#endif
