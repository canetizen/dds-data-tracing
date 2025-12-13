# DDS Data Tracing Demo

A demonstration project showing distributed tracing across DDS (Data Distribution Service) messages using Eclipse CycloneDDS and OpenTelemetry.

## Overview

This project demonstrates how to implement end-to-end distributed tracing in a DDS-based pub-sub system. Trace context (W3C Trace Context standard) is embedded directly into DDS message headers, enabling correlation of operations across multiple services.

## Architecture

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
        ▲
        │         MissionOrderTopic
        │         ReconReportTopic
        └──────── SupplyUpdateTopic ───────────────────┘
```

## Services

| Service | Role | DDS Topics |
|---------|------|------------|
| **command-center** | Issues mission orders, creates root spans | Publishes: `MissionOrderTopic` |
| **recon-unit** | Executes reconnaissance missions | Subscribes: `MissionOrderTopic`, Publishes: `ReconReportTopic` |
| **logistics-depot** | Manages supply dispatching | Subscribes: `ReconReportTopic`, Publishes: `SupplyUpdateTopic` |
| **tactical-display** | Monitors all operations | Subscribes: All topics |

## Project Structure

```
dds-data-tracing/
├── docker-compose.yml          # Service orchestration
├── Dockerfile                  # Multi-stage build (IDL gen → OTel SDK → App → Runtime)
├── Makefile                    # Build shortcuts
├── include/
│   └── traced_dds.hpp          # Tracing middleware library
├── shared/
│   ├── CombatMessages.idl      # DDS message type definitions with TraceContext
│   └── cyclonedds.xml          # CycloneDDS configuration
└── services/
    ├── command-center/         # Mission order issuer
    │   ├── main.cpp
    │   └── CMakeLists.txt
    ├── recon-unit/             # Reconnaissance processor
    │   ├── main.cpp
    │   └── CMakeLists.txt
    ├── logistics-depot/        # Supply management
    │   ├── main.cpp
    │   └── CMakeLists.txt
    └── tactical-display/       # Central monitoring
        ├── main.cpp
        └── CMakeLists.txt
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

### 4. Trace Flow

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

```
[ORDER] RECON | Zone: Alpha | Priority: HIGH | ID: MSN-1734112800-0 | trace: 7c469062
[RECON] Mission: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
[INTEL] TARGET CONFIRMED | Enemies: 25 | Threat: HIGH | Terrain: URBAN | trace: 7c469062
[DISPATCH] AMMO x30 -> Mission MSN-1734112800-0 | Stock: 70 | trace: 7c469062
[DISPLAY] NEW MISSION: RECON | Zone: Alpha | Priority: HIGH | trace: 7c469062
```

Notice how all log lines share the same trace prefix (`7c469062`), indicating they belong to the same distributed trace.

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