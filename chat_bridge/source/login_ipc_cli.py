"""Manual IPC integration client. start prints taskId BEFORE sending the request."""
import argparse
import asyncio
import json
import sys
import uuid

from login_bridge import LoginBridge
from login_protocol import PIPE_NAME


async def main(args):
    async with LoginBridge(args.pipe) as client:
        if args.command == "ping":
            result = await client.ping()
        elif args.command == "start":
            task_id = args.task_id or str(uuid.uuid4())
            print(json.dumps({"taskId": task_id, "notice": "Keep this ID for status queries"}), flush=True)
            result = await client.start(task_id=task_id, account_ref=args.account, server_id=args.server,
                                        character_id=args.character, timeout_seconds=args.timeout)
        elif args.command == "status":
            result = await client.status(args.task_id)
        elif args.command == "cancel":
            result = await client.cancel(args.task_id)
        else:
            result = await client.wait(args.task_id, timeout=args.timeout)
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="AION2 local login IPC v1")
    parser.add_argument("--pipe", default=PIPE_NAME)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("ping")
    start = commands.add_parser("start")
    start.add_argument("--task-id")
    start.add_argument("--account", required=True)
    start.add_argument("--server", required=True)
    start.add_argument("--character", required=True)
    start.add_argument("--timeout", type=int, default=180)
    for name in ("status", "cancel", "wait"):
        command = commands.add_parser(name)
        command.add_argument("task_id")
        if name == "wait":
            command.add_argument("--timeout", type=int, default=240)
    try:
        asyncio.run(main(parser.parse_args()))
    except Exception as error:
        print(f"{type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
