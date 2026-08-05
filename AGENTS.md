## Project Structure & Module Organization

This workspace is a Zephyr/west firmware tree for the IMEC clicker, anchor, and gateway system. Project-specific code lives under `firmware/`; imported SDKs and platform dependencies live in `zephyr/`, `nrf/`, `modules/`, `nrfxlib/`, `bootloader/`, and `dwm3000 examples and sdk/`.

- `firmware/include/`: shared protocol, report, route, status, survey, and UWB headers.
- `firmware/src/`: platform-independent C modules with native unit tests.
- `firmware/app/`: Zephyr application, board overlay, DWM3000 port, role-specific runtime.
- `firmware/tests/`: native C tests.
- `Documentation/`: architecture, protocols, implementation task list.

I like ambitious ideas, simple systems, and software that feels obvious. Do not preserve complexity just because it already exists. Do not introduce machinery because it looks architecturally impressive. Understand the real constraint, then fight for the smallest model that makes the correct behavior unsurprising.

Channel both "measure twice, cut once" and "yagni". Fight scope creep. Try to honor the dev's intent in both a minimal and realistic fashion.

The rest of this document is meant to help you navigate the codebase and make changes effectively. Think of these instructions less as "hard rules", more as "good defaults". The developer's preferences should be able to override anything here.

## Build, Test, and Development Commands

Use the local uv-managed Python environment:

```sh
UV_CACHE_DIR=$PWD/.uv-cache uv venv --clear .venv
UV_CACHE_DIR=$PWD/.uv-cache uv pip install --python .venv/bin/python -r zephyr/scripts/requirements.txt -r nrf/scripts/requirements.txt
```

Native library tests:

```sh
cmake -S firmware -B firmware/build
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure
```

## Firmware Lines and Role Meaning

The connected-routing mesh line is the production successor and will become the main firmware after the migration is complete. Treat its role presets as the default target for new product behavior even while their names still carry the `mesh_` prefix:

- `mesh_clicker`: normal battery clicker behavior. It sleeps normally, wakes for a physical click/range sequence, and uses the connected-routing mesh path for delivery. It is not a continuously active test transmitter.
- `mesh_anchor`: the single connected-routing anchor image for every production anchor. It derives a stable node ID from the nRF FICR hardware identity; logical discovery/ranging order is assigned by the gateway and persisted. An anchor ranges local clicks, relays mesh work, and prioritizes its own click reports over transit traffic.
- `mesh_gateway`: gateway role for the same connected-routing firmware, including gateway BLE ingress/egress and highest-priority gateway commands.

The remaining build lines are not alternative production architectures:

- `mesh_transmitter` and `mesh_transmitter_forcedhop` are synthetic trafficgenerators used to load and regression-test the production-successor mesh path. They must not be treated as deployable anchor firmware.
- `ml_clicker` and `ml_anchor_1` through `ml_anchor_8` are demo/data-collection images for gathering training and validation data for a distance-offset compensation model. You do not need to take them into account in tests or validation unless its specifically mentioned by the user.

## Flashing and Monitoring

Direct `west flash` is not a deployment path for `mesh_clicker`, `mesh_anchor`, or `mesh_gateway`. The only supported deployment path is the repository-owned verified wrapper below:

```sh
.venv/bin/python firmware/scripts/flash_verified_mesh.py \
  --build-dir build/mesh-anchor \
  --hardware-manifest logs/stack-evidence/mesh-anchor-<capture-id>.json \
  --probe-id E46070D247233537
```

```sh
.venv/bin/west flash --runner pyocd --build-dir build/mesh-transmitter-forcedhop -- --frequency 4000000
```

With more than one probe attached, pass the probe ID to west after the runner separator: `-- --dev-id <probe-id> --frequency 4000000`. The shorter `-u <probe-id>` form belongs to direct `pyocd` commands such as RTT and must not be used as a west-flash argument.

For the (outdated) deterministic ML anchors, replace `build/ml-anchor-1` with the anchor image being programmed, such as `build/ml-anchor-2` through `build/ml-anchor-8`.

Flashing at 4MHz has been proven to work, if a flash fails, assume the cabling is at fault and do not reduce flash speed.

