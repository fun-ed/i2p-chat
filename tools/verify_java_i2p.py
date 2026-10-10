#!/usr/bin/env python3
"""Check pinned Java I2P artifacts under JDK 25 without starting a router."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request


I2P_VERSION = "2.13.0"
ARTIFACT_SHA256 = {
    "i2p": "d76b3c4c4fa7043c2a3553ab3209954e63151dfd1a90f7c020fbc58b33c89896",
    "router": "703b1e0af5a7695a85558d19fe936958e0834c34912d5ea1786575d0209011f4",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", choices=[I2P_VERSION])
    parser.add_argument("--artifact-dir", type=Path)
    args = parser.parse_args()

    java_version = subprocess.run(
        ["java", "-version"], capture_output=True, text=True, check=True
    )
    java_output = java_version.stdout + java_version.stderr
    if not re.search(r'version "25(?:\.|\")', java_output):
        raise RuntimeError("JDK 25 required; run this check through mise")
    print(java_output.strip())

    artifact_dir = args.artifact_dir or (
        Path(__file__).resolve().parents[1] / "build" / "java-i2p" / args.version
    )
    artifact_dir.mkdir(parents=True, exist_ok=True)
    jars = []
    for artifact, expected_hash in ARTIFACT_SHA256.items():
        jar = artifact_dir / f"{artifact}-{args.version}.jar"
        if not jar.exists():
            url = (
                f"https://repo.maven.apache.org/maven2/net/i2p/{artifact}/"
                f"{args.version}/{jar.name}"
            )
            with urllib.request.urlopen(url, timeout=60) as response, jar.open("xb") as out:
                shutil.copyfileobj(response, out)
        digest = hashlib.sha256(jar.read_bytes()).hexdigest()
        if digest != expected_hash:
            raise RuntimeError(f"SHA-256 mismatch: {jar.name}")
        print(f"SHA-256 OK: {jar.name}")
        jars.append(str(jar))

    classpath = os.pathsep.join(jars)
    for class_name, prefix in [
        ("net.i2p.CoreVersion", "I2P Core version: "),
        ("net.i2p.router.RouterVersion", "I2P Router version: "),
    ]:
        output = subprocess.check_output(
            ["java", "-cp", classpath, class_name], text=True
        )
        first_line = output.splitlines()[0]
        if not re.fullmatch(re.escape(prefix + args.version) + r"(?:-.*)?", first_line):
            raise RuntimeError(f"Unexpected version from {class_name}: {first_line}")
        print(output.strip())
    print("PASS: Java I2P 2.13.0 core/router version entry points under JDK 25")
    print("No router was started; this is not a live-network or peer test.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
