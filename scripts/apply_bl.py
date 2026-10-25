#!/usr/bin/env python3
"""给账号下属全部云存档写入 bl=1（10000 倍伤害镇压）。

用法:
  python scripts/apply_bl.py <acc> [saves_dir]
  python scripts/apply_bl.py 13123383238
  python scripts/apply_bl.py 13123383238 /opt/jh-server/data/saves
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from jh_xxtea import apply_bl, decrypt_dat, get_dat_cipher


def iter_saves(saves_dir: Path, acc: str) -> list[tuple[Path, int]]:
    out: list[tuple[Path, int]] = []
    prefix = acc + "_"
    for path in sorted(saves_dir.glob(prefix + "*.sav")):
        stem = path.stem  # acc_area
        area_str = stem[len(prefix) :]
        try:
            area = int(area_str)
        except ValueError:
            continue
        out.append((path, area))
    return out


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__.strip())
        sys.exit(2)
    acc = sys.argv[1]
    saves_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parent.parent / "data" / "saves"
    saves = iter_saves(saves_dir, acc)
    if not saves:
        print(f"no saves for acc={acc} in {saves_dir}")
        sys.exit(1)

    changed = 0
    for path, area in saves:
        blob = path.read_text(encoding="utf-8", errors="replace")
        new_blob, did = apply_bl(blob, area)
        mode, plain = decrypt_dat(get_dat_cipher(new_blob), area)
        doc = json.loads(plain)
        if did:
            bak = path.with_suffix(path.suffix + ".pre_bl.bak")
            if not bak.exists():
                shutil.copy2(path, bak)
            path.write_text(new_blob, encoding="utf-8", newline="\n")
            # 同步 meta 里的 save_bytes
            meta_path = path.with_suffix(".json")
            if meta_path.exists():
                try:
                    meta = json.loads(meta_path.read_text(encoding="utf-8"))
                    meta["save_bytes"] = len(new_blob.encode("utf-8"))
                    meta_path.write_text(
                        json.dumps(meta, ensure_ascii=False, separators=(",", ":")),
                        encoding="utf-8",
                        newline="\n",
                    )
                except Exception as ex:
                    print(f"warn: meta update failed {meta_path.name}: {ex}")
            changed += 1
            print(f"OK  area={area} mode={mode} wrote bl -> {path.name}")
        else:
            print(f"SKIP area={area} mode={mode} already has bl={doc.get('bl')} -> {path.name}")

    print(f"done acc={acc} saves={len(saves)} changed={changed}")


if __name__ == "__main__":
    main()
