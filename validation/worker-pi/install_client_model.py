"""Install the existing Adam model descriptor into the Nier Pi client profile."""

import argparse
import json
import os
from pathlib import Path
import shutil
import stat


parser = argparse.ArgumentParser()
parser.add_argument("profile", type=Path)
parser.add_argument("descriptor", type=Path)
arguments = parser.parse_args()
model = json.loads(arguments.descriptor.read_text())
if model["id"] != "group/deepseek":
    raise ValueError("Expected the existing group/deepseek client descriptor")

profile = json.loads(arguments.profile.read_text())
provider = profile["providers"]["magpie"]
if provider["api"] != "openai-completions":
    raise ValueError("The existing Magpie client must use openai-completions")
models = provider["models"]
matches = [entry for entry in models if entry["id"] == model["id"]]
if matches and matches != [model]:
    raise ValueError("An existing DeepSeek descriptor differs from Adam's definition")
if not matches:
    backup = arguments.profile.with_name("models.json.before-lance-c-deepseek")
    if backup.exists():
        raise FileExistsError("The client profile backup already exists")
    shutil.copy2(arguments.profile, backup)
    models.append(model)
    temporary = arguments.profile.with_name("models.json.lance-c.tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as output:
        json.dump(profile, output, indent=2)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())
    temporary.chmod(stat.S_IMODE(arguments.profile.stat().st_mode))
    temporary.replace(arguments.profile)

print("Registered existing client model group/deepseek with native thinking mapping")