When capturing RTT logs for startup or boot behavior, use pyOCD's `pre-reset` connect mode so the capture includes reset-time output, for example `pyocd rtt -t nrf52833 -M pre-reset -u <probe-id>`. `pyocd rtt` needs a TTY; run it interactively or under `script`, and do not redirect its stdout directly to a file because that can fail with `Inappropriate ioctl for device`.

Hardware RTT and flash commands must run with direct USB device access. A sandboxed command can misleadingly show both probes in `pyocd list` yet leave `pyocd rtt` waiting forever for the same probe ID because the RTT subprocess cannot access the USB device. If that exact mismatch occurs, rerun the hardware command with full host/USB permissions.

## Preventing bugs

**Mandatory pre-step**: Before starting any new work, adding code, refactoring, performing file operations, structural changes, or build modifications, read the entire `AGENT_KNOWN_ISSUES_SUMMARY.md`; it is the recommended first pass and intentionally weights contemporary problems more heavily. It is strictly a bug log: its entries are historical evidence, not requirements. Search or review the full append-only `AGENT_KNOWN_ISSUES.md` for similar subsystem-specific history when relevant. Add every new annoying tool behavior, escaped bug, or corrected root cause as a one-line entry to the full file, never to the summary; update the summary only as a deliberate curation task. Historical entries do not prove that a bug is still present, so verify current code and tests before relying on them.

**Project context for agents**: This is a research experience-sampling system for correlating subjective user clicks with environmental sensors in a real office testbed. A major new capability is automated anchor self-setup (solving network geometry from anchor-to-anchor distances), the solving part is not part of the firmware, but a separate well-working project with known and tested input requirements. The single most important property is **robustness** — the system must not stall, lose data, or return incorrect results under any circumstances, including multi-month operation. See the Documentation folder for core requirements and the narrative behind them. You should not edit the files in this folder unless asked. You are allowed to deviate from the requirements in small ways as long as user permission is aquired for each change. Read the narrative and requirements before starting any core work beyond small patches. You are not responsible for every single requirement. For example, 20cm location inaccuracy is inherent to the design, and is not something you should worry about; those tradeoffs have already been made. The same goes for anonymity and showing the question.

## Long Term Stable Development

You are expected to **autonomously monitor** code size and complexity during your work and proactively suggest refactors or splits when files are becoming unwieldy — even if the user has not asked. This is especially important for stubborn issues: think about whether a refactor or rewrite could solve the issue in a more durable way than a patch.

Prioritize small, focused modules with clear ownership. It is smart to keep some parts of the implementation split in such a way that editing one should not interfere with the other, this goes for constants, but also for some protocols. For example: the custom mesh communication protocol should be known to be working well and independent, so that a bug hunt can largely assume this module works.

For timing, radio state, routing, queues, packet capacity, or success/failure accounting, add a worst-case test or build-time guard before relying on the path. Keep hardware assumptions aligned across code.

Treat `firmware/tests/mesh_integration/` as a mandatory pre-flash gate for the production-candidate mesh line. Changes to routing, channel scheduling, click priority, retries, BLE transport, watchdog behavior, radio sleep/wake, SPI timing, airtime, or stack budgets must run both the focused native test and:

```sh
ctest --test-dir firmware/build -L mesh_integration --output-on-failure
ctest --test-dir firmware/build -L hardware_models --output-on-failure
```

The simulator must preserve the hardware constraints it models: a UWB frame is decodable only when its complete airtime is contained in a matching RX window;
partial overlap is a timeout or decode failure; overlapping transmissions collide;
SPI, BLE credit, watchdog, and stack-budget delays are not zero-time.
Unsupported relay actions, capacity exhaustion, malformed frames, and missing routes must fail the scenario explicitly.
Do not add direct-delivery or seeded route fallbacks that can hide a broken production path.

After diagnosing a firmware bug that escaped all existing tests, delegate a test task to a subagent when available. Add the regression only after the cause is understood, and prefer a broader invariant or scenario test over an assertion that reproduces only one observed trace.

Before changing mesh routing, channel 5/channel 9 scheduling, DWM3000 sleep/idle behavior in mesh roles, ACK retry, route discovery, wake-train semantics, blind flooding, or click preemption, read `Documentation/Mesh Connected Routing Contract.md`. Treat explicitly user-approved requirements there as the high-level design contract. Descriptions of the current implementation are not requirements or permission gates merely because they appear in that file. Do not push through a change that contradicts an explicit requirement without user permission; first generate a clear list of the new behavior.

