"""Small direct named-pipe client for BlueprintBridge.

This avoids shell path rewriting issues with Unreal package paths such as /Game/...
Run from Windows Python while the Unreal editor/plugin is active.
"""

from __future__ import annotations

import argparse
import json
import struct
import uuid
from dataclasses import dataclass
from typing import Any, Mapping, Sequence


DEFAULT_PIPE = r"\\.\pipe\BlueprintBridge"


class BlueprintBridgeError(RuntimeError):
    def __init__(self, response: Mapping[str, Any]):
        self.response = response
        error = response.get("error") or {}
        super().__init__(f"{error.get('code', 'Error')}: {error.get('message', response)}")


@dataclass
class BlueprintBridgeClient:
    pipe: str = DEFAULT_PIPE
    auth_token: str | None = None
    version: int = 1
    validate_unknown_commands: bool = False

    def request(self, command: str, params: Mapping[str, Any] | None = None, *, request_id: str | None = None) -> dict[str, Any]:
        if self.validate_unknown_commands and command not in {"DescribeCommand", "Batch"}:
            self.describe_command(command)

        payload: dict[str, Any] = {
            "id": request_id or str(uuid.uuid4()),
            "version": self.version,
            "command": command,
            "params": dict(params or {}),
        }
        if self.auth_token:
            payload["authToken"] = self.auth_token

        response = self._send(payload)
        if not response.get("ok", False):
            raise BlueprintBridgeError(response)
        return response

    def result(self, command: str, params: Mapping[str, Any] | None = None) -> Any:
        return self.request(command, params).get("result")

    def describe_command(self, command: str) -> dict[str, Any]:
        return self.result("DescribeCommand", {"command": command})

    def describe_blueprint(self, asset: str) -> dict[str, Any]:
        return self.result("DescribeBlueprint", {"asset": asset})

    def describe_graph(self, asset: str, graph: str) -> dict[str, Any]:
        return self.result("DescribeGraph", {"asset": asset, "graph": graph})

    def describe_function(self, class_path: str, function: str) -> dict[str, Any]:
        return self.result("DescribeFunction", {"class": class_path, "function": function})

    def find_reflection_symbols(
        self,
        query: str,
        *,
        kinds: Sequence[str] | None = None,
        blueprint_callable_only: bool = True,
        include_engine: bool = False,
        include_project: bool = True,
        max_results: int = 50,
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "query": query,
            "blueprintCallableOnly": blueprint_callable_only,
            "includeEngine": include_engine,
            "includeProject": include_project,
            "maxResults": max_results,
        }
        if kinds:
            params["kinds"] = list(kinds)
        return self.result("FindReflectionSymbols", params)

    def check_delegate_compatibility(
        self,
        *,
        function: str,
        delegate_owner_class: str,
        delegate: str,
        asset: str | None = None,
        function_class: str | None = None,
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "function": function,
            "delegateOwnerClass": delegate_owner_class,
            "delegate": delegate,
        }
        if asset:
            params["asset"] = asset
        if function_class:
            params["functionClass"] = function_class
        return self.result("CheckDelegateCompatibility", params)

    def apply_graph_patch(
        self,
        *,
        asset: str,
        graph: str,
        nodes: Sequence[Mapping[str, Any]] | None = None,
        links: Sequence[Mapping[str, Any]] | None = None,
        defaults: Sequence[Mapping[str, Any]] | None = None,
        rollback_on_failure: bool = True,
        compile: bool = False,
    ) -> dict[str, Any]:
        return self.result(
            "ApplyGraphPatch",
            {
                "asset": asset,
                "graph": graph,
                "nodes": list(nodes or []),
                "links": list(links or []),
                "defaults": list(defaults or []),
                "rollbackOnFailure": rollback_on_failure,
                "compile": compile,
            },
        )

    def duplicate_function_graph(
        self,
        *,
        asset: str,
        source_graph: str,
        new_graph: str,
        renames: Mapping[str, str] | None = None,
        pin_renames: Mapping[str, str] | None = None,
        variable_renames: Mapping[str, str] | None = None,
        strict_renames: bool = True,
        compile: bool = False,
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "asset": asset,
            "sourceGraph": source_graph,
            "newGraph": new_graph,
            "strictRenames": strict_renames,
            "compile": compile,
        }
        if renames:
            params["renames"] = dict(renames)
        if pin_renames:
            params["pinRenames"] = dict(pin_renames)
        if variable_renames:
            params["variableRenames"] = dict(variable_renames)
        return self.result("DuplicateFunctionGraph", params)

    def batch(self, requests: Sequence[Mapping[str, Any]], *, rollback_on_failure: bool = True) -> dict[str, Any]:
        normalized = []
        for request in requests:
            child = dict(request)
            child.setdefault("id", str(uuid.uuid4()))
            child.setdefault("version", self.version)
            if self.auth_token and "authToken" not in child:
                child["authToken"] = self.auth_token
            normalized.append(child)
        return self.result("Batch", {"requests": normalized, "rollbackOnFailure": rollback_on_failure})

    def _send(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        frame = struct.pack("<I", len(data)) + data
        with open(self.pipe, "r+b", buffering=0) as pipe:
            pipe.write(frame)
            header = pipe.read(4)
            if len(header) != 4:
                raise RuntimeError("BlueprintBridge pipe closed before response length was read")
            (size,) = struct.unpack("<I", header)
            body = pipe.read(size)
            if len(body) != size:
                raise RuntimeError(f"BlueprintBridge pipe closed mid-response ({len(body)}/{size} bytes)")
        return json.loads(body.decode("utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description="Send one BlueprintBridge request over the named pipe.")
    parser.add_argument("command")
    parser.add_argument("params", nargs="?", default="{}", help="JSON params object")
    parser.add_argument("--pipe", default=DEFAULT_PIPE)
    parser.add_argument("--auth-token")
    args = parser.parse_args()

    client = BlueprintBridgeClient(pipe=args.pipe, auth_token=args.auth_token)
    response = client.request(args.command, json.loads(args.params))
    print(json.dumps(response, indent=2))


if __name__ == "__main__":
    main()
