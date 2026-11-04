"""The Home Assistant configuration the MQTT tab generates is the one the docs show.

web/ui/mqtt.js haConfigText fills in a TV's topic prefix, device id, name and MAC address;
docs/integrations/home-assistant.md shows the same YAML for the defaults. Both are parsed, so
a change to one that the other does not follow fails here. Exits 77 (a skip) without Node or
PyYAML, which are not what the build itself needs.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("skipped: PyYAML is not installed")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[2]
NODE = shutil.which("node")
if not NODE:
    print("skipped: node is not installed")
    sys.exit(77)

SCRIPT = """
const fs = require('fs'), vm = require('vm');
const source = fs.readFileSync(process.argv[1], 'utf8');
const start = source.indexOf('function haConfigText');
const end = source.indexOf('// The saved settings, and the address');
const context = {};
vm.createContext(context);
vm.runInContext(source.slice(start, end), context);
const inputs = JSON.parse(process.argv[2]);
console.log(JSON.stringify(inputs.map(context.haConfigText)));
"""


def generate(*inputs):
    result = subprocess.run([NODE, "-e", SCRIPT, str(ROOT / "native-tv/web/ui/mqtt.js"),
                             json.dumps(inputs)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def documented():
    text = (ROOT / "docs/integrations/home-assistant.md").read_text()
    config = {}
    for block in re.findall(r"```yaml\n(.*?)```", text, re.S):
        parsed = yaml.safe_load(block)
        if isinstance(parsed, dict) and ("media_player" in parsed or
                                         parsed.get("switch", [{}])[0].get("platform") == "template"):
            config.update(parsed)
    return config


def without_names(config):
    """What a TV's own name changes: the docs name the player 'LG OLED TV'."""
    config = json.loads(json.dumps(config))
    for player in config["media_player"]:
        player.pop("name"), player.pop("unique_id")
    return config


failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


defaults, other, no_mac = generate(
    {"id": "lg_tv", "name": "LG TV", "prefix": "lgtv", "mac": "AA:BB:CC:DD:EE:FF", "address": ""},
    {"id": "den_tv", "name": "Den \"TV\"", "prefix": "home/tv", "mac": "00:11:22:aa:bb:cc",
     "address": "192.168.1.50"},
    {"id": "lg_tv", "name": "LG TV", "prefix": "lgtv", "mac": "", "address": ""})

shown = documented()
check(set(shown) == {"template", "switch", "media_player"},
      "the docs show the sensor, the switch and the media player: %s" % sorted(shown))
generated = yaml.safe_load(defaults)
check(without_names(generated) == without_names(shown),
      "the generated configuration for the defaults differs from the one in the docs")

parsed = yaml.safe_load(other)
text = other
check(parsed["media_player"][0]["name"] == 'Den "TV"', "a name with quotes survives as YAML")
check("lg_tv" not in text, "no default device id is left in a configuration for another TV")
check(text.count("den_tv") >= 10, "the entity ids start with the device id")
check('topic: "home/tv/command/volumeStep"' in text, "the topic prefix is used for the volume steps")
check('mac: "00:11:22:AA:BB:CC"' in text, "the MAC address is written in capitals")
check("192.168.1.50" in text.splitlines()[0], "the TV's address is in the heading")
check(list(parsed["switch"][0]["switches"]) == ["den_tv"], "the power switch is named for the device id")
check("{{" in text and text.count("{{") == text.count("}}"), "the templates are whole")

placeholder = yaml.safe_load(no_mac)
check("MAC address was not found" in no_mac.splitlines()[1] and 'mac: "AA:BB:CC:DD:EE:FF"' in no_mac,
      "a TV whose MAC address is not known gets a placeholder and a note")
check(without_names(placeholder) == without_names(generated),
      "a placeholder MAC changes nothing else")

if failures:
    print("\n".join("FAIL: " + failure for failure in failures))
    sys.exit(1)
print("Home Assistant configuration checks passed")
