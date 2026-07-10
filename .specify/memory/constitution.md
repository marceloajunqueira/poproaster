# Pop Roaster Constitution

## Core Principles

### I. Safety-First, Non-Negotiable
Every command that can affect the heater or fan (from display, web, or Artisan) MUST pass through a single, centralized Safety Manager before being applied — no UI, network client, or external integration may bypass it. Hard-fail rules (minimum ventilation floor, absolute temperature cutoff, sensor-failure cutoff, indirect fan-failure detection, maximum roast duration watchdog, dedicated Emergency Stop) are always enforced regardless of operating mode. Critical alarms MUST require explicit manual acknowledgment before the roast can continue. Any relaxation of a safety rule requires an explicit, documented decision (via `/speckit.clarify`) and must be recorded as a residual risk if accepted, never silently assumed.

### II. Domain/Interface Separation
Business logic (roast state machine, safety rules, profile curve following, RoR/DTR calculation) MUST NOT depend on any specific display, web framework, or external protocol. All hardware-specific details (pins, display driver, touch controller) are isolated behind a `board_config` contract so new boards/displays can be added by implementing that contract, without modifying `roast_core/`, `safety/`, or business rules elsewhere.

### III. Configuration Over Hardcoding
Hardware wiring that can vary between physical builds (external peripheral GPIOs for sensors/actuators wired via expansion connectors) MUST be configurable (e.g., via Kconfig/menuconfig) rather than hardcoded, so the same firmware source supports rewiring without code changes. Fixed, board-soldered hardware (the display panel and its touch controller) is defined once per board profile and treated as immutable for that board.

### IV. Data Integrity and Continuity
User-created data (roast profiles, roast session history, calibration) MUST survive power loss, reboots, and — where explicitly decided — deletion of related records (e.g., a session keeps its own snapshot of the profile used, independent of the profile's later deletion). Firmware updates MUST use a safe scheme (A/B partitions) where failure never leaves the device in a non-functional state. Users MUST be able to export/import their own data (profiles, sessions) independent of the device.

### V. Spec-Driven Development
All functional ambiguity MUST be resolved through the `/speckit.clarify` workflow and recorded in `spec.md` before or during planning — decisions are not to be assumed silently by whoever implements them. The standard flow for this project is: `/speckit.specify` → `/speckit.clarify` → `/speckit.plan` → `/speckit.tasks` → `/speckit.analyze` → `/speckit.implement`. Significant scope changes discovered mid-flow (e.g., new hardware facts, new safety considerations) require re-running the affected steps rather than patching artifacts inconsistently.

## Security & Trust Boundaries

The v1 scope explicitly trusts the local network the device is connected to: there is no authentication for the web interface, control commands, or firmware upload. This is a deliberate, documented trade-off for a single-user, home/workshop-network context — not an oversight. Any future work that exposes the device beyond a trusted local network (e.g., remote/cloud access) MUST revisit this principle explicitly via `/speckit.clarify` before being implemented, since the current design assumes physical/network isolation as the only access control.

Known accepted residual risks (documented in `spec.md` Assumptions) MUST remain visible there and MUST NOT be quietly removed without an explicit decision to mitigate or formally re-accept them.

## Development Workflow

- Every feature starts with a specification (`spec.md`) covering user stories, functional requirements, success criteria, and edge cases before any implementation planning begins.
- `/speckit.analyze` MUST be run after `/speckit.tasks` and before `/speckit.implement` to catch coverage gaps, inconsistencies, and drift between spec/plan/tasks; CRITICAL findings block implementation, MEDIUM/LOW findings are remediated or explicitly accepted by the user.
- Hardware facts (pinouts, chip models, module variants) MUST be grounded in verifiable sources (vendor documentation, component registries, physically confirmed pin tests) before being written into `research.md`/`plan.md`/board configuration files — assumptions must be clearly labeled as such until confirmed.
- Tests are optional per feature and only added when explicitly requested; when omitted, the `quickstart.md` scenarios serve as the acceptance validation pass before considering a feature done.

## Governance

This constitution supersedes ad-hoc practices for this project. Amendments require an explicit update to this file (via `/speckit.constitution` or direct edit) with a version bump and a note of what changed and why. All specs, plans, and tasks for this project should be checked against these principles during `/speckit.analyze`; any conflict is a CRITICAL finding that must be resolved by adjusting the spec/plan/tasks — not by silently ignoring the principle.

**Version**: 1.0.0 | **Ratified**: 2026-07-06 | **Last Amended**: 2026-07-06

