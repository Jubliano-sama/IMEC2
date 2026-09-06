#!/usr/bin/env python3
"""Compile the exact current BLE completion custody classifier and record outputs."""

import hashlib
import json
import pathlib
import subprocess
import tempfile


def main():
    here = pathlib.Path(__file__).parent
    repo = here.parents[3]
    origin = repo / "firmware/app/src/app_gateway_ble.c"
    original = origin.read_text()
    signature = "static bool gateway_host_custody_supported(const struct proto_packet *packet)"
    start = original.index(signature)
    end = original.index("\n}\n", start) + 3
    function = original[start:end]
    source_sha256 = hashlib.sha256(origin.read_bytes()).hexdigest()
    extracted = here / "ble_survey_classification_extracted.c"
    extracted.write_text(
        "/* Exact extraction from firmware/app/src/app_gateway_ble.c.\n"
        f" * Source SHA256: {source_sha256}\n */\n"
        '#include "protocol.h"\n#include <stdio.h>\n\n' + function +
        '\nint main(void)\n{\n'
        '    struct proto_packet packet = {.flags = FLAG_GATEWAY_ACK_REQUIRED};\n'
        '    packet.msg_type = MSG_SURVEY_EVENT;\n'
        '    printf("ACK-required MSG_SURVEY_EVENT: %d\\n",\n'
        '           gateway_host_custody_supported(&packet));\n'
        '    packet.msg_type = MSG_COMMAND_RESULT;\n'
        '    printf("ACK-required MSG_COMMAND_RESULT: %d\\n",\n'
        '           gateway_host_custody_supported(&packet));\n'
        '    return 0;\n}\n')
    with tempfile.TemporaryDirectory(prefix="imec-ble-survey-classification-") as work:
        binary = pathlib.Path(work) / "probe"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
             "-I", str(repo / "firmware/include"), str(extracted),
             "-o", str(binary)], check=True, timeout=10)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                check=True, timeout=1)
    report = {
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo,
                                        text=True).strip(),
        "source": str(origin.relative_to(repo)),
        "source_line": original[:start].count("\n") + 1,
        "source_sha256": source_sha256,
        "function_sha256": hashlib.sha256(function.encode()).hexdigest(),
        "extracted_sha256": hashlib.sha256(extracted.read_bytes()).hexdigest(),
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
    (here / "ble_survey_classification_results.json").write_text(
        json.dumps(report, indent=2) + "\n")
    print(f"source_sha256={source_sha256}")
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
