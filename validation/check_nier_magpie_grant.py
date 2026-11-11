#!/usr/bin/env python3
import json
import os
import urllib.request


def main() -> None:
    config_path = os.path.expanduser("~/.pi/agent/models.json")
    with open(config_path, encoding="utf-8") as config_file:
        config = json.load(config_file)
    provider = config["providers"]["magpie"]
    base_url = provider["baseUrl"].rstrip("/")
    request = urllib.request.Request(
        f"{base_url}/models",
        headers={"Authorization": f"Bearer {provider['apiKey']}"},
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        body = json.load(response)
        print(f"base_url={base_url}")
        print(f"http_status={response.status}")
    model_ids = {model.get("id") for model in body.get("data", [])}
    print(f"model_count={len(model_ids)}")
    if "group/deepseek" not in model_ids:
        raise SystemExit("Magpie grant does not expose group/deepseek")
    print("group_deepseek=available")


if __name__ == "__main__":
    main()
