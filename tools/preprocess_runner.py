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


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: preprocess_runner.py <request.json> <response.json>")

    request_path = Path(sys.argv[1])
    response_path = Path(sys.argv[2])
    request = _load_request(request_path)

    repo_root = Path(__file__).resolve().parents[2]
    preprocess_dir = repo_root / "preprocess_integration"
    sys.path.insert(0, str(preprocess_dir))

    from reprocess_classify_model1_0420 import run_inference_api  # noqa: E402

    result = run_inference_api(**request)
    _write_response(response_path, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
