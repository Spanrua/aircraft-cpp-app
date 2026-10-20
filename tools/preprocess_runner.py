from __future__ import annotations

import json
import sys
from pathlib import Path


def _load_request(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _write_response(path: Path, payload: dict) -> None:
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)


def find_preprocess_integration_dir(script_path: Path) -> Path | None:
    resolved_script_path = script_path.resolve()
    for ancestor in resolved_script_path.parents:
        candidate = ancestor / "preprocess_integration"
        if candidate.is_dir():
            return candidate
    return None


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: preprocess_runner.py <request.json> <response.json>")

    request_path = Path(sys.argv[1])
    response_path = Path(sys.argv[2])
    request = _load_request(request_path)

    preprocess_dir = find_preprocess_integration_dir(Path(__file__))
    if preprocess_dir is None:
        raise RuntimeError("未找到 preprocess_integration；请将其置于应用目录的上级工程目录中")
    sys.path.insert(0, str(preprocess_dir))

    from iqraw_preprocess_runner import run  # noqa: E402

    result = run(request)
    _write_response(response_path, result)
    return 0 if result.get("success") else 1


if __name__ == "__main__":
    raise SystemExit(main())
