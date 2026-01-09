# DDS Data Tracing Demo

A demonstration project showing distributed tracing across DDS (Data Distribution Service) messages using Eclipse CycloneDDS and OpenTelemetry.

## Overview

This project demonstrates how to implement end-to-end distributed tracing in a DDS-based pub-sub system. Trace context (W3C Trace Context standard) is embedded directly into DDS message headers, enabling correlation of operations across multiple services.

## Architecture

The project contains two demo systems:

### 1. Combat Management System (Original)

```
┌─────────────────┐     MissionOrderTopic      ┌─────────────────┐
│  command-center │ ─────────────────────────► │   recon-unit    │
│  (Root Spans)   │                            │  (Child Spans)  │
└─────────────────┘                            └────────┬────────┘
                                                        │
                                               ReconReportTopic
                                                        │
                                                        ▼
┌─────────────────┐     SupplyUpdateTopic      ┌─────────────────┐
│ tactical-display│ ◄───────────────────────── │ logistics-depot │
│   (Monitoring)  │                            │  (Child Spans)  │
└─────────────────┘                            └─────────────────┘
```

### 2. Track Fusion System (Fan-In Tracing Demo)

Demonstrates **fan-in trace correlation** where multiple independent traces converge into a single fused trace:

```
┌──────────────┐
│ radar-sensor │──── trace AAA ────┐
│  (RADAR-1)   │                   │
└──────────────┘                   │
                                   │
┌──────────────┐                   │      ┌──────────────┐      ┌────────────────┐
│  esm-sensor  │──── trace BBB ────┼─────►│ track-fusion │─────►│ track-consumer │
│   (ESM-2)    │                   │      │ (trace TTT)  │      │  (child span)  │
└──────────────┘                   │      └──────────────┘      └────────────────┘
                                   │             │
┌──────────────┐                   │             │ Creates NEW trace with
│ optik-sensor │──── trace CCC ────┘             │ links to source traces
│  (OPTIK-3)   │                                 │
└──────────────┘                                 ▼
                                          
                                    Jaeger shows:
                                    ├── fuse-tracks (root)
                                    │   ├── receive-RADAR [link→AAA]
                                    │   ├── receive-ESM [link→BBB]
                                    │   ├── receive-OPTIK [link→CCC]
                                    │   ├── correlate
                                    │   └── publish-tactical
                                    │       └── emit-tactical-track
                                    └── process-tactical (consumer)
```

**Key Concept:** When multiple sensors detect the same target, their independent traces are **linked** (not parented) to a new fusion trace. This preserves sensor autonomy while enabling end-to-end visibility.

## Services

### Combat Management Services

| Service | Role | DDS Topics |
|---------|------|------------|
| **command-center** | Issues mission orders, creates root spans | Publishes: `MissionOrderTopic` |
| **recon-unit** | Executes reconnaissance missions | Subscribes: `MissionOrderTopic`, Publishes: `ReconReportTopic` |
| **logistics-depot** | Manages supply dispatching | Subscribes: `ReconReportTopic`, Publishes: `SupplyUpdateTopic` |
| **tactical-display** | Monitors all operations | Subscribes: All topics |

### Track Fusion Services

| Service | Role | DDS Topics |
|---------|------|------------|
| **radar-sensor** | Publishes radar detections | Publishes: `SourceTrackTopic` |
| **esm-sensor** | Publishes ESM detections | Publishes: `SourceTrackTopic` |
| **optik-sensor** | Publishes optical detections | Publishes: `SourceTrackTopic` |
| **track-fusion** | Fuses source tracks into tactical tracks | Subscribes: `SourceTrackTopic`, Publishes: `TacticalTrackTopic` |
| **track-consumer** | Consumes tactical tracks | Subscribes: `TacticalTrackTopic` |

## Project Structure

```
dds-data-tracing/
├── docker-compose.yml          # Service orchestration
├── Dockerfile                  # Multi-stage build
├── Makefile                    # Build shortcuts
├── include/
│   └── traced_dds.hpp          # Tracing middleware library
├── shared/
│   ├── CombatMessages.idl      # DDS message definitions
│   └── cyclonedds.xml          # CycloneDDS configuration
└── services/
    ├── command-center/         # Mission order issuer
    ├── recon-unit/             # Reconnaissance processor
    ├── logistics-depot/        # Supply management
    ├── tactical-display/       # Central monitoring
    ├── radar-sensor/           # Radar track source
    ├── esm-sensor/             # ESM track source
    ├── optik-sensor/           # Optical track source
    ├── track-fusion/           # Multi-sensor fusion
    └── track-consumer/         # Tactical track consumer
```

## How Tracing Works

### 1. Trace Context in DDS Messages

Each DDS message includes a `TraceContext` struct in its header:

