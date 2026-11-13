# Krastor python tool runner (development.md.3).
# Invoked as: venv/Scripts/python.exe tools/python/runner.py
# Reads one JSON object on stdin (the LLM args + Krastor's _policy/_approved/
# _workspace), executes inline `code` OR `script` + `args`, and prints one
# JSON envelope on stdout: {"ok":true,"result":{"stdout":...,"error":...}}.
# The user code's own stdout/stderr are captured into the envelope; this
# script's own protocol output stays on the real stdout stream.

import io
import json
import sys
import traceback
import contextlib


def emit(obj):
    data = json.dumps(obj)
    # Write to the original stdout (fd 1): sys.stdout may be redirected by
    # the user code we are running.
    sys.__stdout__.write(data)
    sys.__stdout__.flush()


def main():
    raw = sys.stdin.read()
    try:
        req = json.loads(raw) if raw.strip() else {}
        if not isinstance(req, dict):
            raise ValueError("input must be a JSON object")
    except Exception as exc:  # noqa: BLE001
        emit({"ok": False, "error": "python tool: bad input JSON: %s" % exc})
        return

    # Self-enforcement: without Krastor's approval stamp nothing runs.
    if not req.get("_approved"):
        emit({"ok": False,
              "error": "python tool: refusing to run without Krastor "
                       "approval (run through the assistant)"})
        return

    code = req.get("code")
    script = req.get("script")
    args = req.get("args") or []
    if not isinstance(args, list):
        emit({"ok": False, "error": "python tool: 'args' must be a list"})
        return

    if bool(code) == bool(script):
        emit({"ok": False,
              "error": "python tool: provide exactly one of 'code' or "
                       "'script'"})
        return

    filename = "<krastor_code>"
    source = code
    if script:
        import os
        filename = script if os.path.isabs(script) else os.path.join(
            req.get("_workspace") or os.getcwd(), script)
        try:
            with open(filename, "r", encoding="utf-8") as fh:
                source = fh.read()
        except OSError as exc:
            emit({"ok": False, "error": "python tool: cannot read script: %s" % exc})
            return

    user_stdout = io.StringIO()
    user_stderr = io.StringIO()
    g = {"__name__": "__main__", "__file__": filename}
    old_argv = sys.argv
    sys.argv = [filename] + [str(a) for a in args]
    status = "ok"
    try:
        with contextlib.redirect_stdout(user_stdout), \
                contextlib.redirect_stderr(user_stderr):
            exec(compile(source, filename, "exec"), g)
    except SystemExit as exc:
        if exc.code not in (0, None):
            status = "error"
            user_stderr.write("SystemExit: %s\n" % exc.code)
    except BaseException:  # noqa: BLE001 - report any code failure as result
        status = "error"
        traceback.print_exc(file=user_stderr)
    finally:
        sys.argv = old_argv

    emit({"ok": True,
          "result": {"stdout": user_stdout.getvalue(),
                     "status": status,
                     "error": user_stderr.getvalue()}})


if __name__ == "__main__":
    main()