For difficult DWM3000 bring-up failures, explicitly audit SPI speed transitions and sleep/wake configuration retention before assuming the protocol or RF path is at fault. Both fast/slow SPI ordering and retained sleep configuration have caused hard-to-find behavior where a path works once after reset but fails after sleep or wake.

## Writing style

Write in flowing technical prose, the way a sharp senior engineer talks in chat - direct, conversational, and confident. Not documentation, not a report, not a slide deck.

Rules:

1. **Answer exactly what was asked, at the length it deserves - err short.** A yes/no or confirmation question gets 2-4 sentences. A "which one should I pick" gets a few paragraphs. Only a genuinely multi-part design question earns a long answer. Before sending, cut any paragraph that doesn't change what the reader does next: background they didn't ask for, restating their situation back to them, generic advice ("monitor it", "measure first") they'd already know. Seven paragraphs where three would do is a style failure even if every paragraph is well-written.
2. **Every paragraph and every bullet carries a complete argument** - claim, mechanism, and consequence together. Never state a fact without saying why it matters in the same breath. Not "MoR increases scan cost, latency, and metadata overhead" but "MoR is cheap to write, but every read has to reconcile delete files against data files, so scans get slower and flakier until something compacts them - and now that's your problem to operate."
3. **Match the form to the content - and vary it.** A long answer whose every block has the same shape (all paragraphs, all bold-lead paragraphs, all bullets) is monotonous and hard to scan; real explanations mix forms because the content mixes kinds. Pick per part:
 - **Distinct sections or comparison axes** (cost vs ops, "how generation works" vs "conventions") -> short bold headings on their own line, like "**The API reference is generated, not hand-written**" or "**Cost:**". A multi-axis comparison in undifferentiated paragraphs is a style failure just like a fragmented list is.
 - **A genuine sequence** (pipeline stages, diagnostic steps, ranked guesses) -> a numbered list, each item opening with a short bolded lead phrase and continuing in full sentences (1-4 of them).
 - **Genuinely parallel, enumerable facts** (the four config files involved, the three limits that apply) -> a plain bullet list; items may be a single full sentence when the facts are simple, and that's fine.
 - **Reasoning, causality, narrative** -> paragraphs.
 Shortening never means flattening: when rule 1 says cut, cut sentences within the structure - don't collapse headings, lists, and sections into uniform paragraphs.
4. **Don't shred connected reasoning into bullets.** If items connect with "because"/"so"/"but", those connections are the content - write prose. And never a bolded label followed by a clipped noun phrase posing as a bullet.
5. **Open with the verdict and its central caveat in one or two plain sentences.** Not a bolded headline.
6. **Conversational but not dramatic.** Use contractions (it's, you'd, don't). Say "so" and "but", not "therefore" and "however". Never write scaffolding like "The deciding mechanism is", "It is worth noting", "Importantly". No theatrical labels or hype adjectives: no "**The poison**", "the trap", "brutally expensive", "the killer feature", "sharp edge", "absurdly cheap". State the actual problem in plain words - "this rewrites gigabytes to change megabytes" beats any dramatic framing.
 - No staccato, short dramatic sentences. Let sentences breathe with commas, dependent clauses, and ideas linked together.
 - No cheesy setup phrases that introduce a point instead of stating it. Never write "here's the thing", "here's the kicker", "the part nobody warns you about", "what nobody tells you", "the dirty secret", "the truth is", "plot twist", "the reality is", "here's what's wild". State the claim directly.
 - No contrastive "not just X, but Y" structure or its variants ("it's not just X, it's Y", "not only X but also Y"). State the point directly instead of negating one framing to elevate another.
7. **No compression.** No dropped articles, no strings of abstract nouns where one concrete mechanism explains more. Shortness comes from cutting low-value content (rule 1), never from clipping sentences.
8. **End with a bottom line only when the answer weighed a real decision.** One plain-prose sentence: the call plus the condition that would flip it. Short factual or confirmation answers just end - no formulaic closer.
