/* A small VT100 subset for BusyBox's line editing, less, top and vi. */

(function () {
"use strict";

var MAX_LINES = 2000;
var ROWS = 24;

var KEYS = {
    Enter: "\r", Backspace: "\x7f", Tab: "\t", Escape: "\x1b",
    ArrowUp: "\x1b[A", ArrowDown: "\x1b[B", ArrowRight: "\x1b[C", ArrowLeft: "\x1b[D",
    Home: "\x1b[H", End: "\x1b[F", Delete: "\x1b[3~",
    PageUp: "\x1b[5~", PageDown: "\x1b[6~"
};

window.oveVt = function (screen, send) {
    var decoder = new TextDecoder("utf-8");
    var encoder = new TextEncoder();

    var lines = [""];
    var row = 0, col = 0;
    var esc = null;                /* null, "" after ESC, or "[..." in a CSI */
    var dirty = false;

    function setLine(text) {
        lines[row] = text;
    }

    function put(ch) {
        var line = lines[row];
        while (line.length < col) line += " ";
        setLine(line.slice(0, col) + ch + line.slice(col + 1));
        col++;
    }

    function newline() {
        row++;
        if (row >= lines.length) lines.push("");
        if (lines.length > MAX_LINES) {
            var drop = lines.length - MAX_LINES;
            lines.splice(0, drop);
            row -= drop;
        }
        col = 0;
    }

    /* Cursor addressing works on the last ROWS lines, the screen a full-screen
     * program such as top or vi draws on; what is above stays as scrollback. */
    function moveTo(r, c) {
        var top = Math.max(0, lines.length - ROWS);
        row = top + Math.min(Math.max(r, 0), ROWS - 1);
        while (lines.length <= row) lines.push("");
        col = Math.max(c, 0);
    }

    function csi(seq) {
        var final = seq[seq.length - 1];
        var params = seq.slice(1, -1).replace(/^\?/, "").split(";");
        var n = parseInt(params[0], 10) || 0;
        var top = Math.max(0, lines.length - ROWS);
        switch (final) {
        case "K":                  /* erase in line */
            if (n === 0) setLine(lines[row].slice(0, col));
            else if (n === 2) setLine("");
            break;
        case "J":                  /* erase in display */
            if (n === 0) {
                lines.length = row + 1;
                setLine(lines[row].slice(0, col));
            } else if (n === 2 || n === 3) {
                for (var i = top; i < lines.length; i++) lines[i] = "";
            }
            break;
        case "H": case "f":        /* cursor position, 1-based */
            moveTo((n || 1) - 1, (parseInt(params[1], 10) || 1) - 1);
            break;
        case "A": moveTo(row - top - (n || 1), col); break;
        case "B": moveTo(row - top + (n || 1), col); break;
        case "C": col += n || 1; break;
        case "D": col = Math.max(0, col - (n || 1)); break;
        case "G": col = Math.max(0, (n || 1) - 1); break;
        default: break;            /* colours, modes, scrolling: ignored */
        }
    }

    function step(code) {
        var ch = String.fromCharCode(code);
        if (esc !== null) {
            if (esc === "") {
                if (ch === "[") { esc = "["; return; }
                esc = null;        /* a two-byte escape: ignored */
                return;
            }
            esc += ch;
            if (code >= 0x40 && code <= 0x7e) { csi(esc); esc = null; }
            else if (esc.length > 32) esc = null;
            return;
        }
        switch (code) {
        case 0x1b: esc = ""; break;
        case 0x0d: col = 0; break;
        case 0x0a: newline(); break;
        case 0x08: col = Math.max(0, col - 1); break;
        case 0x09: do put(" "); while (col % 8); break;
        case 0x07: break;
        default:
            if (code >= 0x20) put(ch);
        }
    }

    function render() {
        dirty = false;
        var stick = screen.scrollTop + screen.clientHeight >= screen.scrollHeight - 4;
        var line = lines[row];
        var before = lines.slice(0, row).join("\n") + (row ? "\n" : "") + line.slice(0, col);
        var cursor = document.createElement("span");
        cursor.className = "cursor";
        cursor.textContent = line.charAt(col) || " ";
        var after = line.slice(col + 1) + (row + 1 < lines.length ? "\n" + lines.slice(row + 1).join("\n") : "");
        screen.replaceChildren(document.createTextNode(before), cursor, document.createTextNode(after));
        if (stick) screen.scrollTop = screen.scrollHeight;
    }

    function write(bytes) {
        var text = decoder.decode(bytes, { stream: true });
        for (var i = 0; i < text.length; i++) step(text.charCodeAt(i));
        if (dirty) return;
        dirty = true;
        requestAnimationFrame(render);
    }

    function type(text) {
        send(encoder.encode(text));
    }

    screen.addEventListener("keydown", function (e) {
        if (e.metaKey) return;
        var seq = null;
        if (e.ctrlKey && !e.altKey && e.key.length === 1) {
            var c = e.key.toUpperCase().charCodeAt(0);
            if (c >= 0x40 && c <= 0x5f) seq = String.fromCharCode(c - 0x40);
            else if (e.key === " ") seq = "\0";
            if (e.key === "c" && window.getSelection().toString()) return;  /* copy */
            if (e.key === "v") return;                                      /* paste */
        } else if (KEYS[e.key]) {
            seq = KEYS[e.key];
        } else if (e.key.length === 1 && !e.ctrlKey) {
            seq = (e.altKey ? "\x1b" : "") + e.key;
        }
        if (seq === null) return;
        type(seq);
        e.preventDefault();
        e.stopPropagation();
    });

    screen.addEventListener("paste", function (e) {
        type(e.clipboardData.getData("text").replace(/\r?\n/g, "\r"));
        e.preventDefault();
    });

    return { write: write, type: type, text: function () { return lines.join("\n"); } };
};
})();
