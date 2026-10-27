#!/usr/bin/env python3
"""Write the AltStore / SideStore source for the newest iOS build to stdout.

A source is just this JSON at a fixed URL (the `realtime` release asset
altstore-source.json). Players add that URL once in AltStore or SideStore;
both then offer every new build as an update and sign it on the device, so
nothing is ever submitted anywhere. Only the newest build is listed: its
downloadURL is the dated copy on realtime-history, which never changes
contents, unlike the fixed-name file on `realtime`.

Inputs come from the CI environment (build-ios.yml, publish step).
"""
import json
import os

env = os.environ
repo = env["R"]
icon = ("https://raw.githubusercontent.com/" + repo +
        "/web/web_port/www/icon-512.png")

source = {
    "name": "Silent Hill: Online",
    "identifier": "com.silenthill.online.source",
    "subtitle": "Automatic iOS builds of Silent Hill: Online",
    "description": "Every change to Silent Hill: Online, built automatically. "
                   "These builds are not tested first, so back up your saves.",
    "iconURL": icon,
    "website": "https://github.com/" + repo,
    "apps": [{
        "name": "Silent Hill: Online",
        "bundleIdentifier": "com.silenthill.online",
        "developerName": "SlickAmogus",
        "subtitle": "Co-op Silent Hill on iPhone and iPad",
        "localizedDescription": "Silent Hill (1999), built from the decompilation, with "
                                "online co-op. You need your own disc image of the game.",
        "iconURL": icon,
        "tintColor": "7A1F1A",
        "versions": [{
            "version": env["VER"],
            "buildVersion": env["BUILD"],
            "date": env["DATE"],
            "localizedDescription": env.get("SUMMARY", ""),
            "downloadURL": "https://github.com/" + repo +
                           "/releases/download/realtime-history/" + env["DATED"],
            "size": int(env["SIZE"]),
            "minOSVersion": env["MINOS"],
        }],
        "appPermissions": {"entitlements": [], "privacy": {}},
    }],
    "news": [],
}
print(json.dumps(source, indent=2))
