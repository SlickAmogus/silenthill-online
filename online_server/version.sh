#!/bin/sh
# Prints the server's version number: how many commits have changed its source,
# as `git log` on these files counts them. It goes up with every change to the
# server and with nothing else (a merge that leaves these files alone is not
# counted), and every clone with the whole history gets the same number, so the
# release build and a server stamped from a developer's clone agree on which
# is newer.
cd "$(dirname "$0")/.." || exit 1
git rev-list --count HEAD -- \
    online_server/sh_master.c \
    pc_port/include/sh_net_proto.h \
    pc_port/include/sh_net_memo_text.h \
    pc_port/include/sh_net_chat_filter.h
