"""Restore the licensed local SR175 mesh without distributing it in Git."""
import argparse
import hashlib
import shutil
from pathlib import Path

EXPECTED_SHA256 = "e56f0aaaa21e576c2e6f4ba1e2645343e0f8bd981fc75e69c820699ea5aee031"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Local R6.4 sr175.mesh")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != EXPECTED_SHA256:
        parser.error("Mesh SHA-256 does not match the R6.4 rig; use the matching licensed local asset")
    target = Path(__file__).resolve().parents[1] / "assets" / "sr175.mesh"
    target.parent.mkdir(parents=True, exist_ok=True)
    if source != target.resolve():
        shutil.copyfile(source, target)
    print(f"Verified R6.4 model: {target}")


if __name__ == "__main__":
    main()
