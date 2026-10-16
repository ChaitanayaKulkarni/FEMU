#!/usr/bin/env python3
"""Run CDL qtests with source provenance and retained evidence.

Case requirements deliberately include tests not yet implemented. A passing
subset cannot turn an incomplete T1-T22 implementation into a conforming one.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shlex
import subprocess
import time


CASES = {
    "T1": ["cdl-v2", "cdl-count-1", "cdl-count-256",
           "cdl-discovery-matrix", "cdl-create-negative"],
    "T2": ["cdl-class-aggregation"],
    "T3": [f"cdl-priority{reverse}-seed-{seed}"
           for reverse in ("", "-reverse") for seed in range(5)],
    "T4": [f"cdl-priority{reverse}-seed-{seed}"
           for reverse in ("", "-reverse") for seed in range(5)],
    "T5": [f"cdl-no-idle-credit-seed-{seed}" for seed in range(5)]
          + ["cdl-alone-progress"],
    "T6": [f"cdl-sq-count-seed-{seed}" for seed in range(5)],
    "T7": ["cdl-bypass-progress"],
    "T8": ["cdl-timing", "cdl-fetch-fairness"],
    "T9": ["cdl-timing", "cdl-saturation"],
    "T10": ["cdl-command-scope", "cdl-command-scope-bbssd",
            "cdl-command-scope-csd", "cdl-csi-alias", "cdl-zoned-alias"],
    "T11": ["cdl-v2", "cdl-lifecycle-delete"],
    "T12": ["cdl-feature-matrix", "cdl-generic-features"],
    "T13": ["cdl-feature-matrix", "cdl-saved-restart"],
    "T14": ["cdl-enable-barriers"],
    "T15": ["cdl-crash-recovery", "cdl-stop-crash"] + [f"cdl-store-fault-{point}" for point in (
        "txn-open", "txn-write", "txn-fsync", "txn-rename", "txn-dirsync",
        "store-open", "store-write", "store-fsync", "store-rename",
        "store-dirsync", "after-intent", "after-store", "before-unlink",
        "after-unlink")],
    "T16": ["cdl-saturation", "cdl-reset-readiness", "cdl-controller-isolation"],
    "T17": ["cdl-v2", "cdl-count-1", "cdl-count-256", "cdl-active-snapshot"],
    "T18": ["cdl-round-robin"],
    "T19": ["cdl-mixed-round-robin"],
    "T20": ["cdl-backpressure", "cdl-backend-ring-pressure",
            "cdl-completion-ring-pressure"],
    "T21": ["cdl-cursor-membership", "cdl-insertion-matrix", "cdl-lifecycle-delete"],
    "T22": ["cdl-excluded-turns", "cdl-semantic-dependency"],
    "T23": ["cdl-capacity-reference", "cdl-capacity-properties",
            "cdl-discovery-matrix"],
    "T24": ["cdl-capacity-reference", "cdl-capacity-aggregate", "cdl-capacity-bound"],
    "T25": ["cdl-capacity-reference", "cdl-capacity-aggregate", "cdl-capacity-bound",
            "cdl-lifecycle-delete", "cdl-insertion-matrix"],
    "T26": ["cdl-capacity-reference"],
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_json(path: Path, value: object) -> None:
    temp = path.with_suffix(path.suffix + ".new")
    temp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temp.replace(path)


def validate_trace(directory: Path) -> dict:
    expected = ["fetch", "validation", "admission", "dispatch",
                "backend_complete", "completion_check", "durable_commit", "cq_post"]
    errors = []
    attempts = 0
    completed = 0
    for trace_file in sorted(directory.glob("trace-*.log")):
        records = {}
        for line in trace_file.read_text(errors="replace").splitlines():
            match = re.search(r"femu_cdl_(\w+) (.*)", line)
            if not match:
                continue
            event, body = match.groups()
            fields = dict(re.findall(r"(\w+)=([^ ]+)", body))
            if not fields.get("store") or not fields.get("attempt"):
                errors.append(f"missing attempt identity: {line}")
                continue
            key = (fields["store"], fields["attempt"])
            records.setdefault(key, []).append((event, fields))
        for key, events in records.items():
            attempts += 1
            names = [event for event, _ in events]
            description = f"{trace_file.name}:{key[0]}/{key[1]}"
            timed = "admission" in names
            allowed = expected if timed else ["fetch", "validation"] + (
                ["bypass_dispatch"] if "bypass_dispatch" in names else []) + ["cq_post"]
            if timed and "backend_issue" in names:
                allowed = allowed.copy()
                allowed.insert(allowed.index("backend_complete"), "backend_issue")
            if timed and "persist_latency" in names:
                allowed = allowed.copy()
                allowed.insert(allowed.index("cq_post"), "persist_latency")
            if names != allowed[:len(names)]:
                errors.append(f"{description}: lifecycle order {names}")
                continue
            if "cq_post" in names:
                completed += 1
                if names != allowed:
                    errors.append(f"{description}: incomplete lifecycle before CQ")
            fields = dict(events)
            if timed:
                admission = fields["admission"]
                if admission.get("enabled") != "1" or int(admission["class"]) == 0:
                    errors.append(f"{description}: invalid timed admission")
                if "completion_check" in fields:
                    check = fields["completion_check"]
                    elapsed = int(check["elapsed"])
                    if (check["start"] != admission["start"] or
                            check["limit"] != admission["limit"] or
                            elapsed != int(check["stop"]) - int(check["start"])):
                        errors.append(f"{description}: inconsistent frozen timing")
                    if elapsed >= 0 and int(check["miss"]) != (elapsed > int(check["limit"])):
                        errors.append(f"{description}: incorrect duration boundary")
                    if elapsed < 0 and ("durable_commit" in fields or "cq_post" in fields):
                        errors.append(f"{description}: backward clock completed normally")
                if "cq_post" in fields and (fields["cq_post"]["status"] !=
                                               fields["backend_complete"]["status"]):
                    errors.append(f"{description}: CDL changed the ordinary status")
    return {"attempts": attempts, "completed": completed,
            "errors": errors, "status": "FAIL" if errors else "PASS"}


def validate_observations(name: str, directory: Path, lifecycle: dict) -> dict:
    errors = []
    observations = {}
    io_exempt = {"cdl-feature-matrix", "cdl-generic-features",
                 "cdl-count-1", "cdl-count-256", "cdl-discovery-matrix",
                 "cdl-reset-readiness", "cdl-capacity-reference",
                 "cdl-capacity-aggregate", "cdl-capacity-bound",
                 "cdl-capacity-properties"}
    if name.startswith("cdl-") and name not in io_exempt and lifecycle["attempts"] == 0:
        errors.append("no command lifecycle evidence for an I/O scenario")
    if name == "cdl-capacity-reference":
        identify = (directory / "identify-0.bin").read_bytes()
        config = (directory / "config-0.bin").read_bytes()
        stats = (directory / "statistics-0.bin").read_bytes()
        get = lambda image, off, size: int.from_bytes(image[off:off + size], "little")
        capacities = [get(identify, off, 2) for off in range(3104, 3114, 2)]
        if len(identify) != 4096 or capacities != [52, 52, 48, 8, 6]:
            errors.append("reference Identify size/capacities do not match")
        if [get(identify, off, 2) for off in (3080, 3082, 3084, 3086)] != [2, 1, 64, 7]:
            errors.append("reference Identify profile version/count do not match")
        if len(config) != 176 or len(stats) != 232 or any(
                [get(log, 4, 2), get(log, 6, 2)] != [2, 0] for log in (config, stats)):
            errors.append("reference logs must remain 2.0 with N=7 lengths")
        observations = {"identify_bytes": len(identify), "profile": [2, 1],
                        "classes": 7, "capacity_fields": capacities,
                        "log_versions": [2, 0], "log_lengths": [len(config), len(stats)]}
    if name == "cdl-fetch-fairness":
        fetches = []
        for file in sorted(directory.glob("trace-*.log")):
            for line in file.read_text(errors="replace").splitlines():
                if "femu_cdl_fetch " in line:
                    match = re.search(r"sqid=(\d+)", line)
                    if match:
                        fetches.append(int(match[1]))
        if len(fetches) != 66000:
            errors.append(f"expected 66000 fetches across three phases, got {len(fetches)}")
        else:
            phases = []
            for phase in range(3):
                window = fetches[phase * 22000 + 1000:phase * 22000 + 11000]
                counts = {str(qid): window.count(qid) for qid in (1, 2)}
                phases.append(counts)
                if phase < 2:
                    for offset in range(0, 10000, 1000):
                        if set(window[offset:offset + 1000]) != {1, 2}:
                            errors.append(f"phase {phase}: fetch starvation at {offset}")
            for qid in ("1", "2"):
                if abs(phases[0][qid] - phases[1][qid]) > 500:
                    errors.append(f"duration swap changed SQ{qid}'s fetch total beyond tolerance")
            observations = {"equal_qprio_phases": phases[:2],
                            "unequal_qprio_diagnostic": phases[2]}
    if name == "cdl-bypass-progress":
        dispatches = []
        for file in sorted(directory.glob("trace-*.log")):
            for line in file.read_text(errors="replace").splitlines():
                if "femu_cdl_dispatch " in line:
                    dispatches.append("timed")
                elif "femu_cdl_bypass_dispatch " in line:
                    dispatches.append("ordinary")
        if len(dispatches) != 154000:
            errors.append(f"expected 154000 service dispatches, got {len(dispatches)}")
        else:
            phases = []
            for phase in range(7):
                window = dispatches[phase * 22000 + 1000:phase * 22000 + 11000]
                phases.append({kind: window.count(kind) for kind in ("timed", "ordinary")})
                for offset in range(0, 10000, 1000):
                    if set(window[offset:offset + 1000]) != {"timed", "ordinary"}:
                        errors.append(f"opcode phase {phase}: service starvation at {offset}")
            observations = {"phases": phases}
    return {"status": "FAIL" if errors else "PASS", "errors": errors,
            "observations": observations}


def capture_source(repo: Path, out: Path, qemu: Path, qos: Path) -> dict:
    def git(*args: str) -> bytes:
        return subprocess.check_output(["git", "-C", str(repo), *args])

    patch = git("diff", "--binary", "HEAD")
    (out / "source.patch").write_bytes(patch)
    names = set(git("diff", "--name-only", "HEAD").decode().splitlines())
    names.update(git("ls-files", "--others", "--exclude-standard").decode().splitlines())
    files = {}
    for name in sorted(names):
        # Build outputs are captured separately; they are not source inputs.
        top = name.split("/", 1)[0]
        if name == "GNUmakefile" or (repo / top / "config-host.mak").is_file():
            continue
        source = repo / name
        if not source.is_file():
            continue
        data = source.read_bytes()
        target = out / "source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        files[name] = hashlib.sha256(data).hexdigest()
    documents = {}
    for name in ("CDL_SPECIFICATION_V2_DRAFT.md", "CDL_EXPERIMENTAL_V2_PROFILE.md",
                 "CDL_SPECIFICATION_CONFORMANCE.md",
                 "CDL_FEMU_IMPLEMENTATION_AND_VALIDATION_PLAN.md"):
        source = repo.parent / name
        if source.exists():
            documents[name] = digest(source)
            (out / name).write_bytes(source.read_bytes())
    build_files = {}
    for name in ("config.status", "config-host.mak",
                 "meson-info/intro-buildoptions.json"):
        source = qemu.parent / name
        if source.is_file():
            target = out / "build" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read_bytes())
            build_files[name] = digest(source)
    version = subprocess.run([str(qemu), "--version"], capture_output=True,
                             text=True, check=True).stdout
    (out / "qemu-version.txt").write_text(version)
    return {"head": git("rev-parse", "HEAD").decode().strip(),
            "branch": git("branch", "--show-current").decode().strip(),
            "patch_sha256": hashlib.sha256(patch).hexdigest(),
            "source_files": files, "documents": documents,
            "build_files": build_files, "qemu_version": version.strip(),
            "qemu_sha256": digest(qemu), "qos_sha256": digest(qos)}


def run_node(qos: Path, node: str, out: Path, env: dict,
             timeout: float) -> dict:
    name = node.rsplit("/", 1)[-1]
    directory = out / name
    directory.mkdir()
    stores = directory / "stores"
    stores.mkdir()
    child_env = env | {"TMPDIR": str(stores),
                       "QTEST_TRACE": shlex.quote(
                           f"enable=femu_cdl_*,file={directory / 'trace-%d.log'}"),
                       "QTEST_LOG": "1", "QTEST_LOG_DIR": str(directory),
                       "FEMU_CDL_ARTIFACT_DIR": str(directory)}
    argv = [str(qos), "-q", "-p", node]
    start = time.monotonic()
    result = {"node": node, "command": argv, "status": "FAIL",
              "transport": "pty" if child_env.get("QTEST_USE_PTY") else "socket"}
    save_json(directory / "input.json", result)
    with (directory / "stdout.log").open("wb") as stdout, \
            (directory / "stderr.log").open("wb") as stderr:
        proc = subprocess.Popen(argv, env=child_env, stdout=stdout,
                                stderr=stderr, start_new_session=True)
        try:
            result["exit_code"] = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            result["reason"] = "watchdog timeout"
            os.killpg(proc.pid, signal.SIGKILL)
            result["exit_code"] = proc.wait()
        finally:
            # Stop only descendants of this test process, including failed QEMU.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    output = (directory / "stdout.log").read_text(errors="replace")
    stderr_text = (directory / "stderr.log").read_text(errors="replace")
    trace = [line for line in stderr_text.splitlines() if "femu_cdl_" in line]
    for trace_file in sorted(directory.glob("trace-*.log")):
        trace.extend(line for line in trace_file.read_text(errors="replace").splitlines()
                     if "femu_cdl_" in line)
    (directory / "trace.log").write_text("\n".join(trace) + "\n")
    result["trace_records"] = len(trace)
    trace_result = validate_trace(directory)
    observation = validate_observations(name, directory, trace_result)
    save_json(directory / "observations.json", observation)
    trace_result["errors"].extend(observation["errors"])
    if trace_result["errors"]:
        trace_result["status"] = "FAIL"
    save_json(directory / "trace-validation.json", trace_result)
    result["trace_status"] = trace_result["status"]
    if result["exit_code"] == 0 and re.search(r"^ok \d+ " + re.escape(node) + r"$",
                                            output, re.MULTILINE) and not trace_result["errors"]:
        result["status"] = "PASS"
    result["artifacts"] = {
        str(p.relative_to(directory)): digest(p)
        for p in directory.rglob("*") if p.is_file() and p.name != "result.json"}
    save_json(directory / "result.json", result)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--match", help="regex selecting a diagnostic test subset")
    parser.add_argument("--regression", action="store_true",
                        help="run non-CDL FEMU nodes selected by --match")
    parser.add_argument("--pty", action="store_true")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    if args.regression and not args.match:
        parser.error("--regression requires an explicit --match selection")
    repo = Path(__file__).resolve().parents[3]
    build = args.build.resolve()
    qemu = build / "qemu-system-x86_64"
    qos = build / "tests/qtest/qos-test"
    if args.timeout <= 0 or not qemu.is_file() or not qos.is_file():
        parser.error("positive timeout and built QEMU/qos-test are required")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    manifest = capture_source(repo, out, qemu, qos)
    manifest["diagnostic_subset"] = bool(args.match)
    manifest["regression"] = args.regression
    save_json(out / "manifest.json", manifest)
    client = repo.parent / "nvme-cdl-femu-qemu-kernel/tools/cdl-ctrl.c"
    probe = out / "cdl-profile-probe"
    compile_probe = [os.environ.get("CC", "cc"), "-O2", "-Wall", "-Wextra", "-Werror",
                     "-std=gnu11", f'-DCDL_HOST_PARSER_SOURCE="{client}"',
                     str(repo / "hw/femu/tests/cdl-profile-probe.c"), "-o", str(probe)]
    compiled = subprocess.run(compile_probe, capture_output=True, text=True)
    (out / "profile-probe-build.stdout").write_text(compiled.stdout)
    (out / "profile-probe-build.stderr").write_text(compiled.stderr)
    if compiled.returncode:
        save_json(out / "results.json", {"conforming": False,
                  "reason": "host profile probe compilation failed",
                  "exit_code": compiled.returncode})
        print(compiled.stderr, flush=True)
        return 1
    manifest["profile_probe_sha256"] = digest(probe)
    manifest["host_parser_sha256"] = digest(client)
    (out / "cdl-ctrl.c").write_bytes(client.read_bytes())
    save_json(out / "manifest.json", manifest)
    env = os.environ | {"QTEST_QEMU_BINARY": str(qemu),
                        "QTEST_QEMU_IMG": str(build / "qemu-img"),
                        "FEMU_CDL_PROFILE_PROBE": str(probe)}
    if args.pty:
        env["QTEST_USE_PTY"] = "1"
    else:
        env.pop("QTEST_USE_PTY", None)
    discovery = subprocess.run([str(qos), "-l"], env=env, capture_output=True,
                               text=True, timeout=60)
    (out / "discovery.stdout").write_text(discovery.stdout)
    (out / "discovery.stderr").write_text(discovery.stderr)
    if discovery.returncode:
        save_json(out / "results.json", {"conforming": False,
                  "reason": "test discovery failed", "exit_code": discovery.returncode})
        return 1
    nodes = [line.strip() for line in discovery.stdout.splitlines()
             if line.startswith("/") and "/femu-tests/" in line and
             (("/femu-tests/cdl-" not in line) if args.regression else
              ("/femu-tests/cdl-" in line))]
    if not nodes:
        raise RuntimeError("no applicable FEMU tests discovered")
    names = {node.rsplit("/", 1)[-1]: node for node in nodes}
    selected = [node for node in nodes if not args.match or re.search(args.match, node)]
    if not selected:
        raise RuntimeError("selection matched no FEMU tests")
    results = {}
    for node in selected:
        name = node.rsplit("/", 1)[-1]
        print(f"RUN {name}", flush=True)
        results[name] = run_node(qos, node, out, env, args.timeout)
        print(f"{results[name]['status']} {name} "
              f"({results[name]['elapsed_seconds']}s)", flush=True)
        save_json(out / "node-results.json", results)
    cases = {}
    for case, required in CASES.items():
        missing = [name for name in required if name not in names]
        not_run = [name for name in required if name in names and name not in results]
        failed = [name for name in required if name in results
                  and results[name]["status"] != "PASS"]
        status = "FAIL" if failed else "INCOMPLETE" if missing or not_run else "PASS"
        cases[case] = {"status": status, "required_nodes": required,
                       "missing": missing, "not_run": not_run, "failed": failed}
    binaries_stable = (digest(qemu) == manifest["qemu_sha256"] and
                       digest(qos) == manifest["qos_sha256"])
    summary = {"conforming": binaries_stable and not args.match and all(
                   entry["status"] == "PASS" for entry in cases.values()),
               "binary_fingerprint_stable": binaries_stable,
               "cases": cases, "nodes": results,
               "declared_exception": "reported feature-save failure semantics",
               "limits": ["experimental wire allocations", "virtual timer only", "H1 separate"]}
    save_json(out / "results.json", summary)
    print("CONFORMING" if summary["conforming"] else "CONFORMANCE NOT ESTABLISHED",
          flush=True)
    # Diagnostic runs can succeed while explicitly retaining an incomplete gate.
    return 0 if binaries_stable and args.match and all(r["status"] == "PASS" for r in results.values()) \
        else 0 if summary["conforming"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
