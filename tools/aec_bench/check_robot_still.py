#!/usr/bin/env python3
"""Was the robot driven around during an acoustic capture?

The Raspberry Pi *is* the robot, and `codex-loop-camera-check` periodically
drives it forward/backward/left/right. Any of that during a capture changes the
distance and angle between the Pi's speaker and the ESP32's microphone, which
is precisely the thing E2b/E3/E4 hold constant. A run that overlapped a move is
not comparable to one that did not, and the difference is invisible in the
recording itself -- it just looks like a quieter or louder take.

Rather than trying to stop another agent's loop, every capture checks
afterwards and is discarded if it was disturbed.

Usage:
    python3 check_robot_still.py <window_start_epoch> [window_end_epoch]

Exit status 0 = undisturbed, 1 = a move overlapped the window.
"""
import json
import sys
import time
import urllib.request

TASKS_URL = "https://www.wangyutang.cn/action/api/tasks?limit=40"


def task_epoch(task_id: str) -> float:
    # ids look like "1789406529135-b7918a"; the prefix is epoch milliseconds
    head = task_id.split("-", 1)[0]
    return int(head) / 1000.0 if head.isdigit() else 0.0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    start = float(sys.argv[1])
    end = float(sys.argv[2]) if len(sys.argv) > 2 else time.time()

    with urllib.request.urlopen(TASKS_URL, timeout=30) as fh:
        payload = json.load(fh)
    tasks = payload.get("tasks") or payload
    if not isinstance(tasks, list):
        print("unexpected task list shape; treating as disturbed to be safe")
        return 1

    moves = []
    for task in tasks:
        action = str(task.get("action", ""))
        if not action.startswith("move"):
            continue
        when = task_epoch(str(task.get("id", "")))
        # A move is only relevant if it landed inside the capture window. Allow
        # a couple of seconds either side: the task timestamp is when it was
        # queued, and the motion itself continues for a moment after.
        if start - 2 <= when <= end + 2:
            moves.append((action, when, task.get("source")))

    if moves:
        print(f"DISTURBED: {len(moves)} move(s) during the capture window")
        for action, when, source in moves:
            print(f"  {action} at {when:.0f} (source={source})")
        return 1

    print("undisturbed: no robot movement during the capture window")
    return 0


if __name__ == "__main__":
    sys.exit(main())
