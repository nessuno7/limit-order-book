"""Localhost viewer for the market-making backtester.

    python ui/server.py [--port 8765]          (or `make ui` in cpp_engine/)

Serves ui/index.html and a small JSON API around cpp_engine's lob_backtest:
it runs the backtester with the parameters from the page, keeps every run's
output (summary.json, series.csv, fills.csv) under backtest_out/runs/, and
hands them back for plotting. Standard library only, and bound to 127.0.0.1.

    GET    /api/datasets      the synthetic profiles and the LOBSTER days in data/lobster/
    GET    /api/runs          every stored run: parameters and summary, newest first
    GET    /api/runs/<id>     one run, with its series and fills as columns
    GET    /api/runs/<id>/window?t0=NS&t1=NS
                              its series re-sampled finely over a window (the run is replayed again)
    POST   /api/runs          run the backtester; the body is the page's parameters
    DELETE /api/runs/<id>     forget a run
"""

import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"
RUNS = ROOT / "backtest_out" / "runs"
LOBSTER = ROOT / "data" / "lobster"
EXE = ROOT / "cpp_engine" / "build" / "release" / ("lob_backtest.exe" if sys.platform == "win32" else "lob_backtest")

PROFILES = ["tight", "deep", "volatile"]
RUN_ID = re.compile(r"^[0-9]{8}-[0-9]{6}-[0-9]+$")

# name: (type, minimum, maximum, default). Every run parameter the page may send.
PARAMS = {
    "size": (int, 1, 1_000_000, 10),
    "max_position": (int, 0, 10_000_000, 200),
    "order_latency_us": (int, 0, 10_000_000, 100),
    "md_latency_us": (int, 0, 10_000_000, 0),
    "markout_horizon_ms": (int, 1, 3_600_000, 1_000),
    "maker_fee": (float, -1_000.0, 1_000.0, 0.0),
    "taker_fee": (float, -1_000.0, 1_000.0, 0.0),
    "seed": (int, 0, 2**63 - 1, 1),
    "ops": (int, 1_000, 20_000_000, 1_000_000),
    "sample_ms": (int, 0, 3_600_000, 0),
}

_run_lock = threading.Lock()  # one backtest at a time; each one already uses a core flat out


def datasets():
    """Every feed the page can pick: id -> description and the arguments that select it."""
    out = {}
    for p in PROFILES:
        out[f"synthetic:{p}"] = {"label": f"Synthetic, {p} flow", "kind": "synthetic", "args": ["--profile", p]}
    if LOBSTER.is_dir():
        for msg in sorted(LOBSTER.glob("*_message_*.csv")):
            book = msg.with_name(msg.name.replace("_message_", "_orderbook_"))
            if not book.exists():
                continue
            m = re.match(r"^([A-Z.]+)_(\d{4}-\d{2}-\d{2})_\d+_\d+_message_(\d+)\.csv$", msg.name)
            label = f"LOBSTER {m.group(1)} {m.group(2)}, {m.group(3)} level{'' if m.group(3) == '1' else 's'}" if m else msg.name
            out[f"lobster:{msg.name}"] = {
                "label": label,
                "kind": "lobster",
                "args": ["--lobster", str(msg), "--lobster-book", str(book), "--price-divisor", "100"],
            }
    return out


def parse_params(body):
    params = {}
    for name, (kind, lo, hi, default) in PARAMS.items():
        raw = body.get(name, default)
        try:
            value = kind(raw)
        except (TypeError, ValueError):
            raise ValueError(f"{name} must be a number")
        if not lo <= value <= hi:
            raise ValueError(f"{name} must be between {lo} and {hi}")
        params[name] = value
    sets = datasets()
    params["dataset"] = body.get("dataset", "synthetic:tight")
    if params["dataset"] not in sets:
        raise ValueError(f"unknown dataset {params['dataset']}")
    params["label"] = str(body.get("label", ""))[:80]
    return params, sets[params["dataset"]]


def command(params, dataset, out_dir):
    args = [str(EXE), *dataset["args"]]
    if dataset["kind"] == "synthetic":
        args += ["--seed", str(params["seed"]), "--ops", str(params["ops"])]
    args += [
        "--size", str(params["size"]),
        "--max-position", str(params["max_position"]),
        "--latency-us", str(params["order_latency_us"]),
        "--md-latency-us", str(params["md_latency_us"]),
        "--horizon-ms", str(params["markout_horizon_ms"]),
        "--maker-fee", repr(params["maker_fee"]),
        "--taker-fee", repr(params["taker_fee"]),
        "--out", str(out_dir),
    ]
    if params["sample_ms"] > 0:
        args += ["--sample-ms", str(params["sample_ms"])]
    return args


def new_run_id():
    stamp = time.strftime("%Y%m%d-%H%M%S")
    n = 1
    while (RUNS / f"{stamp}-{n}").exists():
        n += 1
    return f"{stamp}-{n}"


