#!/usr/bin/env python3
"""Fail if the local configuration page can put a credential on screen.

THE BUG THIS EXISTS TO PREVENT

config_ui.html shows every frame it sends and receives in a traffic log, which
is the thing a person screenshots into a bug report. The first version of that
log printed frames verbatim:

    logLine("rx", JSON.stringify(m));

and `hub.network.get` answers with the device's stored Wi-Fi PSK, as `key`
beside `ssid` -- see ezlopi_cloud_network.c, where the wire format is left
unchanged on purpose because the Ezlo app reads it. So merely opening the Wi-Fi
tab wrote the PSK into the page. The outbound half leaked too: the log
special-cased hub.offline.login.ui's token and missed the other two methods
that carry a credential in their params, hub.network.wifi.try_connect
(network.key) and hub.mqtt.config.set (password).

It tested clean, because the stand-in device used to exercise the page did not
send a `key` in its hub.network.get reply. A harness that cannot fail does not
pass -- the stand-in now sends one.

WHY A CHECK

Same reason as check_no_secret_logging.py: the four leaks were one class, found
by going looking rather than by the suite, and nothing about fixing them stops
a fifth. The failure mode is specifically a quiet one -- adding
`logLine("rx", JSON.stringify(frame))` back looks like ordinary debugging and
breaks nothing visible.

WHAT COUNTS AS A VIOLATION

Three things, each the shape of a leak that actually happened:

  1. JSON.stringify() applied directly to a frame-shaped identifier, without
     redact(), on a line that also feeds a SINK -- the log or the DOM.
     Frame-shaped means one of FRAME_VARS, the names this page uses for a whole
     frame or a whole result. The sink requirement is what separates a leak
     from `this.ws.send(JSON.stringify(req))`, which is the wire and must stay
     verbatim: redacting there would send "***" to the device as the password.
  2. redact() or logFrame() missing from the file, or logFrame() not routing
     through redact(). Someone "simplifying" the helper away.
  3. A credential field name absent from SECRET_FIELDS, or the ssid-qualified
     "key" rule gone.

LIMITS

This is a text check over one file, not a JavaScript parser. It cannot see a
leak built by string concatenation, or one that renames the frame variable to
something not in FRAME_VARS, and it says nothing about whether redact() is
*correct* -- that is pinned by the redact() cases in the page's own comment and
was verified in a browser against a stand-in that really does send a PSK. What
it does catch is the direct re-introduction, which is the form the bug took.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PAGE = os.path.join(ROOT, "ezlopi-services", "ezlopi-service-ws-server",
                    "config_ui.html")

# Identifiers this page uses for a whole frame or a whole result. Anything
# stringified from one of these reaches the screen.
FRAME_VARS = ["m", "req", "r", "m.result", "ev.data", "frame", "msg"]

# Calls that put their argument where a person can read it. A stringified frame
# is only a leak if it reaches one of these; `ws.send` is the wire, not a sink,
# and must keep sending the real values.
SINKS = ["logLine(", "logFrame(", "say(", "textContent", "innerText",
         "innerHTML", "el("]

# Field names that must stay in the page's own SECRET_FIELDS list.
REQUIRED_SECRET_FIELDS = ["token", "password", "passphrase", "psk"]


def fail(problems):
    for p in problems:
        print("FAIL: %s" % p)
    print("\n%d problem(s). See the docstring in %s"
          % (len(problems), os.path.basename(__file__)))
    return 1


def main():
    if not os.path.isfile(PAGE):
        print("FAIL: page not found: %s" % PAGE)
        return 1

    with open(PAGE, encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    problems = []

    # (1) A frame stringified without redact(). Checked per line, with block
    # comments skipped -- the file's own comments quote the broken form on
    # purpose, to explain what not to do.
    in_block = False
    for n, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith("/*"):
            in_block = not stripped.endswith("*/")
            continue
        if in_block:
            if stripped.endswith("*/"):
                in_block = False
            continue
        if stripped.startswith("//") or stripped.startswith("**"):
            continue

        # The wire, not a sink. Checked before the sink window below, because
        # the send sits directly under the logFrame() call it pairs with and
        # would otherwise be swept in by it.
        if ".send(" in line:
            continue

        # The sink and the stringify can sit on different physical lines, as
        # the broadcast log line does, so look at this line and the one above.
        window = (lines[n - 2] if n >= 2 else "") + line
        if not any(s in window for s in SINKS):
            continue

        for var in FRAME_VARS:
            # JSON.stringify(m)  /  JSON.stringify(m, null, 2)
            bad = re.search(r'JSON\.stringify\(\s*' + re.escape(var)
                            + r'\s*[,)]', line)
            if bad:
                problems.append(
                    "config_ui.html:%d: JSON.stringify(%s) without redact(), "
                    "on a line that writes to the page -- a whole frame "
                    "reaching the screen unredacted.\n"
                    "        %s" % (n, var, stripped))

    text = "\n".join(lines)

    # (2) The helpers still exist and are still wired to each other.
    if not re.search(r'\bfunction\s+redact\s*\(', text):
        problems.append("config_ui.html: function redact() is gone.")
    if not re.search(r'\bfunction\s+logFrame\s*\(', text):
        problems.append("config_ui.html: function logFrame() is gone.")
    else:
        body = text.split("function logFrame")[1].split("}")[0]
        if "redact(" not in body:
            problems.append(
                "config_ui.html: logFrame() no longer calls redact(), so every "
                "frame it logs is verbatim.")

    # (3) The field list and the ssid-qualified "key" rule.
    decl = re.search(r'const\s+SECRET_FIELDS\s*=\s*\[(.*?)\]', text, re.S)
    if not decl:
        problems.append("config_ui.html: SECRET_FIELDS is gone.")
    else:
        listed = re.findall(r'"([^"]+)"', decl.group(1))
        for want in REQUIRED_SECRET_FIELDS:
            if want not in listed:
                problems.append(
                    'config_ui.html: SECRET_FIELDS no longer lists "%s". '
                    "Three local-API methods carry a credential in their "
                    "params and hub.network.get returns one; see CHANGELOG.md "
                    "under Credentials." % want)

    if not re.search(r'"key"\s*&&\s*wifiShaped', text):
        problems.append(
            'config_ui.html: the ssid-qualified "key" rule is gone. '
            "hub.network.get reports the stored Wi-Fi PSK as `key` beside "
            "`ssid` (ezlopi_cloud_network.c), so that pair must stay blanked.")

    if problems:
        return fail(problems)

    print("OK: config_ui.html routes frames through redact(); "
          "SECRET_FIELDS covers %s plus ssid-qualified `key`."
          % ", ".join(REQUIRED_SECRET_FIELDS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