```idl
struct TraceContext {
    string trace_id;        // 32 hex characters (128-bit)
    string span_id;         // 16 hex characters (64-bit)
    string parent_span_id;  // Parent span ID
    octet trace_flags;      // Sampling flag (01 = sampled)
};

struct MissionOrder {
    TraceContext trace_ctx;  // Embedded trace context
    // ... payload fields
};
```

### 2. Tracing Middleware (`traced_dds.hpp`)

The project uses a **zero-configuration** middleware library that completely automates trace context injection/extraction. Application code contains **no explicit tracing calls** - everything is handled transparently.

**Configuration via Environment Variables:**

| Variable | Description |
|----------|-------------|
| `TRACED_SERVICE_NAME` | Service name for tracing (required) |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | OTLP endpoint (default: `http://localhost:4318/v1/traces`) |

**Key Components:**

| Component | Description |
|-----------|-------------|
| `traced::Writer<T>` | DDS writer wrapper with automatic trace injection |
| `traced::Reader<T>` | DDS reader wrapper with automatic trace extraction |
| `TRACED_DDS_TYPE()` | Macro to register message types for tracing |
| `TRACED_WRITER()` | Convenience macro to create traced writers |
| `TRACED_READER()` | Convenience macro to create traced readers |

**How It Works:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         traced::Writer.write()                          │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Auto-initialize tracing on first use (from env vars)                │
│  2. Continue active trace chain OR create root span                     │
│  3. Inject trace_id/span_id into message.trace_ctx                      │
│  4. Call dds_write() with enriched message                              │
│  5. Auto-set OK status and end span                                     │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                         traced::Reader.take()                           │
├─────────────────────────────────────────────────────────────────────────┤
│  1. Auto-initialize tracing on first use (from env vars)                │
│  2. Call dds_take() to receive messages                                 │
│  3. For each message:                                                   │
│     a. Extract trace_id/span_id from message.trace_ctx                  │
│     b. Create child span with parent context                            │
│     c. Set thread-local context for automatic propagation               │
│     d. Call user callback with message and span                         │
│     e. Auto-set OK status and end span                                  │
└─────────────────────────────────────────────────────────────────────────┘
```

### 3. Usage Examples

**Complete Service Example (Zero Tracing Code!):**

```cpp
#include "traced_dds.hpp"
#include "CombatMessages.h"

// Register message types for tracing
TRACED_DDS_TYPE(combat_MissionOrder);
TRACED_DDS_TYPE(combat_ReconReport);

int main() {
    // Create DDS participant - NO tracing initialization needed!
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);

    // Create traced writer/reader
    auto writer = TRACED_WRITER(combat_MissionOrder, participant, "MissionOrderTopic");
    auto reader = TRACED_READER(combat_ReconReport, participant, "ReconReportTopic");

    // ... business logic only, no tracing code!

    dds_delete(participant);
    return 0;
}
```

**Publishing (Root Span):**

```cpp
combat_MissionOrder order;
// ... fill order fields ...

// Automatically creates span, injects trace context, publishes
writer.write(order, "issue-mission");
```

**Subscribing and Forwarding:**

```cpp
// Callback receives: message and span for optional attributes
reader.take("execute-recon", [&](combat_MissionOrder& order, traced::trace_api::Span& span) {
    // Span already created as child of incoming trace!
    span.SetAttribute("mission.type", order.mission_type);

    // Process message...

    // Forward to next service - trace context automatically propagated!
    writer.write(report, "send-report");

    // NO need to set status - middleware handles it automatically!
});
```

**Key Features:**
- **Zero initialization** - tracing auto-initializes on first Writer/Reader use
- **Zero shutdown** - cleanup handled automatically via static destructor
- **Zero status management** - OK status set automatically after callback
- **Automatic propagation** - `writer.write()` inside `reader.take()` continues the trace chain via thread-local context

### 4. Fan-In Tracing (Track Fusion)

The Track Fusion system demonstrates a more complex tracing pattern where **multiple independent traces converge**:

```cpp
// track-fusion/main.cpp - Simplified example

// 1. Collect source tracks from multiple sensors
std::vector<traced::TraceLink> links;
for (const auto& track : collected_tracks) {
    links.push_back(track.link);  // Extract trace links
}

// 2. Create NEW root span with links to all source traces
auto [fuse_span, scope] = traced::create_linked_span("fuse-tracks", links);

// 3. Create child spans for each processing phase
{
    auto [recv_span, s] = traced::create_child_span("receive-RADAR");
    // ... process radar data
    recv_span->End();
}

{
    auto [corr_span, s] = traced::create_child_span("correlate");
    // ... fusion algorithm
    corr_span->End();
}

// 4. Publish tactical track - continues the trace
writer.write(tactical_track, "emit-tactical-track");

