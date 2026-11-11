#!/usr/bin/env python3
"""Writes the Steam Input action manifest and one default layout per controller
type into pc_port/assets/gamedata/steam_input/.

The online build initialises Steam under an app id that Steam treats as a
native Steam Input title, so Steam hides controllers from every other input
API and expects the game to read them through ISteamInput. The game hands
Steam this manifest (ISteamInput::SetInputActionManifestFilePath) and reads
back one action per pad button, stick and trigger; pc_port/src/net/
sh_net_steam_input.c turns those into an ordinary game controller, so the
game's own bind screens stay in charge of what each button does.

The action names here are the ones that file asks Steam for. Run after
changing them:
    python pc_port/tools/gen_steam_input.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "pc_port", "assets", "gamedata", "steam_input")

SET = "sh_pad"
BUTTONS = [
    ("pad_a", "A / Cross"), ("pad_b", "B / Circle"), ("pad_x", "X / Square"), ("pad_y", "Y / Triangle"),
    ("pad_lb", "Left bumper"), ("pad_rb", "Right bumper"), ("pad_back", "Back / Select"), ("pad_start", "Start"),
    ("pad_ls", "Left stick click"), ("pad_rs", "Right stick click"),
    ("pad_up", "D-pad up"), ("pad_down", "D-pad down"), ("pad_left", "D-pad left"), ("pad_right", "D-pad right"),
]
STICKS = [("left_stick", "Left stick"), ("right_stick", "Right stick")]
TRIGGERS = [("left_trigger", "Left trigger"), ("right_trigger", "Right trigger")]

# Steam's name for each controller family it keeps a separate layout for.
TYPES = ["controller_xbox360", "controller_xboxone", "controller_xboxelite", "controller_ps4", "controller_ps5",
         "controller_ps5_edge", "controller_switch_pro", "controller_neptune", "controller_generic"]


class Vdf:
    def __init__(self):
        self.lines = []
        self.depth = 0

    def kv(self, k, v):
        self.lines.append("\t" * self.depth + '"%s"\t\t"%s"' % (k, v))

    def open(self, k):
        self.lines.append("\t" * self.depth + '"%s"' % k)
        self.lines.append("\t" * self.depth + "{")
        self.depth += 1

    def close(self):
        self.depth -= 1
        self.lines.append("\t" * self.depth + "}")

    def text(self):
        return "\n".join(self.lines) + "\n"


def label(name):
    return "#Action_" + name


def localization(v):
    v.open("localization")
    v.open("english")
    v.kv("Set_Pad", "Controller")
    for name, text in BUTTONS + STICKS + TRIGGERS:
        v.kv("Action_" + name, text)
    v.close()
    v.close()


def manifest():
    v = Vdf()
    v.open("Action Manifest")
    v.open("configurations")
    for t in TYPES:
        v.open(t)
        v.open("0")
        v.kv("path", t + ".vdf")
        v.close()
        v.close()
    v.close()
    v.open("actions")
    v.open(SET)
    v.kv("title", "#Set_Pad")
    v.open("StickPadGyro")
    for name, _ in STICKS:
        v.open(name)
        v.kv("title", label(name))
        v.kv("input_mode", "joystick_move")
        v.close()
    v.close()
    v.open("AnalogTrigger")
    for name, _ in TRIGGERS:
        v.kv(name, label(name))
    v.close()
    v.open("Button")
    for name, _ in BUTTONS:
        v.kv(name, label(name))
    v.close()
    v.close()
    v.close()
    localization(v)
    v.close()
    return v.text()


def press(v, source, action):
    v.open(source)
    v.open("activators")
    v.open("Full_Press")
    v.open("bindings")
    v.kv("binding", "game_action %s %s, %s" % (SET, action, label(action)))
    v.close()
    v.close()
    v.close()
    v.close()


def group(v, gid, mode, inputs, analog=None):
    v.open("group")
    v.kv("id", str(gid))
    v.kv("mode", mode)
    v.open("inputs")
    for source, action in inputs:
        press(v, source, action)
    v.close()
    if analog:
        v.open("gameactions")
        v.kv(SET, analog)
        v.close()
    v.close()


def layout(ctype):
    v = Vdf()
    v.open("controller_mappings")
    v.kv("version", "3")
    v.kv("revision", "1")
    v.kv("title", "Silent Hill PC")
    v.kv("description", "Every button, stick and trigger as the game's own controller settings see it.")
    v.kv("creator", "0")
    v.kv("controller_type", ctype)
    v.open("actions")
    v.open(SET)
    v.kv("title", "#Set_Pad")
    v.kv("legacy_set", "0")
    v.close()
    v.close()
    localization(v)
    group(v, 0, "four_buttons", [("button_a", "pad_a"), ("button_b", "pad_b"), ("button_x", "pad_x"), ("button_y", "pad_y")])
    group(v, 1, "dpad", [("dpad_north", "pad_up"), ("dpad_south", "pad_down"), ("dpad_east", "pad_right"), ("dpad_west", "pad_left")])
    group(v, 2, "joystick_move", [("click", "pad_ls")], "left_stick")
    group(v, 3, "joystick_move", [("click", "pad_rs")], "right_stick")
    group(v, 4, "trigger", [], "left_trigger")
    group(v, 5, "trigger", [], "right_trigger")
    group(v, 6, "switches", [("button_escape", "pad_start"), ("button_menu", "pad_back"),
                             ("left_bumper", "pad_lb"), ("right_bumper", "pad_rb")])
    v.open("preset")
    v.kv("id", "0")
    v.kv("name", SET)
    v.open("group_source_bindings")
    v.kv("6", "switch active")
    v.kv("0", "button_diamond active")
    v.kv("1", "dpad active")
    v.kv("2", "joystick active")
    v.kv("3", "right_joystick active")
    v.kv("4", "left_trigger active")
    v.kv("5", "right_trigger active")
    v.close()
    v.close()
    v.close()
    return v.text()


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "steam_input_manifest.vdf"), "w", encoding="utf-8", newline="\n") as f:
        f.write(manifest())
    for t in TYPES:
        with open(os.path.join(OUT, t + ".vdf"), "w", encoding="utf-8", newline="\n") as f:
            f.write(layout(t))
    print("wrote %d files to %s" % (len(TYPES) + 1, OUT))


if __name__ == "__main__":
    main()