def run_backtest(body):
    if not EXE.exists():
        raise RuntimeError(f"{EXE.relative_to(ROOT)} is missing: build it with `make ui` (or `make backtest`) in cpp_engine/")
    params, dataset = parse_params(body)
    with _run_lock:
        run_id = new_run_id()
        out_dir = RUNS / run_id
        out_dir.mkdir(parents=True)
        args = command(params, dataset, out_dir)
        started = time.monotonic()
        proc = subprocess.run(args, capture_output=True, text=True, timeout=600, cwd=ROOT / "cpp_engine")
        wall = time.monotonic() - started
    if proc.returncode != 0:
        shutil.rmtree(out_dir, ignore_errors=True)
        raise RuntimeError((proc.stderr or proc.stdout).strip() or f"lob_backtest exited with {proc.returncode}")
    meta = {"id": run_id, "params": params, "dataset_label": dataset["label"], "wall_seconds": wall,
            "stdout": proc.stdout, "command": args}
    (out_dir / "run.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return load_run(run_id, full=False)


def run_window(run_id, t0, t1):
    """The run's series sampled finely over [t0, t1] (feed time, ns). The
    backtest is deterministic, so it is re-run with the stored parameters and
    traced over that window only; a LOBSTER day replays in tens of ms."""
    if not (0 <= t0 < t1):
        raise ValueError("need 0 <= t0 < t1")
    meta = json.loads((RUNS / run_id / "run.json").read_text(encoding="utf-8"))
    params = dict(meta["params"], sample_ms=0)
    sets = datasets()
    if params["dataset"] not in sets:
        raise RuntimeError(f"the dataset of this run is gone: {params['dataset']}")
    out_dir = Path(tempfile.mkdtemp(prefix="window-", dir=RUNS / run_id))
    try:
        args = command(params, sets[params["dataset"]], out_dir) + ["--trace-from", str(t0), "--trace-to", str(t1)]
        with _run_lock:
            proc = subprocess.run(args, capture_output=True, text=True, timeout=600, cwd=ROOT / "cpp_engine")
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or proc.stdout).strip() or f"lob_backtest exited with {proc.returncode}")
        return {"id": run_id, "t0": t0, "t1": t1, "series": columns(out_dir / "series.csv")}
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)


def columns(path):
    """A CSV as {column: [values]}; numbers parsed, empty fields as None."""
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader)
        cols = [[] for _ in header]
        for row in reader:
            for i, v in enumerate(row):
                if v == "":
                    cols[i].append(None)
                elif v in ("buy", "sell"):
                    cols[i].append(v)
                else:
                    cols[i].append(float(v) if "." in v else int(v))
    return dict(zip(header, cols))


def load_run(run_id, full):
    d = RUNS / run_id
    meta = json.loads((d / "run.json").read_text(encoding="utf-8"))
    summary = json.loads((d / "summary.json").read_text(encoding="utf-8"))
    run = {"id": run_id, "params": meta["params"], "dataset_label": meta["dataset_label"], "summary": summary}
    if full:
        run["series"] = columns(d / "series.csv")
        run["fills"] = columns(d / "fills.csv")
        run["stdout"] = meta.get("stdout", "")
    return run


def list_runs():
    if not RUNS.is_dir():
        return []
    runs = []
    for d in sorted(RUNS.iterdir(), reverse=True):
        if RUN_ID.match(d.name) and (d / "run.json").exists() and (d / "summary.json").exists():
            try:
                runs.append(load_run(d.name, full=False))
            except (OSError, ValueError, KeyError):
                pass  # a damaged run is skipped, not fatal
    return runs


class Handler(BaseHTTPRequestHandler):
    server_version = "lob-ui/1"

    def log_message(self, fmt, *args):
        sys.stderr.write("  %s\n" % (fmt % args))

    def send_json(self, value, status=HTTPStatus.OK):
        data = json.dumps(value, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_error_json(self, status, message):
        self.send_json({"error": message}, status)

    def run_id(self):
        run_id = self.path.split("?", 1)[0][len("/api/runs/"):].removesuffix("/window")
        if not RUN_ID.match(run_id) or not (RUNS / run_id).is_dir():
            self.send_error_json(HTTPStatus.NOT_FOUND, "no such run")
            return None
        return run_id

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            data = (UI / "index.html").read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)
        elif path == "/api/datasets":
            sets = datasets()
            self.send_json({"datasets": [{"id": k, "label": v["label"], "kind": v["kind"]} for k, v in sets.items()],
                            "params": {k: v[3] for k, v in PARAMS.items()},
                            "executable": EXE.exists()})
        elif path == "/api/runs":
            self.send_json({"runs": list_runs()})
        elif path.startswith("/api/runs/") and path.endswith("/window"):
            run_id = self.run_id()
            if not run_id:
                return
            try:
                q = parse_qs(urlsplit(self.path).query)
                self.send_json(run_window(run_id, int(q["t0"][0]), int(q["t1"][0])))
            except (KeyError, ValueError) as e:
                self.send_error_json(HTTPStatus.BAD_REQUEST, f"bad window: {e}")
            except (RuntimeError, subprocess.TimeoutExpired) as e:
                self.send_error_json(HTTPStatus.INTERNAL_SERVER_ERROR, str(e))
        elif path.startswith("/api/runs/"):
            run_id = self.run_id()
            if run_id:
                self.send_json(load_run(run_id, full=True))
        else:
            self.send_error_json(HTTPStatus.NOT_FOUND, "not found")

    def do_POST(self):
        if self.path != "/api/runs":
            self.send_error_json(HTTPStatus.NOT_FOUND, "not found")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(body, dict):
                raise ValueError("the body must be a JSON object")
            self.send_json(run_backtest(body), HTTPStatus.CREATED)
        except ValueError as e:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(e))
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            self.send_error_json(HTTPStatus.INTERNAL_SERVER_ERROR, str(e))

    def do_DELETE(self):
        if not self.path.startswith("/api/runs/"):
            self.send_error_json(HTTPStatus.NOT_FOUND, "not found")
            return
        run_id = self.run_id()
        if run_id:
            shutil.rmtree(RUNS / run_id)
            self.send_json({"deleted": run_id})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"backtest viewer on http://127.0.0.1:{a.port}/  (Ctrl+C to stop)")
    if not EXE.exists():
        print(f"note: {EXE.relative_to(ROOT)} is missing; build it with `make ui` (or `make backtest`) in cpp_engine/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