fuse_span->End();
```

**Jaeger Visualization:**

```
track-fusion: fuse-tracks ──────────────────────────────── 20ms
│
│  Tags:
│    link.0.trace_id = "abc123..."  ← Click to jump to radar trace
│    link.0.sensor_id = "RADAR-1"
│    link.1.trace_id = "def456..."  ← Click to jump to ESM trace
│    link.1.sensor_id = "ESM-2"
│
├── receive-RADAR ────── 11μs
├── receive-ESM ──────── 8μs
├── receive-OPTIK ────── 14μs
├── correlate ────────── 10ms
└── publish-tactical ─── 1ms
    └── emit-tactical-track ── 100μs
        │
        └── track-consumer: process-tactical ── 5ms
```

### 5. Trace Flow (Linear)

```
command-center                recon-unit               logistics-depot          tactical-display
     │                             │                          │                        │
     │ issue-mission (root)        │                          │                        │
     │ trace_id: abc123...         │                          │                        │
     │ span_id: def456...          │                          │                        │
     ├────────────────────────────►│                          │                        │
     │                             │ execute-recon (child)    │                        │
     │                             │ trace_id: abc123...      │                        │
     │                             │ parent: def456...        │                        │
     │                             │ span_id: ghi789...       │                        │
     │                             ├─────────────────────────►│                        │
     │                             │                          │ dispatch-supplies      │
     │                             │                          │ trace_id: abc123...    │
     │                             │                          │ parent: ghi789...      │
     │                             │                          ├───────────────────────►│
     │                             │                          │                        │ display-*
     │                             │                          │                        │ trace_id: abc123...
```

All spans share the same `trace_id`, enabling end-to-end visibility in Jaeger.

## Quick Start

### Prerequisites

- Docker & Docker Compose
- Port 16686 available (Jaeger UI)

### Run

```bash
# Build and start all services
docker compose up --build

# Or run in background
docker compose up -d --build

# View logs
docker compose logs -f
```

### View Traces

1. Open Jaeger UI: **http://localhost:16686**
2. Select a service from the dropdown (e.g., `command-center`)
3. Click **Find Traces**
4. Click on a trace to see the full span tree across all services

## Example Output

### Combat Management System

```
[ORDER] RECON | Zone: Alpha | Priority: HIGH | ID: MSN-1734112800-0 | trace: 7c469062
[RECON] Mission: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
[INTEL] TARGET CONFIRMED | Enemies: 25 | Threat: HIGH | Terrain: URBAN | trace: 7c469062
[DISPATCH] AMMO x30 -> Mission MSN-1734112800-0 | Stock: 70 | trace: 7c469062
[DISPLAY] NEW MISSION: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
```

### Track Fusion System

```
[RADAR] Track R-1 | Pos: 40.12, 33.05 | Alt: 8500m | Conf: 0.89
[ESM] Track E-1 | Pos: 40.15, 33.08 | Alt: 8200m | Conf: 0.75
[OPTIK] Track O-1 | Pos: 40.11, 33.04 | Alt: 8600m | Conf: 0.92

[COLLECT] RADAR track R-1 | Pos: 40.12, 33.05
[COLLECT] ESM track E-1 | Pos: 40.15, 33.08
[COLLECT] OPTIK track O-1 | Pos: 40.11, 33.04

[FUSION] ══════════════════════════════════════════
[FUSION] Tactical Track: TT-001
[FUSION] Sources: RADAR-1,ESM-2,OPTIK-3
[FUSION] Position: 40.1267, 33.0567 | Alt: 8433m
[FUSION] Classification: HOSTILE | Confidence: 0.92
[FUSION] ══════════════════════════════════════════

[CONSUMER] ════════════════════════════════════════
[CONSUMER] Received Tactical Track: TT-001
[CONSUMER] From sources: RADAR-1,ESM-2,OPTIK-3
[CONSUMER] Position: 40.1267, 33.0567 | Alt: 8433m
[CONSUMER] Classification: HOSTILE | Confidence: 0.92
[CONSUMER] ════════════════════════════════════════
```

Notice how all log lines share the same trace prefix, indicating they belong to the same distributed trace.

## Technology Stack

- **DDS**: Eclipse CycloneDDS
- **Tracing**: OpenTelemetry C++ SDK
- **Trace Backend**: Jaeger (OTLP HTTP)
- **Language**: C++17
- **Build**: CMake, Docker multi-stage builds

## Configuration

### CycloneDDS (`shared/cyclonedds.xml`)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<CycloneDDS>
  <Domain id="any">
    <General>
      <NetworkInterfaceAddress>auto</NetworkInterfaceAddress>
    </General>
  </Domain>
</CycloneDDS>
```

### OpenTelemetry Exporter

Configured via environment variables in `docker-compose.yml`:

```yaml
environment:
  - TRACED_SERVICE_NAME=my-service
  - OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318/v1/traces
```

## Cleanup

```bash
docker compose down
docker compose down --volumes --rmi all  # Full cleanup
```